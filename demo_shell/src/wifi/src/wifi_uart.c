#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <stdint.h>
#include <string.h>
#include "wifi_uart.h"

#if (CONFIG_USE_UART_TO_DA16200)

#define WIFI_NUM_MSGS 512
typedef void (*wifi_on_rx_cb_t)(uint8_t *data, size_t len, void *user_data);
wifi_on_rx_cb_t wifi_on_rx_cb = NULL;
void* wifi_on_rx_cb_userData = NULL;

/* queue to store up to 10 messages (aligned to 4-byte boundary) */
K_MSGQ_DEFINE(wifi_uart_msgq, WIFI_MSG_SIZE, WIFI_NUM_MSGS, 4);

static const struct device *const wifi_uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart2));

/* receive buffer used in UART ISR callback */
static char rx_buf[WIFI_MSG_SIZE];
static int rx_buf_pos;
/*
 * Read characters from UART until line end is detected. Afterwards push the
 * data to the message queue.
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

    // method1 seems to jumble the end of long replies - SMR
    //
    /* read until FIFO empty */
    // while (uart_fifo_read(wifi_uart_dev, &c, 1) == 1) {
    //  if ((c == '\n' || c == '\r') && rx_buf_pos > 0) {
    //      rx_buf[rx_buf_pos] = c;

    //      /* if queue is full, message is silently dropped */
    //      k_msgq_put(&wifi_uart_msgq, &rx_buf, K_NO_WAIT);
    //      if (wifi_on_rx_cb) {
    //          wifi_on_rx_cb(rx_buf, rx_buf_pos, wifi_on_rx_cb_userData);
    //      }
    //      /* reset the buffer (it was copied to the msgq) */
    //      rx_buf_pos = 0;
    //  } else if (rx_buf_pos < (sizeof(rx_buf) - 1)) {
    //      rx_buf[rx_buf_pos++] = c;
    //  }
    // }


// method 2 - works, but I dont like it
    while (uart_fifo_read(wifi_uart_dev, &c, 1) == 1) {
        if (rx_buf_pos < (sizeof(rx_buf) - 1)) {
        rx_buf[rx_buf_pos++] = c;
        }
    }
    if (wifi_on_rx_cb) {
        wifi_on_rx_cb(rx_buf, rx_buf_pos, wifi_on_rx_cb_userData);
    } 
    k_msgq_put(&wifi_uart_msgq, &rx_buf, K_NO_WAIT);
    memset(rx_buf, 0, sizeof(rx_buf));
    rx_buf_pos=0;
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
    return 0;
}


/*
*	Receive a message from the queue,
*	Buffer must be WIFI_MSG_SIZE in size
*/
int wifi_uart_recv(char *buf, k_timeout_t timeout)
{
    return k_msgq_get(&wifi_uart_msgq, buf, timeout);
}


void wifi_uart_set_rx_cb(void (*cb)(uint8_t *data, size_t len, void *user_data), void *user_data)
{
    wifi_on_rx_cb = cb;
    wifi_on_rx_cb_userData = user_data;
}


int wifi_uart_init(void)
{
    if (!device_is_ready(wifi_uart_dev)) {
        printk("UART device not found!");
        return 0;
    }

    wifi_on_rx_cb = NULL;

    /* configure interrupt and callback to receive data */
    int ret = uart_irq_callback_user_data_set(wifi_uart_dev, wifi_serial_cb, NULL);

    if (ret < 0) {
        if (ret == -ENOTSUP) {
            printk("Interrupt-driven UART API support not enabled\n");
        } else if (ret == -ENOSYS) {
            printk("UART device does not support interrupt-driven API\n");
        } else {
            printk("Error setting UART callback: %d\n", ret);
        }
        return 0;
    }
    uart_irq_rx_enable(wifi_uart_dev);
    return 0;
}

#endif