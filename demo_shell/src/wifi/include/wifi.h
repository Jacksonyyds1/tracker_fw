
#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <zephyr/kernel.h>

//
int wifi_recv(char *buf, k_timeout_t timeout);
int wifi_send(char *buf);
int wifi_init();
void wifi_set_rx_cb(void (*cb)(uint8_t *data, size_t len, void *user_data), void *user_data);
int wifi_power_on();
int wifi_power_off();
int wifi_set_power_key(bool newState);
int wifi_set_3v3_enable(bool newState);
void wifi_sleep(int millis);