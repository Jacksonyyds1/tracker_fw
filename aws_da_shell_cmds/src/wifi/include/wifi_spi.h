#pragma once
#include <stddef.h>
#include "wifi.h"

int wifi_spi_init(void);
void wifi_spi_msg_free(wifi_msg_t *msg);
void wifi_spi_flush_msgs();
int wifi_spi_recv(wifi_msg_t *msg, k_timeout_t timeout);
int wifi_spi_send(char *buf);
int wifi_spi_send_timeout(char *data, k_timeout_t timeout);
void wifi_spi_set_rx_cb(wifi_on_rx_cb_t cb, void *user_data);