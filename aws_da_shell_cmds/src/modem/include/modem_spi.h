#pragma once
#include <stddef.h>


int modem_spi_init(void);
int modem_spi_send(char *buf);
int modem_spi_recv(char *buf, k_timeout_t timeout);
void modem_spi_set_rx_cb(void (*cb)(uint8_t *data, size_t len, void *user_data), void *user_data);