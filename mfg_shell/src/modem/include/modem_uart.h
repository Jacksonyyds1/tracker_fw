#pragma once
#include <stddef.h>

int modem_uart_init(void);
int modem_uart_send(char *buf);
int modem_uart_recv(char *buf, k_timeout_t timeout);
void modem_uart_set_rx_cb(void (*cb)(uint8_t *data, size_t len, void *user_data), void *user_data);