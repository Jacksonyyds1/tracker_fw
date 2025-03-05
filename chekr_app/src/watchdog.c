/*
 * Copyright (c) 2023 Culvert Engineering
 *
 * SPDX-License-Identifier: Unlicensed
 */

#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include "watchdog.h"

LOG_MODULE_REGISTER(watchdog, LOG_LEVEL_DBG);

#define WATCHDOG_TIMEOUT_SEC (5)

struct wdt_data_storage {
	const struct device *wdt_drv;
	int wdt_channel_id;
};

static struct wdt_data_storage wdt_data;
static bool force_wdt;

void wdt_callback(const struct device *dev, int channel_id)
{
	// TODO: we have a few uS before we reboot, add any handling here if desired
}

static int watchdog_timeout_install(struct wdt_data_storage *data)
{
	static const struct wdt_timeout_cfg wdt_settings = {
		.window =
			{
				.min = 0,
				.max = WATCHDOG_TIMEOUT_SEC * 1000,
			},
		.callback = wdt_callback,
		.flags = WDT_FLAG_RESET_SOC};

	data->wdt_channel_id = wdt_install_timeout(data->wdt_drv, &wdt_settings);
	if (data->wdt_channel_id < 0) {
		LOG_ERR("Cannot install watchdog timer! Error code: %d", data->wdt_channel_id);
		return -EFAULT;
	}

	LOG_INF("Watchdog timeout installed. Timeout: %d s", WATCHDOG_TIMEOUT_SEC);
	return 0;
}

static int watchdog_start(struct wdt_data_storage *data)
{
	int err = wdt_setup(data->wdt_drv, WDT_OPT_PAUSE_HALTED_BY_DBG);

	if (err) {
		LOG_ERR("Cannot start watchdog! Error code: %d", err);
	} else {
		LOG_INF("Watchdog started");
	}
	return err;
}

int watchdog_init(void)
{
	int err = -ENXIO;

	wdt_data.wdt_drv = DEVICE_DT_GET(DT_NODELABEL(wdt));

	if (wdt_data.wdt_drv == NULL) {
		LOG_ERR("Cannot bind watchdog driver");
		return err;
	}

	err = watchdog_timeout_install(&wdt_data);
	if (err) {
		return err;
	}

	err = watchdog_start(&wdt_data);
	if (err) {
		return err;
	}

	return 0;
}

void watchdog_feed(void)
{
	if (force_wdt) {
		LOG_INF("watchdog_force ofen");
		return; // strictly for testing WDT
		
	}

	int err = wdt_feed(wdt_data.wdt_drv, wdt_data.wdt_channel_id);

	if (err) {
		LOG_ERR("Cannot feed watchdog. Error code: %d", err);
	}
}

void watchdog_force(void)
{
	force_wdt = true;
}

static int watchdog_force_shell(const struct shell *sh, size_t argc, char **argv)
{
	force_wdt = true;
	shell_fprintf(sh, SHELL_NORMAL,
		      "disabled watchdog petting, should reboot in %d seconds\r\n",
		      WATCHDOG_TIMEOUT_SEC);
	return 0;
}

SHELL_CMD_REGISTER(watchdog_force, NULL, "force watchdog to fire", watchdog_force_shell);
