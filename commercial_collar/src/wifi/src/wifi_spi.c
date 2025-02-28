/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
/****************************************************************
 *  wifi_spi.c
 *  SPDX-License-Identifier: LicenseRef-Proprietary
 *
 * The wifi code is layered as follows:
 *     wifi_spi.c or wifi_uart.c - Talked to hardware to transfer data to and from the DA1200
 *     wifi.c - HW independent layer to send adn receive messages to and from the DA1200
 *     wifi_at.c - AT command layer to send and receive AT commands to and from the DA1200
 *     net_mgr.c - The network (wifi and lte) api layer, manages the state of the da and
 *                 lte chip and network communication state machine and publishes zbus
 *                 message when revevent stuff happens
 *     wifi_shell.c - A collection of shell commands to test and debug during development
 */
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <stdint.h>
#include <string.h>
#include "wifi_spi.h"

LOG_MODULE_REGISTER(wifi_spi, CONFIG_WIFI_SPI_LOG_LEVEL);

// From the da16200 datasheet
#define GEN_CMD_ADDR       (0x50080254)    // Address to Write Command
#define ATCMD_ADDR         (0x50080260)    // Address to Send AT Command
#define RESP_ADDR          (0x50080258)    // Address to Read Response
#define AUTO_INC_WRITE_CMD (0x80)
#define AUTO_INC_READ_CMD  (0xC0)

/* stack definition and dialog workqueue */
K_THREAD_STACK_DEFINE(dialog_stack, 2048);
static struct k_work_q da_resp_work_q;
static struct k_work   da_resp_work;

static struct spi_config spi_cfg = {
	.frequency = 4000000,
	.operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_LINES_SINGLE | SPI_TRANSFER_MSB,
	.slave = 0,
	.cs =
		{
			.gpio = GPIO_DT_SPEC_GET(DT_NODELABEL(spi2cs), gpios),
			.delay = 0,
		},
};

#define DEF_BUF_SET(_name, _buf_array)     \
    const struct spi_buf_set _name = {     \
        .buffers = _buf_array,             \
        .count   = ARRAY_SIZE(_buf_array), \
    }

struct gpio_callback spi_int_cb;

typedef struct _da_header
{
    uint32_t addr_type;
    uint8_t  cmd;
    uint8_t  length[3];
} __attribute__((packed)) da_header_t;

typedef struct _da_rsp
{
    uint32_t buf_addr;
    uint16_t length;
    uint8_t  rsp;
    uint8_t  dummy;
} __attribute__((packed)) da_rsp_t;

typedef struct _da_write_rqst
{
    uint16_t length;
    uint8_t  cmd;
    uint8_t  dummy;
} __attribute__((packed)) da_write_rqst_t;

static const struct device *gpio_p1;
static const struct device *gpio_p0;
static const struct device *spidev;

//////////////////////////////////////////////////////////
// da_spi_write_rqst
// Write a at AT/ESC request to the DA16200.
//
//  @param type - command type
//  @param data - pointer to buffer containing the data to write
//  @param size - size of the data to write
static int
da_spi_write_rqst(uint32_t type, char *data, uint32_t len)
{
    int      err;
    char    *four_byte_align_buf[4];
    uint32_t len_aligned  = ((len / 4) + 1) * 4;
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
        .addr_type =
            ((type & 0xFF) << 24) + ((type & 0xFF00) << 8) + ((type & 0xFF0000) >> 8) + ((type & 0xFF000000) >> 24),
        .cmd       = AUTO_INC_WRITE_CMD,
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
        err = spi_write(spidev, &spi_cfg, &txe);
    } else {
        err = spi_write(spidev, &spi_cfg, &tx);
    }
    if (err) {
        LOG_ERR("'%s'(%d) on SPI write", wstrerr(-err), err);
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
static int
da_spi_read_data(da_rsp_t *rsp, wifi_msg_t *wifi_msg, uint32_t buf_actual_len)
{
    wifi_msg->data_len = rsp->length;
    uint32_t addr = ((rsp->buf_addr & 0xFF) << 24) + ((rsp->buf_addr & 0xFF00) << 8) + ((rsp->buf_addr & 0xFF0000) >> 8)
                    + ((rsp->buf_addr & 0xFF000000) >> 24);

    da_header_t header = {
        .addr_type = addr,
        .cmd       = AUTO_INC_READ_CMD,
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
            .len = buf_actual_len,
        },
    };

    DEF_BUF_SET(tx, tx_buf);
    DEF_BUF_SET(rx, rx_buf);

    int err = spi_transceive(spidev, &spi_cfg, &tx, &rx);
    if (err) {
        LOG_ERR("failed on spi_transceive_cb");
        return err;
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
static int
da_spi_get_response(da_rsp_t *rsp)
{
    da_header_t header = {
        .addr_type = ((RESP_ADDR & 0xFF) << 24) + ((RESP_ADDR & 0xFF00) << 8) + ((RESP_ADDR & 0xFF0000) >> 8)
                     + ((RESP_ADDR & 0xFF000000) >> 24),
        .cmd       = AUTO_INC_READ_CMD,
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

    const struct spi_buf rx_buf[] = { {
                                          .buf = NULL,
                                          .len = sizeof(da_header_t),
                                      },
                                      { .buf = rsp, .len = sizeof(da_rsp_t) } };

    DEF_BUF_SET(tx, tx_buf);
    DEF_BUF_SET(rx, rx_buf);

    int err;
    err = spi_transceive(spidev, &spi_cfg, &tx, &rx);
    if (err) {
        LOG_ERR("failed on spi_transceive_cb");
        return err;
    }

    return 0;
}

//////////////////////////////////////////////////////////
//  da_resp_work_fn
//
//  This is the work function that is called when the response
// work object is submitted.  It is responsible for reading the
// response from the DA16200.
extern long http_amt_written;
static void
da_resp_work_fn()
{
    // Allocate memory for the response we are about to receive
    da_rsp_t   rsp;
    wifi_msg_t msg;
    int        ret;

    if (da_spi_get_response(&rsp) != 0) {
        goto reset_wifi_data_ready_shadow;
    }
    // rsp has the location and len of the response data
    if (rsp.length > WIFI_MSG_SIZE) {
        // If the power to the DA was shut off we will get FFs an the
        // size will be invalid. Only complain if the DA is on
        if (wifi_get_power_key() && wifi_get_1v8() && wifi_get_3v0()) {
            LOG_ERR("DA response too large [%d] > [%d]", rsp.length, WIFI_MSG_SIZE);
        }
        goto reset_wifi_data_ready_shadow;
    }

    if (rsp.buf_addr == 0xffffffff) {
        // No data to read from the DA, the result is in rsp.rsp
        // allocate memory for a response message
        rsp.length = 20;
    }

    // The message we get from the DA is 4 byte aligned, so we
    // need to make sure we allocate extra if the length is not
    // a multiple of 4. After running this for a while it seem
    // that the DA always sets rsp.length to be a multiple of 4.
    int alloc_len = rsp.length;
    if (rsp.length % 4 != 0) {
        alloc_len = ((rsp.length / 4) + 1) * 4;
        LOG_DBG("alloc_len %d for a %d response", alloc_len, rsp.length);
    }
    // We alloc an extra byte to guarentee null termination
    alloc_len += 1;
    ret = wifi_init_new_msg(&msg, true, alloc_len);
    if (msg.data == NULL) {
        LOG_ERR("No heap left for spi data, trying to alloc %d", alloc_len);
        goto reset_wifi_data_ready_shadow;
    }
    memset(msg.data, 0, alloc_len);
    msg.data_len = rsp.length;    // Probably not needed because data is always 4 byte aligned

    // Make or read the response
    if (rsp.buf_addr == 0xffffffff) {
        if (rsp.rsp == 0x20) {
            snprintf((char *)msg.data, msg.data_len, "\r\nOK\r\n");
        } else {
            snprintf((char *)msg.data, msg.data_len, "\r\nERROR:%d\r\n", (int8_t)rsp.rsp);
        }
        msg.data_len = strlen((char *)msg.data);
    } else {
        // buf_addr indicates we need to read the data from the DA
        // The DA requires a 300us interval between the response and the data
        k_sleep(K_USEC(300));

        // Read the message (up to but not including the null terminator)
        ret = da_spi_read_data(&rsp, &msg, alloc_len - 1);
        if (ret != 0) {
            wifi_msg_free(&msg);
            goto reset_wifi_data_ready_shadow;
        }
        if (msg.data[alloc_len - 1] != 0) {
            LOG_ERR("Response not null terminated");
        }
    }

    // Let the world know we have received a message
    if (msg.data_len == 0) {
        LOG_DBG("Incoming message len is %d, drop it!!!", msg.data_len);
        wifi_msg_free(&msg);
    } else {
        ret = wifi_notify_cbs(&msg);
        if (ret != 0) {
            LOG_ERR("failed to put on q, so freeing incoming message");
            wifi_msg_free(&msg);
        }
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
static void
da_data_is_ready()
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
int
wifi_spi_init(void)
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

    /* thread and queue setup */
    struct k_work_queue_config da_resp_work_q_cfg = {
        .name     = "da_resp_work_q",
        .no_yield = 1,
    };
    k_work_queue_start(
        &da_resp_work_q,
        dialog_stack,
        K_THREAD_STACK_SIZEOF(dialog_stack),
        CONFIG_SYSTEM_WORKQUEUE_PRIORITY - 1,
        &da_resp_work_q_cfg);
    k_work_init(&da_resp_work, da_resp_work_fn);

    // Set up the data ready pin interrupt so we read responses when data is ready
    int                  pin   = 7;
    const struct device *port  = gpio_p1;
    char                *pname = "P1";

    ret = gpio_pin_configure(port, pin, GPIO_INPUT | GPIO_PULL_DOWN);
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
// wifi_spi_send
//
// Write an AT or ESC command to the DA16200.
//
//  @param data - pointer to buffer containing the AT command
//
//  @return - 0 on success,
//           -errno on error
int
wifi_spi_send(char *data)
{
    int len = strlen(data);
    int err = da_spi_write_rqst(ATCMD_ADDR, data, len);
    if (err) {
        LOG_ERR("failed to write AT_cmd");
        return err;
    }
    return 0;
}
