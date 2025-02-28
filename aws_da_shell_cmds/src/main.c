/**
	@file main.c
	@brief Project main startup code.
	Copyright (c) 2023 Culvert Engineering - All Rights Reserved
 */

#include <stdlib.h>
#include <zephyr/sys/printk.h>
#include <getopt.h>


#include "modem.h"
#include "wifi.h"
#include "watchdog.h"
#include <zephyr/logging/log.h>

#include "pmic.h"
#if (CONFIG_ENABLE_RFFE_TO_QM13335TR13)
#include "rffe.h"
#endif

LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);


#define SLEEP_TIME_MS 100


////////////////////////////////////////////////////////////////////////////////////
//
// Main
//
bool initDevices() 
{

	if (pmic_init())
	{
		printk("Error: PMIC initialization failed.\n");
		return false;
	}
	set_switch_state(PMIC_SWITCH_VSYS, true);
	set_switch_state(PMIC_SWITCH_WIFI, true);

#if (CONFIG_WATCHDOG)
	if (watchdog_init())
	{
		printk("Error: watchdog initialization failed.\n");
		return false;
	}
#endif

#if (CONFIG_ENABLE_RFFE_TO_QM13335TR13)
	if (RFFE_init())
	{
		printk("Error: RFFE initialization failed.\n");
		return false;
	}
#endif

	if (modem_init())
	{
		printk("Error: 9160 Modem initialization failed.\n");
		return false;
	}

	if (wifi_init())
	{
		printk("Error: Wifi initialization failed.\n");
		return false;
	}


	return true;
}


////////////////////////////////////////////////////////////////////////////////////
//
// Main
//
int main(void)
{

	if (!initDevices())
	{
		printk("Error: Device initialization failed.\n");
		return -1;
	}

	while (1)
	{

		k_msleep(SLEEP_TIME_MS);
	}
}
