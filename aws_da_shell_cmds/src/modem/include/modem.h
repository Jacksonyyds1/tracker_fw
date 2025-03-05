
#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <zephyr/kernel.h>

//
int modem_recv(char *buf, k_timeout_t timeout);
int modem_send(char *buf);
int modem_init();
void modem_set_rx_cb(void (*cb)(uint8_t *data, size_t len, void *user_data), void *user_data);
int modem_power_on();
int modem_power_off();