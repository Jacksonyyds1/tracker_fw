/*
 * Copyright (c) 2023 Culvert Engineering Inc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/dfu/mcuboot.h>

#include "app_version.h"
#include "ble.h"
#include "imu.h"
#include "ml.h"
#include "pmic.h"
#include "storage.h"
#include "utils.h"
#include "watchdog.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

int main(void)
{
	LOG_INF("Staring chekr-app Version %d.%d.%d (%s) \r\nBoard: %s", APP_VERSION_MAJOR,
		APP_VERSION_MINOR, APP_VERSION_PATCH, GIT_HASH, CONFIG_BOARD);

	// we don't use these for chekr app, so turn them off to save power
	util_enable_dialog(false);
	util_enable_9160(false);

	imu_init();
	ml_init();
	storage_init();
	ble_init();
	watchdog_init();

	/*
	   mark the currently running firmware image as OK,
	   which will install it permanently after an OTA
	 */
	boot_write_img_confirmed();

	while (1) {
		k_sleep(K_MSEC(1000));
		watchdog_feed();
	}
	return 0;
}

