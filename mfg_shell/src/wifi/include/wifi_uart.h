#pragma once
#include <stddef.h>
#include "wifi.h"

int wifi_uart_init(void);
/*
 *	Receive a message from the queue,
 *	Buffer must be WIFI_MSG_SIZE in size
 */
int wifi_uart_recv(wifi_msg_t *msg, k_timeout_t timeout);
void wifi_uart_set_rx_cb(wifi_on_rx_cb_t cb, void *user_data);

int wifi_uart_send(char *buf);
int wifi_uart_send_timeout(char *data, k_timeout_t timeout);
void wifi_uart_msg_free(wifi_msg_t *msg);
void wifi_uart_flush_msgs();
int wifi_uart_msg_cnt();
