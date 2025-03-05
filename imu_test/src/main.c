/*
 * Copyright (c) 2023 Culvert Engineering Inc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/dfu/mcuboot.h>

#include "ble.h"
#include "imu.h"
#include "storage.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

int main(void)
{
	LOG_INF("Staring imu_test Version %d.%d.%d \r\nBoard: %s", APP_VERSION_MAJOR,
			APP_VERSION_MINOR, APP_VERSION_PATCH, CONFIG_BOARD);

	imu_init();
	storage_init();
	ble_init();

	/*
	   mark the currently running firmware image as OK,
	   which will install it permanently after an OTA
	 */
	boot_write_img_confirmed();

	while (1)
	{
		k_sleep(K_MSEC(1000));
	}
	return 0;
}
