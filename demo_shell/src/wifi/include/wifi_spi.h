#pragma once
#include <stddef.h>


int wifi_spi_init(void);
int wifi_spi_send(char *buf);
int wifi_spi_recv(char *buf, k_timeout_t timeout);
void wifi_spi_set_rx_cb(void (*cb)(uint8_t *data, size_t len, void *user_data), void *user_data);