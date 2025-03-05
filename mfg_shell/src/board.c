/*
 * Copyright (c) 2022 Kenzen Inc.
 *
 * SPDX-License-Identifier: Unlicensed
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>

static int board_d1_init(void)
{
	return 0;
}

SYS_INIT(board_d1_init, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
