/**
	@file main.c
	@brief Project main startup code.
	Copyright (c) 2023 Culvert Engineering - All Rights Reserved
 */

#include <stdlib.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/sys/printk.h>
#include <getopt.h>

#include "modem.h"
#include "wifi.h"
#include "watchdog.h"
#include "uicr.h"
#include <zephyr/logging/log.h>

#include "imu.h"
#include "ble.h"
#include "pmic.h"
#if (CONFIG_ENABLE_RFFE_TO_QM13335TR13)
#include "rffe.h"
#endif
#include "app_version.h"
LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#define SLEEP_TIME_MS 100

////////////////////////////////////////////////////////////////////////////////////
//
// Main
//
bool initDevices()
{

	if (pmic_init(true,true)) {
		printk("Error: PMIC initialization failed.\n");
		return false;
	}
	set_switch_state(PMIC_SWITCH_VSYS, true);
	set_switch_state(PMIC_SWITCH_WIFI, true);

#if (CONFIG_WATCHDOG)
	if (watchdog_init()) {
		printk("Error: watchdog initialization failed.\n");
		return false;
	}
#endif

#if (CONFIG_ENABLE_RFFE_TO_QM13335TR13)
	if (RFFE_init()) {
		printk("Error: RFFE initialization failed.\n");
		return false;
	}
#endif

	if (modem_init()) {
		printk("Error: 9160 Modem initialization failed.\n");
		return false;
	}

	if (wifi_init()) {
		printk("Error: Wifi initialization failed.\n");
		return false;
	}
	if (ble_init()) {
		printk("Error: BLE initialization failed.\n");
		return false;
	}

	if (imu_init()) {
		printk("Error: IMU initialization failed.\n");
		return false;
	}

	return true;
}
int enable_nrf9160()
{
	printf("IN DEMO mode, so Sending modem start command\n");
	uint8_t cmd = COMMAND_START;
	char resp_buf[200];
	uint16_t resp_len = 0;

	int my_handle = modem_send_command(MESSAGE_TYPE_COMMAND, &cmd, 1, true);
	memset(resp_buf, 0, sizeof(resp_buf));
	if (modem_recv_resp(my_handle, resp_buf, &resp_len, 1000) ==
	    0) { // 1 sec timeout, thats a looooong time
		// Modem returned valid response
		if (strstr(resp_buf, "OK\r\n")) {
			printf("[%s]..OK...\n", "COMMAND_START");
		}
		// printf("<<<<{%s}\n",resp_buf);
	} else {
		// Modem response timed out
		printf("Modem response timed out\n");
	}
	return 0;
}

#if CONFIG_CULVERT_LOCATION_DEMO_MODE

// ASSET_TRACKER_V2 Batt Level Stuff
void do_asset_tracker_update_fuel_gauge(struct k_work *work)
{
	fuel_gauge_info_t info;
	fuel_gauge_get_latest(&info);
	// EAS Clutter LOG_DBG("Sending updated battery percentage: %f", info.soc);
	modem_send_command(MESSAGE_TYPE_BATTERY_LEVEL, (uint8_t *)(&(info.soc)), sizeof(info.soc),
			   false);
}
K_WORK_DEFINE(my_fuel_gauge_work, do_asset_tracker_update_fuel_gauge);
void my_fuel_gauge_timer_handler(struct k_timer *dummy)
{
	k_work_submit(&my_fuel_gauge_work);
}
K_TIMER_DEFINE(my_fuel_gauge_timer, my_fuel_gauge_timer_handler, NULL);

char packet_to_tx[2000];
char packet_to_rx[2000];
int packet_to_tx_index = 0;
static void parse_ssids(const char *buf)
{
#define TAB 9
#define EOL 10
	// EAS  clutter reduce printf("%s\n",buf);
	enum states {
		MAC,
		CHANNEL,
		RSSI,
		CREDS,
		SSID
	};
	enum states sm_state = MAC;

	char *match = strstr(&buf[2], "+WFSCAN");
	if (match == NULL) {
		printf("this is NOT a wifi packet, abort\n");
		return;
	} else {
		if (match == &buf[2]) {
			// EAS clutter printf("this is a TRULY  wifi packet\n");
		} else {
			// EAS clutter printf("this is STILL NOT a wifi packet\n");
			return;
		}
	}
	memset(packet_to_tx, 0, 2000);
	packet_to_tx_index = 0;
	for (int i = 10; i < strlen(buf); i++) {
		switch (buf[i]) {
		case TAB:
			// EAS  clutter reduce printf(" +  ");
			switch (sm_state) {
			case MAC:
				packet_to_tx[packet_to_tx_index++] = ',';
				sm_state = CHANNEL;
				break;
			case CHANNEL:
				sm_state = RSSI;
				break;
			case RSSI:
				sm_state = CREDS;
				break;
			case CREDS:
				sm_state = SSID;
				break;
			default:
				break;
			}
			break;
		case EOL:
			// EAS clutter reduce  printf("+");
			packet_to_tx[packet_to_tx_index++] = '\n';
			sm_state = MAC;
			break;
		default:
			if (sm_state == RSSI || sm_state == MAC) {
				// EAS  clutter reduce printf("%c",buf[i]);
				packet_to_tx[packet_to_tx_index++] = buf[i];
			}
		}
	}
	packet_to_tx[packet_to_tx_index] = 0;
	// EAS  clutter reduce printf("\n\n>>> %s\n\n",packet_to_tx);
	int my_handle =
		modem_send_command(MESSAGE_TYPE_SSIDS, packet_to_tx, packet_to_tx_index, false);
	(void)my_handle;
#if 0
    uint16_t len = 2048;
    packet_to_rx[0] = 0xFF;
    if(modem_recv_resp(my_handle, packet_to_rx, &len, 5000) == 0) {
        if( packet_to_rx[0] == 0 ) {
            printf("Modem responded OK to SSIDs\n");
        }
    }else{
        printf("No response in 5 seconds\n");
    }
    modem_free_reply_data(my_handle);
#endif
}
#define SSID_SCAN_INTERVAL (55)
char ssid_buffer[1500];

void do_asset_tracker_update_ssids(struct k_work *work)
{
	memset(ssid_buffer, 0, sizeof(ssid_buffer)); // start it off cleared
	// EAS  clutter reduce int ret = get_wfscan(ssid_buffer, 1500);
	get_wfscan(ssid_buffer, 1500);
	// EAS  clutter reduce printf("%d:%s",ret,ssid_buffer);
	parse_ssids(ssid_buffer);
}
K_WORK_DEFINE(my_ssids_work, do_asset_tracker_update_ssids);
void my_ssids_timer_handler(struct k_timer *dummy)
{
	k_work_submit(&my_ssids_work);
}
K_TIMER_DEFINE(my_ssids_timer, my_ssids_timer_handler, NULL);
#endif
////////////////////////////////////////////////////////////////////////////////////
//
// Main
//
int main(void)
{
	if (!initDevices()) {
		printk("Error: Device initialization failed.\n");
		return -1;
	}

	/*
	   mark the currently running firmware image as OK,
	   which will install it permanently after an OTA
	 */
	boot_write_img_confirmed();

#if CONFIG_CULVERT_LOCATION_DEMO_MODE
	printf("NRFCLOUD DEMO App version %d.%d.%d (%s)\r\n", APP_VERSION_MAJOR, APP_VERSION_MINOR,
	       APP_VERSION_PATCH, GIT_HASH);

	printf("Built on %s by %s\r\n", DBUILD_DATE, DBUILD_MACHINE);
#else
	printf("App version %d.%d.%d (%s)\r\n", APP_VERSION_MAJOR, APP_VERSION_MINOR,
	       APP_VERSION_PATCH, GIT_HASH);

	printf("Built on %s by %s\r\n", DBUILD_DATE, DBUILD_MACHINE);
#endif

#if CONFIG_CULVERT_LOCATION_DEMO_MODE
#pragma message("BUILDING A CULVERT LOCATION DEMO VERSION OF THE SHELL!!!!!")
	LOG_WRN("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
	LOG_WRN("THIS IS A CULVERT LOCATION DEMO VERSION OF THE SHELL!!!!!");
	LOG_WRN("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
	k_timer_start(&my_fuel_gauge_timer, K_SECONDS(30), K_SECONDS(60));
	k_timer_start(&my_ssids_timer, K_SECONDS(60), K_SECONDS(60));
	enable_nrf9160();
#endif
	while (1) {
		k_msleep(SLEEP_TIME_MS);
	}
}
#if CONFIG_CULVERT_LOCATION_DEMO_MODE
#include <zephyr/shell/shell.h>
SHELL_CMD_REGISTER(parse_ssids, NULL, "parse ssids", do_asset_tracker_update_ssids);
#endif