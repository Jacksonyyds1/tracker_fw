/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
/*
 *
 * SPDX-License-Identifier: LicenseRef-Proprietary
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/printk.h>
#include <stdbool.h>
#include <zephyr/logging/log.h>
#include "watchdog.h"

LOG_MODULE_REGISTER(watchdog, LOG_LEVEL_DBG);

bool                       watchdog_disable_kick = false;
const struct device *const wdt                   = DEVICE_DT_GET(DT_NODELABEL(npm1300_wdt));

void
watchdog_work_handler(struct k_work *work)
{
    if (watchdog_disable_kick) {
        LOG_DBG("Watchdog kick disabled, skipping feed");
        return;
    }
    int ret = wdt_feed(wdt, 0);
    if (ret != 0) {
        LOG_ERR("Watchdog feed error");
    }
    // disabled for now, its chatty.
    // else if (ret == 0) {
    //     LOG_DBG("Watchdog feed OK");
    // }
}
K_WORK_DEFINE(watchdog_work, watchdog_work_handler);

void
watchdog_timer_handler(struct k_timer *dummy)
{
    k_work_submit(&watchdog_work);
}
K_TIMER_DEFINE(watchdog_timer, watchdog_timer_handler, NULL);

int
watchdog_init()
{
    if (!device_is_ready(wdt)) {
        LOG_ERR("%s: watchdog device not ready.", wdt->name);
        return 0;
    }

#if (CONFIG_WDT_DISABLE_AT_BOOT)
    LOG_DBG("Watchdog device ready, but NOT started.");
#else
    uint32_t timeout = CONFIG_WDT_NPM1300_TIMEOUT_IN_MS;

    struct wdt_timeout_cfg wdt_config = {
        .window.max = timeout,
        .callback   = NULL,
        .flags      = WDT_FLAG_RESET_SOC,
    };

    int err = wdt_install_timeout(wdt, &wdt_config);
    if (err < 0) {
        LOG_ERR("Watchdog install error");
        return 0;
    }

    err = wdt_setup(wdt, 0);
    if (err < 0) {
        LOG_ERR("Watchdog setup error\n");
        return 0;
    }

    uint32_t timeout_ms = CONFIG_WDT_NPM1300_TIMEOUT_IN_MS;
    k_timer_start(&watchdog_timer, K_MSEC(100), K_MSEC(timeout_ms / 2));
    LOG_DBG("Watchdog device ready.");
#endif

    return 0;
}

void
watchdog_kick()
{
    wdt_feed(wdt, 0);
}

void
watchdog_disable()
{
    wdt_disable(wdt);
    k_timer_stop(&watchdog_timer);
    LOG_DBG("Watchdog disabled");
}