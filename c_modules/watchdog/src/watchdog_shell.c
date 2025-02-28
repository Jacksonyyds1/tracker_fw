/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */

#if (CONFIG_WATCHDOG)

#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/drivers/sensor.h>
#include <stdlib.h>
#include <stdio.h>
#include <watchdog.h>

extern bool                       watchdog_disable_kick;
extern const struct device *const wdt;

void
disable_watchdog_kick_cmd(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    // feed it one last time to reset the timeout
    // int ret = wdt_feed(wdt, 0);
    watchdog_kick();

    watchdog_disable_kick = true;
    shell_print(sh, "Watchdog kick disabled, resetting in %d ms\n", CONFIG_WDT_NPM1300_TIMEOUT_IN_MS);
}

void
disable_watchdog_cmd(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    watchdog_disable();
    shell_print(sh, "Watchdog disabled\n");
}

void
enable_watchdog_cmd(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

    watchdog_init();
    shell_print(sh, "Watchdog enabled\n");
}

// SHELL_CMD_REGISTER(disable_watchdog, NULL, "disables watchdog", disable_watchdog_cmd);

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_watchdog,
    SHELL_CMD(disable, NULL, "disable watchdog checking", disable_watchdog_cmd),
    SHELL_CMD(disable_kick, NULL, "stop kicking/feeding the watchdog", disable_watchdog_kick_cmd),
    SHELL_CMD(enable, NULL, "enable the watchdog", enable_watchdog_cmd),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(watchdog, &sub_watchdog, "Commands to control the watchdog", NULL);

#endif