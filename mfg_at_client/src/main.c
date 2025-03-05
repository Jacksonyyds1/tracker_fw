/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <stdio.h>
#include <string.h>
#include <modem/nrf_modem_lib.h>
#include <zephyr/drivers/uart.h>
#include "5340_interface.h"


// #include <zephyr/drivers/clock_control.h>
// #include <zephyr/drivers/clock_control/nrf_clock_control.h>




int init_devices() {
	int err;

	err = nrf_modem_lib_init();
	if (err) {
		printk("Modem library initialization failed, error: %d\n", err);
		return 0;
	}
	printk("Modem Ready\n");

	init_5340_interface();


	return 0;
}

int main(void)
{
	//int err;

	init_devices();

	return 0;
}