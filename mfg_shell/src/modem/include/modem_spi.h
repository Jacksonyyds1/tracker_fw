#pragma once
#include <stddef.h>

int modem_spi_init(void);
int modem_spi_send(uint8_t *buf, uint8_t *buf2, uint8_t recur_cnt);
int modem_spi_recv(uint8_t *buf, k_timeout_t timeout);
void modem_spi_set_rx_cb(void (*cb)(uint8_t *data, size_t len, void *user_data), void *user_data);
int modem_spi_send_command(modem_message_type_t type, uint8_t *data, uint16_t dataLen,
			   bool reply_requested);
int modem_spi_recv_resp(uint8_t handle, uint8_t *data, uint16_t *dataLen, int timeout);
int modem_spi_free_reply_data(int handle);
int send_spi_command(uint8_t cmd);

int modem_spi_get_IMEI(uint8_t *buf);
int modem_spi_get_VERSION(uint8_t *buf);
int modem_spi_get_ICCID(uint8_t *buf);