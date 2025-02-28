#pragma once
#include <zephyr/types.h>

int client_id_get(char *const buffer, size_t buffer_size);
int modem_setdnsaddr(const char *ip_address);
int network_get_lte_mode();