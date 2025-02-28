#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <stdio.h>
#include <zephyr/sys/reboot.h> // for shell reboot command
#include "app_version.h"
LOG_MODULE_REGISTER(shell, LOG_LEVEL_DBG);

static int version(const struct shell *sh, size_t argc, char **argv)
{
#if CONFIG_CULVERT_LOCATION_DEMO_MODE
	shell_fprintf(sh, SHELL_NORMAL, "NRFCLOUD DEMO App version %d.%d.%d (%s)\r\n",
		      APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_VERSION_PATCH, GIT_HASH);

	shell_fprintf(sh, SHELL_NORMAL, "Built on %s by %s\r\n", DBUILD_DATE, DBUILD_MACHINE);
#else
	shell_fprintf(sh, SHELL_NORMAL, "App version %d.%d.%d (%s)\r\n", APP_VERSION_MAJOR,
		      APP_VERSION_MINOR, APP_VERSION_PATCH, GIT_HASH);

	shell_fprintf(sh, SHELL_NORMAL, "Built on %s by %s\r\n", DBUILD_DATE, DBUILD_MACHINE);
#endif
	return 0;
}

static int system_reboot(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "rebooting...\n");
	sys_reboot(SYS_REBOOT_COLD);
}

SHELL_CMD_REGISTER(version, NULL, "app version", version);
SHELL_CMD_REGISTER(reboot, NULL, "reboot system", system_reboot);
