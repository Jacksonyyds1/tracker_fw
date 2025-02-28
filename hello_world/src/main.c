/*
 * Copyright (c) 2012-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>

int main(void)
{
	printk("Hello World! %s\n", CONFIG_BOARD);
	while(1){
		k_sleep(K_MSEC(1000));
	}
	return 0;
}
