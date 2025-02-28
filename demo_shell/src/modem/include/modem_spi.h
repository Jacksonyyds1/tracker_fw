#pragma once
#include <stddef.h>


int modem_spi_init(void);
int modem_spi_send(uint8_t *buf, uint8_t *buf2);
int modem_spi_recv(uint8_t *buf, k_timeout_t timeout);
void modem_spi_set_rx_cb(void (*cb)(uint8_t *data, size_t len, void *user_data), void *user_data);
int modem_spi_send_command(modem_message_type_t type, uint8_t handle, uint8_t* data, uint16_t dataLen);