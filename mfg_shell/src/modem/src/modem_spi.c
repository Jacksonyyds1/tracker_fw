// #include <zephyr.h>
#include "modem.h"
#include "nrf.h"
#include <nrfx_spim.h>
#include <nrfx_gpiote.h>
#include <drivers/nrfx_errors.h>
// #include <zephyr/printk.h>
#include <string.h>
#include <zephyr/kernel.h>
#include "nrfx_spis.h"
#include "nrfx_gpiote.h"
#include <zephyr/sys/printk.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>

static const struct gpio_dt_spec spi4cs = GPIO_DT_SPEC_GET(DT_NODELABEL(spi4cs), gpios);

LOG_MODULE_REGISTER(spi_modem, LOG_LEVEL_DBG);

#define SPI_INSTANCE 4
static const nrfx_spim_t spim = NRFX_SPIM_INSTANCE(SPI_INSTANCE);

#define APP_SPIM_CS_PIN   (2)
#define APP_SPIM_SCK_PIN  (8)
#define APP_SPIM_MISO_PIN (10)
#define APP_SPIM_MOSI_PIN (9)

#define NRFX_CUSTOM_ERROR_CODES 0 // used in nrfx_errors.h

#define MAX_MODEM_HANDLES 32
typedef struct {
	bool handle_used;
	uint8_t *data;
	uint16_t dataLen;
} modem_handle_t;
modem_handle_t modem_handles[MAX_MODEM_HANDLES];

K_MUTEX_DEFINE(spi_mutex);
K_MUTEX_DEFINE(spi_reply_mutex);

static nrfx_spim_config_t spim_config = NRFX_SPIM_DEFAULT_CONFIG(
	APP_SPIM_SCK_PIN, APP_SPIM_MOSI_PIN, APP_SPIM_MISO_PIN, APP_SPIM_CS_PIN);

#define SPIM_RX_BUFF_SIZE 2048
static uint8_t m_rx_buf[SPIM_RX_BUFF_SIZE];

static volatile bool
	spim_xfer_done; /**< Flag used to indicate that SPIM instance completed the transfer. */
static uint8_t *spim_rx_buff_ptr; // pointer to rx buffer
static uint8_t *spim_rx_buff;     // pointer to rx buffer
typedef void (*spim_on_rx_cb_t)(uint8_t *data, size_t len, void *user_data);
spim_on_rx_cb_t spim_on_rx_cb = NULL;
void *spim_on_rx_cb_userData = NULL;
static uint8_t *spim_tx_buff; // pointer to tx buffer

///////////////////////////////
///
///     manual_isr_setup
///
static void manual_isr_setup()
{
	IRQ_DIRECT_CONNECT(SPIM4_IRQn, 0, nrfx_spim_4_irq_handler, 0);
	irq_enable(SPIM4_IRQn);
}

///////////////////////////////
///
///     spim_recv_action_work_handler
///
void spim_recv_action_work_handler(struct k_work *work)
{
	message_command_v1_t *cmd = (message_command_v1_t *)m_rx_buf;

	if (cmd->messageHandle == 255) {
		return; // handle for NO_OP, theres nothing to do here.
	}

	uint16_t dataLen = cmd->dataLen; //(m_rx_buf[3] << 8) + (m_rx_buf[4]) + 6;
	if (m_rx_buf[0] == 0xcc && m_rx_buf[1] == 0xcc && m_rx_buf[1] == 0xcc) {
		// commented because it breaks the passthru shell to print this all the time.  It's
		// OK to happen
		// LOG_ERR("spim_recv_action_work_handler: SPIS busy or ignoring - 0xcc");
		return;
	}
	if (m_rx_buf[0] == 0xfe && m_rx_buf[1] == 0xfe && m_rx_buf[1] == 0xfe) {
		LOG_ERR("spim_recv_action_work_handler: SPIS overread - 0xfe");
		return;
	}
	if (dataLen > SPIM_RX_BUFF_SIZE) {
		LOG_ERR("spim_recv_action_work_handler: SPIS overread - dataLen > "
			"SPIM_RX_BUFF_SIZE");
		return;
	}

	if (spim_on_rx_cb) {
		spim_on_rx_cb(m_rx_buf, dataLen, spim_on_rx_cb_userData);
	}

	if (cmd->messageHandle >= MAX_MODEM_HANDLES) {
		return;
	}
	int mutex_ret = k_mutex_lock(&spi_reply_mutex, K_MSEC(1));
	if (mutex_ret != 0) {
		LOG_ERR("spim mutex lock failed try again\n");
		return;
	}

	for (int i = 0; i < MAX_MODEM_HANDLES; i++) {
		if (modem_handles[i].handle_used && (i == cmd->messageHandle)) {
			modem_handles[i].data = malloc(dataLen);
			memcpy(modem_handles[i].data, m_rx_buf + (sizeof(message_command_v1_t)),
			       dataLen);
			modem_handles[i].dataLen = dataLen;
			break;
		}
	}
	k_mutex_unlock(&spi_reply_mutex);
}
/* Register the work handler */
K_WORK_DEFINE(spim_recv_action_work, spim_recv_action_work_handler);

///////////////////////////////
///
///     spim_event_handler
///
void spim_event_handler(nrfx_spim_evt_t const *p_event, void *p_context)
{
	if (p_event->type == NRFX_SPIM_EVENT_DONE) {
		spim_xfer_done = true;
		if (spim_tx_buff) {
			free(spim_tx_buff);
			spim_tx_buff = NULL;
		}
	}
	gpio_pin_set_dt(&spi4cs, 1);
	if (m_rx_buf[0] != 0xff) {
		k_work_submit(&spim_recv_action_work);
	}
}
///////////////////////////////
///
///     modem_spi_set_rx_cb
///
void modem_spi_set_rx_cb(void (*cb)(uint8_t *data, size_t len, void *user_data), void *user_data)
{
	spim_on_rx_cb = cb;
	spim_on_rx_cb_userData = user_data;
}

void spi_rx_app_cb(uint8_t *data, size_t len, void *user_data)
{
	// LOG_ERR("Got %d bytes back\n",len);
}
///////////////////////////////
///
///     modem_spi_init
///
int modem_spi_init(void)
{
	LOG_DBG("SPIM setup");
	spim_rx_buff = NULL;
	spim_rx_buff_ptr = NULL;

	int ret = gpio_pin_configure_dt(&spi4cs, GPIO_OUTPUT);
	gpio_pin_set_dt(&spi4cs, 1);

	if (ret != 0) {
		LOG_ERR("Error %d: failed to configure %s pin %d\n", ret, spi4cs.port->name,
			spi4cs.pin);
		return -1;
	}

	spim_xfer_done = true;

	spim_config.frequency = NRFX_MHZ_TO_HZ(4);

	if (NRFX_SUCCESS != nrfx_spim_init(&spim, &spim_config, spim_event_handler, NULL)) {
		LOG_ERR("Init Failed\n");
		return 0;
	}
	for (int i = 0; i < MAX_MODEM_HANDLES; i++) {
		modem_handles[i].data = NULL;
		modem_handles[i].dataLen = 0;
	}

	modem_spi_set_rx_cb(spi_rx_app_cb, NULL);
	manual_isr_setup();

	return 0;
}

///////////////////////////////
///
///     modem_spi_send
///
int modem_spi_send(uint8_t *buf, uint16_t len, uint8_t *buf2, uint8_t recur_cnt)
{
	if (recur_cnt > 5) {
		LOG_ERR("modem_spi_send: recur_cnt > 3");
		return -1;
	}
	memset(m_rx_buf, 0, SPIM_RX_BUFF_SIZE);

	gpio_pin_set_dt(&spi4cs, 0);
	k_sleep(K_USEC(20));

	spim_xfer_done = false;
	nrfx_spim_xfer_desc_t xfer_desc = NRFX_SPIM_XFER_TRX(buf, len, m_rx_buf, SPIM_RX_BUFF_SIZE);

	nrfx_err_t err_code = nrfx_spim_xfer(&spim, &xfer_desc, 0);
	if (m_rx_buf[0] == 0xcc && m_rx_buf[1] == 0xcc && m_rx_buf[1] == 0xcc) {
		k_sleep(K_MSEC(100));
		return modem_spi_send(buf, len, buf2, recur_cnt++);
	}
	if (err_code == NRFX_ERROR_BUSY) {
		LOG_ERR("SPI busy\n");
		return err_code;
	} else if (err_code != NRFX_SUCCESS) {
		LOG_ERR("Error code = %d\n", err_code);
		return err_code;
	}

	if (buf2 != NULL) {
		memcpy(buf2, m_rx_buf, len);
	}

	return 0;
}

///////////////////////////////
///
///     modem_spi_recv
///
int modem_spi_recv(char *buf, k_timeout_t timeout)
{
	return 0;
}

///////////////////////////////
///
///     get_next_handle_id
///
int get_next_handle_id()
{
	for (int i = 1; i < MAX_MODEM_HANDLES; i++) {
		if (modem_handles[i].handle_used == false) {
			modem_handles[i].handle_used = true;
			return i;
		}
	}
	return -1;
}

///////////////////////////////
///
///     clear_handle_id
///
int modem_spi_free_reply_data(int handle)
{
	if (handle < MAX_MODEM_HANDLES) {
		modem_handles[handle].handle_used = false;
		if (modem_handles[handle].data != NULL) {
			free(modem_handles[handle].data);
			modem_handles[handle].data = NULL;
		}
		modem_handles[handle].dataLen = 0;
		return 0;
	}
	return -1;
}

///////////////////////////////
///
///     modem_spi_send_command
///
int modem_spi_send_command(modem_message_type_t type, uint8_t *data, uint16_t dataLen,
			   bool reply_requested)
{
	uint8_t ret = 0;
	int newHandle = 255;
	if (!spim_xfer_done) {
		return -1;
	}
	int mutex_ret = k_mutex_lock(&spi_mutex, K_NO_WAIT);
	if (mutex_ret != 0) {
		LOG_ERR("spim mutex lock failed try again");
		return -1;
	}

	// TODO get next available handle and assign to this command, and return it
	if (reply_requested) {
		newHandle = get_next_handle_id();
		if (newHandle < 0) {
			LOG_ERR("no available handles");
			return -1;
		}
	}

	uint8_t *spim_tx_buff = malloc(dataLen + sizeof(message_command_v1_t));
	if (!spim_tx_buff) {
		LOG_ERR("malloc failed SMR!!");
	}
	message_command_v1_t *cmd = (message_command_v1_t *)spim_tx_buff;

	cmd->version = 0x01;
	cmd->messageType = type;
	cmd->messageHandle = newHandle;
	cmd->dataLen = dataLen;

	for (int i = 0; i < dataLen; i++) {
		spim_tx_buff[i + sizeof(message_command_v1_t)] = data[i];
	}
	ret = modem_spi_send(spim_tx_buff, dataLen + sizeof(message_command_v1_t), NULL, 0);
	if (ret != 0) {
		LOG_ERR("modem_spi_send failed");
		return -1;
	}

	k_mutex_unlock(&spi_mutex);
	return newHandle;
}

int modem_spi_recv_resp(uint8_t handle, uint8_t *data, uint16_t *dataLen, int timeout)
{

	k_sleep(K_MSEC(50));

	// check if response to handle is ready
	int mutex_ret = k_mutex_lock(&spi_reply_mutex, K_MSEC(timeout));
	if (mutex_ret != 0) {
		LOG_ERR("spim mutex lock failed try again\n");
		return -1;
	}

	if (modem_handles[handle].data != NULL) {
		memcpy(data, modem_handles[handle].data, modem_handles[handle].dataLen);
		*dataLen = modem_handles[handle].dataLen;
		k_mutex_unlock(&spi_reply_mutex);
		return 0;
	}
	k_mutex_unlock(&spi_reply_mutex);

	if (timeout > 0) {
		uint32_t startTime = k_uptime_get();
		while ((k_uptime_get() - startTime) < timeout) {
			// printf("\n Try to get data \n");
			char no_op_data[2];

			// send empty spi tx
			modem_spi_send_command(MESSAGE_TYPE_NO_OP, no_op_data, 2, false);

			// check response
			k_mutex_lock(&spi_reply_mutex, K_FOREVER);
			if (modem_handles[handle].data != NULL) {
				memcpy(data, modem_handles[handle].data,
				       modem_handles[handle].dataLen);
				*dataLen = modem_handles[handle].dataLen;
				k_mutex_unlock(&spi_reply_mutex);
				return 0;
			}
			k_mutex_unlock(&spi_reply_mutex);

			// pause briefly?
			k_sleep(K_MSEC(50));
		}
	}
	// return -1 on invalid handle
	return -1;
}

// Example "get one value from the 9160" function
static int modem_spi_get_one_value(char *cmd, uint8_t *buf)
{
	int ret = 0;

	// modem_send_command, takes cmd type, in this case AT, a cmd buffer, and a len.
	// it also takes a bool that says whether we care about the reply.
	// If you dont care, it doent setup the handle/reply system for this and just sends it.
	// Since you care in this case, it will return a handle that you can use to get the reply
	// later.
	int my_handle = modem_send_command(MESSAGE_TYPE_AT, cmd, strlen(cmd), true);
	uint16_t len = 16;

	// now we ask for the response, if we didnt say true above, this would return -1
	// if you specify 0 as the timeout it will return immediately and you can call this in a
	// loop perhaps.
	if (modem_recv_resp(my_handle, buf, &len, 10000) ==
	    0) { // 10 sec timeout, thats a looooong time
		// Modem returned valid response
		ret = 0;
	} else {
		// Modem response timed out
		ret = -1;
	}
	buf[len] = '\0';

	// due to the nature of the buffers returned and being all kinds of different sizes, this
	// return is malloc'd so you MUST free it when you are done with it.
	modem_free_reply_data(my_handle);
	return ret;
}
int modem_spi_get_IMEI(uint8_t *buf)
{
	char *cmd = "AT+CGSN\0";
	return (modem_spi_get_one_value(cmd, buf));
}
int modem_spi_get_ICCID(uint8_t *buf)
{
	char *cmd = "AT\%XICCID\0";
	modem_send_command(MESSAGE_TYPE_AT, "AT+CFUN=1\0", strlen("AT+CFUN=1\0"), false);
	k_sleep(K_MSEC(1000));
	int ret = modem_spi_get_one_value(cmd, buf);
	k_sleep(K_MSEC(500));
	modem_send_command(MESSAGE_TYPE_AT, "AT+CFUN=0\0", strlen("AT+CFUN=0\0"), false);
	// k_sleep(K_MSEC(300));
	return ret;
}
int modem_spi_get_VERSION(uint8_t *buf)
{
	char *cmd = "AT+CGMR\0";
	return (modem_spi_get_one_value(cmd, buf));
}
