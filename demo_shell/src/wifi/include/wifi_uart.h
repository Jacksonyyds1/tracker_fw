#pragma once
#include <stddef.h>

#define WIFI_MSG_SIZE 16

int wifi_uart_init(void);
int wifi_uart_send(char *buf);
/*
*	Receive a message from the queue,
*	Buffer must be WIFI_MSG_SIZE in size
*/
int wifi_uart_recv(char *buf, k_timeout_t timeout);
int wifi_recv_line(char *buf,k_timeout_t timeout);
void wifi_uart_set_rx_cb(void (*cb)(uint8_t *data, size_t len, void *user_data), void *user_data);
