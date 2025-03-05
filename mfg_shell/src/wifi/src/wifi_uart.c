#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <stdint.h>
#include <string.h>
#include "wifi_uart.h"

#if (CONFIG_USE_UART_TO_DA16200)
#if DT_NODE_HAS_STATUS(DT_NODELABEL(spi2), okay)
#error "UART code compiling with spi2 device enabled"
#endif
LOG_MODULE_REGISTER(wifi_uart, CONFIG_WIFI_UART_LOG_LEVEL);

wifi_on_rx_cb_t wifi_on_rx_cb = NULL;
void *wifi_on_rx_cb_userData = NULL;

/* queue to store up to queued messages structures which
contain a len and a pointer to the data received */
K_MSGQ_DEFINE(wifi_msgq, sizeof(wifi_msg_t), 30, 4);

/* The heap stores the received message data */
K_HEAP_DEFINE(wifi_heap, WIFI_MSG_SIZE * 3);

static const struct device *wifi_uart_dev; // = DEVICE_DT_GET(DT_NODELABEL(uart2));

/* receive buffer used in UART ISR callback */
static char rx_buf[WIFI_MSG_SIZE];
static int rx_buf_pos;

void send_rx_msgs_timer_cb(struct k_timer *dummy);
void queue_msgs(void);

K_TIMER_DEFINE(rx_timer, send_rx_msgs_timer_cb, NULL);

/*
 * wifi_serial_cb()
 *
 * Read characters from UART and put "responses" into the msgq.
 * a "response" is anything that begins and ends with <CR><LF>
 *
 * however, since we could miss a character we need a way to sync
 * the start of the message so if the buffer doesn't begin with
 * a <CR><LF> and we see a <CR><LF><CR><LF> we will treat all
 * data before the last <CR><LF> as a message.
 *
 * likewise, if we are looking for the final <CR><LF> and no data
 * comes in for 500ms, we send what we have so far.
 */
void wifi_serial_cb(const struct device *dev, void *user_data)
{
	uint8_t c;
	if (!uart_irq_update(wifi_uart_dev)) {
		return;
	}

	if (!uart_irq_rx_ready(wifi_uart_dev)) {
		return;
	}

	// reset the timeout
	k_timer_start(&rx_timer, K_MSEC(500), K_NO_WAIT);
	while (uart_fifo_read(wifi_uart_dev, &c, 1) == 1) {
		if (rx_buf_pos < (sizeof(rx_buf) - 1)) {
			rx_buf[rx_buf_pos++] = c;
			if (rx_buf_pos > 2 && rx_buf[rx_buf_pos - 2] == '\r' &&
			    rx_buf[rx_buf_pos - 1] == '\n') {
				// Found <CR><LF> not at the start of the rx_buf, we must have a
				// response
				queue_msgs();
				k_timer_stop(&rx_timer);
			}
		} else {
			// we are out of space, send what is in the buffer
			queue_msgs();
			k_timer_stop(&rx_timer);
		}
	}
}

void send_rx_msgs_timer_cb(struct k_timer *dummy)
{
	queue_msgs();
}

/* queue_msgs()
 *
 * queue up all "responses" in the rx_buf and reset the rx_buf_pos
 * Don't call this unless there is data in the rx_buf and we timed
 * out waiting for the rest (in which case send what we have) or
 * we have just read a <CR><LF> that was not at the beginning of
 * the rx_buf so we expect to have a response
 */
void queue_msgs(void)
{
	wifi_msg_t msg;

	if (rx_buf_pos == 0) {
		return;
	}
	int i, start = 0;

	while (start < rx_buf_pos) {
		if (rx_buf_pos - start < 4) { // Too small, just send it
			i = rx_buf_pos - start;
		} else {
			// Look for the ending <CR><LF>
			for (i = start + 1; i < rx_buf_pos - 1; i++) {
				if (rx_buf[i] == '\r' && rx_buf[i + 1] == '\n') {
					// Found <CR><LF> at i
					i++;
					break;
				}
			}
		}
		msg.data_len = i - start + 1;
		msg.data = k_heap_alloc(&wifi_heap, msg.data_len + 1, K_NO_WAIT);
		if (msg.data == NULL) {
			LOG_ERR("No heap left for uart data");
			rx_buf_pos = 0;
			return;
		}
		memcpy(msg.data, rx_buf + start, msg.data_len);
		msg.data[msg.data_len] = 0;
		bool put_on_queue = true;
		if (wifi_on_rx_cb != NULL) {
			// if the callback has consumed the message it will return
			// true.  If so we don't place it on the message queue and
			// free the memory
			put_on_queue = !wifi_on_rx_cb(&msg, wifi_on_rx_cb_userData);
		}
		if (put_on_queue == false) {
			k_heap_free(&wifi_heap, msg.data);
		} else {
			k_msgq_put(&wifi_msgq, &msg, K_NO_WAIT);
		}
		start = i + 2;
	}
	rx_buf_pos = 0;
}

/*
 * Print a null-terminated string character by character to the UART interface
 */
int wifi_uart_send(char *buf)
{
	int msg_len = strlen(buf);
	for (int i = 0; i < msg_len; i++) {
		uart_poll_out(wifi_uart_dev, buf[i]);
	}
	// DA won't process a command unless it ends with <CR><LF>
	if (buf[msg_len - 1] != '\n' && buf[msg_len - 1] != '\r') {
		uart_poll_out(wifi_uart_dev, '\r');
		uart_poll_out(wifi_uart_dev, '\n');
	}
	return 0;
}

int wifi_uart_send_timeout(char *data, k_timeout_t timeout)
{
	return wifi_uart_send(data);
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
int wifi_uart_recv(wifi_msg_t *buf, k_timeout_t timeout)
{
	return k_msgq_get(&wifi_msgq, buf, timeout);
}

void wifi_uart_set_rx_cb(wifi_on_rx_cb_t cb, void *user_data)
{
	wifi_on_rx_cb = cb;
	wifi_on_rx_cb_userData = user_data;
}

int wifi_uart_init(void)
{
	if ((wifi_uart_dev = device_get_binding("uart@b000")) == NULL) {
		LOG_ERR("Error: didn't find %s device", "uart2");
		return -1;
	} else {
		LOG_DBG("found uart2 device");
	}

	if (device_is_ready(wifi_uart_dev) == false) {
		LOG_ERR("Error: %s device not ready", "uart2");
		return -1;
	}

	wifi_on_rx_cb = NULL;

	/* configure interrupt and callback to receive data */
	int ret = uart_irq_callback_user_data_set(wifi_uart_dev, wifi_serial_cb, NULL);

	if (ret < 0) {
		if (ret == -ENOTSUP) {
			LOG_DBG("Interrupt-driven UART API support not enabled\n");
		} else if (ret == -ENOSYS) {
			LOG_DBG("UART device does not support interrupt-driven API\n");
		} else {
			LOG_DBG("Error setting UART callback: %d\n", ret);
		}
		return 0;
	}
	uart_irq_rx_enable(wifi_uart_dev);
	return 0;
}

//////////////////////////////////////////////////////////
// wifi_uart_msg_free()
//
// Free the memory allocated for a wifi_msg_t
// @param msg - pointer to the wifi_msg_t to free
void wifi_uart_msg_free(wifi_msg_t *msg)
{
	k_heap_free(&wifi_heap, msg->data);
}

void wifi_uart_flush_msgs()
{
	wifi_msg_t msg;
	while (k_msgq_get(&wifi_msgq, &msg, K_NO_WAIT) == 0) {
		wifi_uart_msg_free(&msg);
	}
}

int wifi_uart_msg_cnt()
{
	return k_msgq_num_used_get(&wifi_msgq);
}

#endif
