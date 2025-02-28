#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <stdio.h>
#include "rffe.h"

#define TRIGGER 0x38

static int send_ant_cfg(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uint8_t ret = 0;
	if (argc < 2) {
		shell_print(sh, "Usage: %s <cfg>\n", argv[0]);
		shell_print(sh, "\t<cfg> is a hex value from 0x0 to 0xFF where:\n");
		shell_print(sh, "\t[7:4]   shunt switches");
		shell_print(sh, "\t0x0: ALL OFF");
		shell_print(sh, "\t0x1: RF1");
		shell_print(sh, "\t0x2: RF2");
		shell_print(sh, "\t0x3: RF2, RF1");
		shell_print(sh, "\t0x4: RF3");
		shell_print(sh, "\t0x5: RF3, RF1");
		shell_print(sh, "\t0x6: RF3, RF2");
		shell_print(sh, "\t0x7: RF3, RF2, RF1");
		shell_print(sh, "\t0x8: RF4");
		shell_print(sh, "\t0x9: RF4, RF1");
		shell_print(sh, "\t0xA: RF4, RF2");
		shell_print(sh, "\t0xB: RF4, RF2, RF1");
		shell_print(sh, "\t0xC: RF4, RF3");
		shell_print(sh, "\t0xD: RF4, RF3, RF1");
		shell_print(sh, "\t0xE: RF4, RF3, RF2");
		shell_print(sh, "\t0xF: All On\n");
		shell_print(sh, "\t[3:0]   series switches");
		shell_print(sh, "\t0x0: ALL OFF");
		shell_print(sh, "\t0x1: RF1");
		shell_print(sh, "\t0x2: RF2");
		shell_print(sh, "\t0x3: RF2, RF1");
		shell_print(sh, "\t0x4: RF3");
		shell_print(sh, "\t0x5: RF3, RF1");
		shell_print(sh, "\t0x6: RF3, RF2");
		shell_print(sh, "\t0x7: RF3, RF2, RF1");
		shell_print(sh, "\t0x8: RF4");
		shell_print(sh, "\t0x9: RF4, RF1");
		shell_print(sh, "\t0xA: RF4, RF2");
		shell_print(sh, "\t0xB: RF4, RF2, RF1");
		shell_print(sh, "\t0xC: RF4, RF3");
		shell_print(sh, "\t0xD: RF4, RF3, RF1");
		shell_print(sh, "\t0xE: RF4, RF3, RF2");
		shell_print(sh, "\t0xF: All On");

		return 1;
	}

	uint8_t cfg = strtol(argv[1], NULL, 16);
	if (cfg > 0xFF) {
		shell_print(sh, "Invalid cfg value: %d\n", cfg);
		return 1;
	}

	// shell_print(sh, "Sending TriggerMask = %d (%X)\n", TRIGGER,  TRIGGER);
	// RFFE_write(0x1c, TRIGGER);

	// shell_print(sh, "Sending PWR_MODE = 0\n", cfg);
	RFFE_write(0x1c, 0x00);

	ret = RFFE_read(0x001C);
	// shell_print(sh, "Antenna PM_TRIG read back as 0x%02X\n", ret);

	// shell_print(sh, "Sending TriggerMask = %d (%X)\n", TRIGGER,  TRIGGER);
	RFFE_write(0x1c, TRIGGER);

	// shell_print(sh, "Sending ANT_CTRL1 = %X\n", cfg);
	RFFE_write(0x001, cfg);
	// shell_print(sh, "Antenna config set to 0x%02X\n", cfg);

	// shell_print(sh, "Sending TriggerMask = %d (%X)\n", TRIGGER, TRIGGER);
	RFFE_write(0x1c, TRIGGER);

	ret = RFFE_read(0x01);
	shell_print(sh, "Antenna config read back as 0x%02X\n", ret);

	ret = RFFE_read(0x1D);
	shell_print(sh, "RFFE read MFG_ID 0x001D (should be 102): %d\n", ret);

	ret = RFFE_read(0x1e);
	shell_print(sh, "RFFE read MFG_ID 0x001D (should be 198): %d\n", ret);

	// shell_print(sh, "Sending 2D = 0xaaaa\n");
	// RFFE_write(0x2d, 0xaaaa);
	// ret = RFFE_read(0x2d);
	// shell_print(sh, "RFFE read 2d : %X\n", ret);

	ret = 0;

	// while (1) {
	//  k_busy_wait(100);
	//  ret = RFFE_read(0x1f);
	//  shell_print(sh, "RFFE read  0x001f : %d\n", ret);
	//}
	// RFFE_write2(0x2d, 0xaa);
	// ret = RFFE_read(0x002D);
	// printk("RFFE read EXT_TRIG_MASK 0x002D (should be aa): %d\n", ret);

	return 0;
}

SHELL_CMD_REGISTER(send_ant_cfg, NULL, "send ant config", send_ant_cfg);
