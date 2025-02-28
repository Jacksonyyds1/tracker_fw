/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <zephyr/kernel.h>

int             ble_init(void);
struct bt_conn *ble_get_conn(void);
int             ble_advertise_start(int timeout_sec);
int             ble_stop(void);
