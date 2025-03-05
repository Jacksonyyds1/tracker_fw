#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <stdint.h>
#include <string.h>
#include "wifi_spi.h"

#if (!CONFIG_USE_UART_TO_DA16200)
#if DT_NODE_HAS_STATUS(DT_NODELABEL(uart2), okay)
#error "SPI code compiling with UART enabled"
#endif

LOG_MODULE_REGISTER(wifi_spi, CONFIG_WIFI_SPI_LOG_LEVEL);
bool wifi_print_buffers = false;

// A queue to stores wifi_msg_t elements.  The data buffer
// should be k_heap_free()d when done with the message
K_MSGQ_DEFINE(wifi_msgq, sizeof(wifi_msg_t), 30, 4);

K_HEAP_DEFINE(wifi_heap, WIFI_MSG_SIZE * 3);

wifi_on_rx_cb_t wifi_on_rx_cb = NULL;
void *wifi_on_rx_cb_userData = NULL;

// From the da16200 datasheet
#define GEN_CMD_ADDR       (0x50080254) // Address to Write Command
#define ATCMD_ADDR         (0x50080260) // Address to Send AT Command
#define RESP_ADDR          (0x50080258) // Address to Read Response
#define AUTO_INC_WRITE_CMD (0x80)
#define AUTO_INC_READ_CMD  (0xC0)

/* stack definition and dialog workqueue */
K_THREAD_STACK_DEFINE(dialog_stack, 2048);
static struct k_work_q da_resp_work_q;
static struct k_work da_resp_work;

static struct spi_config spi_cfg = {
	.frequency = 4000000,
	.operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_LINES_SINGLE | SPI_MODE_CPOL |
		     SPI_MODE_CPHA | SPI_TRANSFER_MSB,
	.slave = 0,
	.cs =
		{
			.gpio = GPIO_DT_SPEC_GET(DT_NODELABEL(spi2cs), gpios),
			.delay = 0,
		},
};

#define DEF_BUF_SET(_name, _buf_array)                                                             \
	const struct spi_buf_set _name = {                                                         \
		.buffers = _buf_array,                                                             \
		.count = ARRAY_SIZE(_buf_array),                                                   \
	}

struct gpio_callback spi_int_cb;

typedef struct _da_header {
	uint32_t addr_type;
	uint8_t cmd;
	uint8_t length[3];
} __attribute__((packed)) da_header_t;

typedef struct _da_rsp {
	uint32_t buf_addr;
	uint16_t length;
	uint8_t rsp;
	uint8_t dummy;
} __attribute__((packed)) da_rsp_t;

typedef struct _da_write_rqst {
	uint16_t length;
	uint8_t cmd;
	uint8_t dummy;
} __attribute__((packed)) da_write_rqst_t;

static const struct device *gpio_p1;
static const struct device *gpio_p0;
static const struct device *spidev;

void wifi_spi_set_print_txrx(bool print)
{
	wifi_print_buffers = print;
}

//////////////////////////////////////////////////////////
// dbg_print_buf()
static void dbg_print_buf(char *name, const struct spi_buf_set set)
{
	for (int ib = 0; ib < set.count; ib++) {
		LOG_DBG("%s[%d] (%08X) len = %d", name, ib, (uint32_t)(set.buffers[ib].buf),
			set.buffers[ib].len);
		if (set.buffers[ib].buf != NULL) {
			LOG_HEXDUMP_DBG(set.buffers[ib].buf, set.buffers[ib].len, " ");
		}
	}
}

//////////////////////////////////////////////////////////
// da_spi_write_rqst
// Write a at AT/ESC request to the DA16200.
//
//  @param type - command type
//  @param data - pointer to buffer containing the data to write
//  @param size - size of the data to write
static int da_spi_write_rqst(uint32_t type, char *data, uint32_t len)
{
	int err;
	char *four_byte_align_buf[4];
	uint32_t len_aligned = ((len / 4) + 1) * 4;
	uint32_t len_trucated = len_aligned - 4;
	if (len_trucated != len_aligned) {
		// The data needs to be 4 byte aligned and if it isn't the
		// data needs to be 0 terminated so the da can tell where
		// it ends. However the data ptr passed in may be on the stack
		// and so we can't just write 0s after it's end.  Since spi_write()
		// sends a array of buffers, we copy the last 1-3 bytes of the
		// data to a zero filled 4 byte buffer and send the original
		// data, less 1-3 bytes, followed by our 4 byte buffer
		memset(four_byte_align_buf, 0, 4);
		memcpy(four_byte_align_buf, &data[len_trucated], len - len_trucated);
	}

	da_header_t header = {
		.addr_type = ((type & 0xFF) << 24) + ((type & 0xFF00) << 8) +
			     ((type & 0xFF0000) >> 8) + ((type & 0xFF000000) >> 24),
		.cmd = AUTO_INC_WRITE_CMD,
		.length[0] = ((len_aligned & 0xFF0000) >> 16),
		.length[1] = ((len_aligned & 0xFF00) >> 8),
		.length[2] = len_aligned & 0xFF,
	};

	const struct spi_buf tx_buf_exact[] = {
		{
			.buf = &header,
			.len = sizeof(da_header_t),
		},
		{
			.buf = data,
			.len = len_aligned,
		},
	};
	const struct spi_buf tx_buf[] = {
		{
			.buf = &header,
			.len = sizeof(da_header_t),
		},
		{
			.buf = data,
			.len = len_trucated,
		},
		{
			.buf = four_byte_align_buf,
			.len = len_aligned - len_trucated,
		},
	};

	DEF_BUF_SET(tx, tx_buf);
	DEF_BUF_SET(txe, tx_buf_exact);

	if (len == len_aligned) {
		if (wifi_print_buffers) {
			dbg_print_buf("write tx", txe);
		}
		err = spi_write(spidev, &spi_cfg, &txe);
	} else {
		if (wifi_print_buffers) {
			dbg_print_buf("write tx", tx);
		}
		err = spi_write(spidev, &spi_cfg, &tx);
	}
	if (err) {
		LOG_DBG("Reg read failed on SPI write");
		return err;
	}
	return 0;
}

//////////////////////////////////////////////////////////
// da_spi_read_data
//
// Initiate a read from the DA16200.  This should be called
// after have gotten a response from the DA16200 that indicates
// there is data to read.  AT cmds, ESC cmds and async
// responses all have data attached and use the same
// message sequence to retreive the data.
//
//  @param rsp - pointer to response struct that tells us
//               how much data to read and where to read
//               it from
//  @param data - a pointer to a buffer to put incoming data
//                buffer needs to be 2 bytes longer then the
//                data length expected
//  @param callback - callback to call when the data is read
static int da_spi_read_data(da_rsp_t *rsp, wifi_msg_t *wifi_msg)
{
	wifi_msg->data_len = rsp->length;
	uint32_t addr = ((rsp->buf_addr & 0xFF) << 24) + ((rsp->buf_addr & 0xFF00) << 8) +
			((rsp->buf_addr & 0xFF0000) >> 8) + ((rsp->buf_addr & 0xFF000000) >> 24);

	da_header_t header = {
		.addr_type = addr,
		.cmd = AUTO_INC_READ_CMD,
		.length[0] = 0x00,
		.length[1] = (rsp->length) >> 8,
		.length[2] = rsp->length,
	};

	const struct spi_buf tx_buf[] = {
		{
			.buf = &header,
			.len = sizeof(da_header_t),
		},
	};

	const struct spi_buf rx_buf[] = {
		{
			.buf = NULL,
			.len = sizeof(da_header_t),
		},
		{
			.buf = wifi_msg->data,
			.len = rsp->length,
		},
	};

	DEF_BUF_SET(tx, tx_buf);
	DEF_BUF_SET(rx, rx_buf);

	if (wifi_print_buffers) {
		dbg_print_buf("data tx", tx);
	}

	int err = spi_transceive(spidev, &spi_cfg, &tx, &rx);
	if (err) {
		LOG_ERR("failed on spi_transceive_cb");
		return err;
	}

	if (wifi_print_buffers) {
		dbg_print_buf("data rx", rx);
	}
	return 0;
}

//////////////////////////////////////////////////////////
// da_spi_get_response
//  get a response from DA16200.  This should be called when we have submitted
//  a write request to the DA or the data ready line activeate indicating there
//  is data to read.   AT cmds, ESC cmds and async events all use the same
//  response format
//
//  @param dev - spi device
//  @param rsp - pointer to response struct to recieve the data
static int da_spi_get_response(da_rsp_t *rsp)
{
	da_header_t header = {
		.addr_type = ((RESP_ADDR & 0xFF) << 24) + ((RESP_ADDR & 0xFF00) << 8) +
			     ((RESP_ADDR & 0xFF0000) >> 8) + ((RESP_ADDR & 0xFF000000) >> 24),
		.cmd = AUTO_INC_READ_CMD,
		.length[0] = 0x00,
		.length[1] = 0x00,
		.length[2] = 0x08,
	};

	const struct spi_buf tx_buf[] = {
		{
			.buf = &header,
			.len = sizeof(da_header_t),
		},
	};

	const struct spi_buf rx_buf[] = {{
						 .buf = NULL,
						 .len = sizeof(da_header_t),
					 },
					 {.buf = rsp, .len = sizeof(da_rsp_t)}};

	DEF_BUF_SET(tx, tx_buf);
	DEF_BUF_SET(rx, rx_buf);

	if (wifi_print_buffers) {
		dbg_print_buf("response tx", tx);
	}

	int err;
	err = spi_transceive(spidev, &spi_cfg, &tx, &rx);
	if (err) {
		LOG_ERR("failed on spi_transceive_cb");
		return err;
	}

	if (wifi_print_buffers) {
		dbg_print_buf("response rx", rx);
	}
	return 0;
}

//////////////////////////////////////////////////////////
//  da_resp_work_fn
//
//  This is the work function that is called when the response
// work object is submitted.  It is responsible for reading the
// response from the DA16200.
static void da_resp_work_fn()
{
	// Allocate memory for the response we are about to receive
	da_rsp_t rsp;
	wifi_msg_t msg;
	int ret;

	if (da_spi_get_response(&rsp) != 0) {
		goto reset_wifi_data_ready_shadow;
	}
	// rsp has the location and len of the response data

	if (rsp.length > WIFI_MSG_SIZE) {
		LOG_ERR("DA response too large");
		goto reset_wifi_data_ready_shadow;
	}

	if (rsp.buf_addr == 0xffffffff) {
		// No data to read from the DA, the result is in rsp.rsp
		// allocate memory for a response message
		rsp.length = 20;
	}

	// Allocate the memory for the response
	msg.data_len = rsp.length;
	msg.data = k_heap_alloc(&wifi_heap, rsp.length, K_NO_WAIT);
	if (msg.data == NULL) {
		LOG_ERR("No heap left for spi data");
		goto reset_wifi_data_ready_shadow;
	}
	memset(msg.data, 0, rsp.length);

	// Make or read the response
	if (rsp.buf_addr == 0xffffffff) {
		if (rsp.rsp == 0x20) {
			sprintf((char *)msg.data, "\r\nOK\r\n");
		} else {
			sprintf((char *)msg.data, "\r\nERROR:%d\r\n", (int8_t)rsp.rsp);
		}
		msg.data_len = strlen((char *)msg.data);
	} else {
		// buf_addr indicates we need to read the data from the DA
		if (wifi_print_buffers) {
			LOG_DBG("DA response length: %d", rsp.length);
			LOG_DBG("DA response addr: %X", (uint32_t)(rsp.buf_addr));
		}

		// The DA requires a 300us interval between the response and the data
		k_sleep(K_USEC(300));

		ret = da_spi_read_data(&rsp, &msg);
		if (ret != 0) {
			k_heap_free(&wifi_heap, msg.data);
			goto reset_wifi_data_ready_shadow;
		}
		// Since the transfers are always 4 byte aligned, there can
		// be up to 3 bytes of padding at the end of the data. We
		// know that the data is text so we can shorten the length
		while (msg.data[msg.data_len - 1] == 0) {
			msg.data_len--;
		}
	}

	if (wifi_on_rx_cb != NULL) {
		// if the callback has consumed the message it will return
		// true.  If so we don't place it on the message queue and
		// free the memory
		bool consumed = wifi_on_rx_cb(&msg, wifi_on_rx_cb_userData);
		if (consumed) {
			k_heap_free(&wifi_heap, msg.data);
			goto reset_wifi_data_ready_shadow;
		}
	}

	// Copy the wifi_msg_t into the msg response queue
	ret = k_msgq_put(&wifi_msgq, &msg, K_NO_WAIT);
	if (ret != 0) {
		LOG_ERR("Failed to put data on queue");
	}
reset_wifi_data_ready_shadow:
	gpio_pin_set_raw(gpio_p0, 2, 0);
}

//////////////////////////////////////////////////////////
// da_data_is_ready()
//
// This is the callback function that is called when
// the da asserts the data ready line indicating there
// is data to read.  It submits a work object to read
// the data.
static void da_data_is_ready()
{
	gpio_pin_set_raw(gpio_p0, 2, 1);
	// add a work object to the work queue to cause
	// a response to be read from the DA
	k_work_submit_to_queue(&da_resp_work_q, &da_resp_work);
}

//////////////////////////////////////////////////////////
// wifi_spi_init
//
// Initialize the spi interface to the DA16200
int wifi_spi_init(void)
{
	int ret;
	LOG_DBG("spi2 to DA setup");

	if ((spidev = device_get_binding("spi@b000")) == NULL) {
		LOG_ERR("Error: didn't find %s device", "spi2");
		return -1;
	} else {
		LOG_DBG("found spi2 device");
	}

	if (device_is_ready(spidev) == false) {
		LOG_ERR("Error: %s device not ready", "spi2");
		return -1;
	}

	gpio_p0 = device_get_binding("gpio@842500");
	if (gpio_p0 == NULL) {
		LOG_ERR("Error: didn't find %s device", "gpio@842500");
		return -1;
	}
	gpio_p1 = device_get_binding("gpio@842800");
	if (gpio_p1 == NULL) {
		LOG_ERR("Error: didn't find %s device", "gpio@842800");
		return -1;
	}

	// Mirror what is on the WIFI_DATAREADY line, which has no TP, to P0.2 which does.
	ret = gpio_pin_configure(gpio_p0, 2, GPIO_OUTPUT_ACTIVE);
	if (ret != 0) {
		LOG_ERR("Error %d: failed to configure %s pin %d", ret, "GPIO_0", 2);
	} else {
		LOG_DBG("configured WIFI_DATAREADY Mirror line");
	}

	wifi_on_rx_cb = NULL;

	/* thread and queue setup */
	k_work_queue_start(&da_resp_work_q, dialog_stack, K_THREAD_STACK_SIZEOF(dialog_stack),
			   CONFIG_SYSTEM_WORKQUEUE_PRIORITY, NULL);
	k_work_init(&da_resp_work, da_resp_work_fn);

	// Set up the data ready pin interrupt so we read responses when data is ready
	int pin = 7;
	const struct device *port = gpio_p1;
	char *pname = "P1";

	ret = gpio_pin_configure(port, pin, GPIO_INT_EDGE_BOTH);
	if (ret != 0) {
		LOG_ERR("Error %d: failed to configure %s pin %d\n", ret, pname, pin);
		return -1;
	}

	ret = gpio_pin_interrupt_configure(port, pin, GPIO_INT_EDGE_RISING);
	if (ret != 0) {
		LOG_ERR("Error %d: failed to int configure %s pin %d\n", ret, pname, pin);
		return -1;
	}

	gpio_init_callback(&spi_int_cb, da_data_is_ready, BIT(pin));
	if (ret != 0) {
		LOG_ERR("Error %d: failed to init callback %s pin %d\n", ret, pname, pin);
		return -1;
	}

	gpio_add_callback(port, &spi_int_cb);
	if (ret != 0) {
		LOG_ERR("Error %d: failed to add callback %s pin %d\n", ret, pname, pin);
		return -1;
	}

	k_busy_wait(1);

	return 0;
}

//////////////////////////////////////////////////////////
// wifi_spi_atcmd_timeout
//
// Write an AT or ESC command to the DA16200 and return
// the response.
//
// The DA should only process one at/esc cmd at a time.
// so this call waits for a OK or ERROR response.
//
// Responses, can be to a request or async events.  Some
// commands should always return some data before they
//
//  @param data - pointer to buffer containing the AT command
//  @param timeout - timeout for the write
int wifi_spi_atcmd_timeout(char *data, k_timeout_t timeout)
{
	int len = strlen(data);
	int err = da_spi_write_rqst(ATCMD_ADDR, data, len);
	if (err) {
		LOG_DBG("failed to write AT_cmd");
		return err;
	}
	// EAS XXX TODO wait for the response
	return 0;
}

//////////////////////////////////////////////////////////
// wifi_spi_send_timeout
//
// Write an AT or ESC command to the DA16200.
//
//  @param data - pointer to buffer containing the AT command
//  @param timeout - timeout for the write
//
//  @return - 0 on success, -1 on error or timeout
int wifi_spi_send_timeout(char *data, k_timeout_t timeout)
{
	int len = strlen(data);
	int err = da_spi_write_rqst(ATCMD_ADDR, data, len);
	if (err) {
		LOG_DBG("failed to write AT_cmd");
		return err;
	}
	return 0;
}

//////////////////////////////////////////////////////////
// wifi_spi_send
//
// Write an AT or ESC command to the DA16200.
//
//  @param data - pointer to buffer containing the AT command
//
//  @return - 0 on success, -1 on error or timeout
int wifi_spi_send(char *data)
{
	return wifi_spi_send_timeout(data, K_FOREVER);
}

void wifi_spi_set_rx_cb(wifi_on_rx_cb_t cb, void *user_data)
{
	wifi_on_rx_cb = cb;
	wifi_on_rx_cb_userData = user_data;
}

//////////////////////////////////////////////////////////
// wifi_spi_msg_free()
//
// Free the memory allocated for a wifi_msg_t
// @param msg - pointer to the wifi_msg_t to free
void wifi_spi_msg_free(wifi_msg_t *msg)
{
	k_heap_free(&wifi_heap, msg->data);
}

//////////////////////////////////////////////////////////
//	Receive a message from the queue,
//
// @param msg  - pointer to the wifi_msg_t structure that
//               will be filled on success
// @param timeout - timeout for the read
//
// @return - 0 on success, -1 on error or timeout
// @note caller must call wifi_msg_free() to free the
//       memory allocated for the message
int wifi_spi_recv(wifi_msg_t *msg, k_timeout_t timeout)
{
	int ret = -1;

	if (msg != NULL) {
		ret = k_msgq_get(&wifi_msgq, msg, timeout);
	}

	return ret;
}

//////////////////////////////////////////////////////////
//	Requeue a message that was recv'd
//
// @param msg  - a wifi_msg_t that was obtained
//               from wifi_spi_recv()
//
// @note caller can call this instead of wifi_msg_free()
void wifi_spi_requeue(wifi_msg_t *msg)
{
	if (msg != NULL) {
		k_msgq_put(&wifi_msgq, msg, K_NO_WAIT);
	}
}

void wifi_spi_flush_msgs()
{
	wifi_msg_t msg;
	while (k_msgq_get(&wifi_msgq, &msg, K_NO_WAIT) == 0) {
		wifi_spi_msg_free(&msg);
	}
}

int wifi_spi_msg_cnt()
{
	return k_msgq_num_used_get(&wifi_msgq);
}

#endif
