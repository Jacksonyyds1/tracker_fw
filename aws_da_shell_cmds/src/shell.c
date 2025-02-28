#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <stdio.h>
#include <zephyr/sys/reboot.h> // for shell reboot command

LOG_MODULE_REGISTER(shell, LOG_LEVEL_DBG);

static int version(const struct shell *sh, size_t argc, char **argv)
{
    shell_fprintf(sh, SHELL_NORMAL, "App version %d.%d.%d\r\n", APP_VERSION_MAJOR,
                  APP_VERSION_MINOR, APP_VERSION_PATCH);
    return 0;
}

static int system_reboot(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "rebooting...\n");
    sys_reboot(SYS_REBOOT_COLD);
}

SHELL_CMD_REGISTER(version, NULL, "app version", version);
SHELL_CMD_REGISTER(reboot, NULL, "reboot system", system_reboot);
