/*
 * Copyright (c) 2022 Kenzen Inc.
 *
 * SPDX-License-Identifier: Unlicensed
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

#define ACCEL_ON (7)

static int board_d1_init(void)
{
	static const struct device *en_imu = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	if (en_imu == NULL)
	{
		printk("Error: failed to find gpio0\n");
		return 0;
	}
	else
	{
		int ret = gpio_pin_configure(en_imu, ACCEL_ON, GPIO_OUTPUT_ACTIVE);
		if (ret != 0)
		{
			printk("Error: failed to configure ACCEL_ON\n");
		}
		else
		{
			printk("powered on the IMU\n");
		}
	}

	return 0;
}

SYS_INIT(board_d1_init, PRE_KERNEL_1,
		 CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
