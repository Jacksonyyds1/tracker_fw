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
#include <zephyr/logging/log.h>

#include "imu.h"
#include "ble.h"
#include "pmic.h"
#include "wifi_uart.h"

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
	if (ble_init())
	{
		printk("Error: BLE initialization failed.\n");
		return false;
	}

	if (imu_init())
	{
		printk("Error: IMU initialization failed.\n");
		return false;
	}

	return true;
}


// ASSET_TRACKER_V2 Batt Level Stuff
void do_asset_tracker_update_fuel_gauge(struct k_work *work) {
	fuel_gauge_info_t info;
    fuel_gauge_get_latest(&info);
	LOG_DBG("Sending updated battery percentage: %f", info.soc);
	modem_send_command(MESSAGE_TYPE_BATTERY_LEVEL, 1, (uint8_t*)(&(info.soc)), sizeof(info.soc));
}
K_WORK_DEFINE(my_fuel_gauge_work, do_asset_tracker_update_fuel_gauge);
void my_fuel_gauge_timer_handler(struct k_timer *dummy)
{
    k_work_submit(&my_fuel_gauge_work);
}
K_TIMER_DEFINE(my_fuel_gauge_timer, my_fuel_gauge_timer_handler, NULL);


uint8_t full_ssid_buf2[5000]={0};
int full_ssid_buf2_index= 0;
uint8_t ssid_rx_buf2[1000]={0};
char packet_to_send2[2000];
int packet_to_send2_index= 0;
static void parse_ssids(const char* buf)
{
    #define TAB 9
    #define EOL 10
    printf("%s\n",buf);
    enum states {MAC, CHANNEL,RSSI, CREDS,SSID};
    enum states sm_state = MAC;

    char* match = strstr(&buf[1],"+WFSCAN");
    if( match == NULL){
        printf("this is NOT a wifi packet, abort\n");
        return;
    }else{
        if( match == &buf[1]){
            printf("this is a TRULY  wifi packet\n");
        }else {
            printf("this is STILL NOT a wifi packet\n");
            return;
        }
    }
    memset(packet_to_send2,0,2000);
    packet_to_send2_index=0;
    for(int i = 9 ;i <strlen(buf);i++){
        switch(buf[i]){
        case TAB:
            printf(" +  ");
            switch(sm_state){
                case MAC:
                    packet_to_send2[packet_to_send2_index++]= ',';
                    sm_state  = CHANNEL;
                    break;
                case CHANNEL:
                    sm_state  = RSSI;
                    break;
                case RSSI:
                    sm_state  = CREDS;
                    break;
                case CREDS:
                    sm_state  = SSID;
                    break;
                default:
                    break;
            }
            break;
        case EOL:
            printf("+");
            packet_to_send2[packet_to_send2_index++]= '\n';
            sm_state = MAC;
            break;
        default:
            if( sm_state == RSSI ||
                sm_state == MAC ){
                printf("%c",buf[i]);
                packet_to_send2[packet_to_send2_index++]= buf[i];
            }
        }
    }
    packet_to_send2[packet_to_send2_index]= 0;
    printf("[%d]%s\n",packet_to_send2_index, packet_to_send2);
    modem_send_command(MESSAGE_TYPE_SSIDS, 0x1, packet_to_send2, packet_to_send2_index);
}
#define SSID_SCAN_INTERVAL (55)
void do_asset_tracker_update_ssids(struct k_work *work)
{
#if 1
    char* my_string = "AT+WFSCAN\r";
    full_ssid_buf2_index =0;
    memset(full_ssid_buf2,0,sizeof(full_ssid_buf2));
    if (true) {
        while (wifi_recv_line(ssid_rx_buf2, K_MSEC(100)) == 0) {
			//Purge
		}
        wifi_send(my_string);
        while (wifi_recv_line(ssid_rx_buf2, K_MSEC(1000)) == 0) {
            if(strstr(ssid_rx_buf2,"+WFSCAN") == NULL){
                continue;
            }
            int rx_cnt = strlen(ssid_rx_buf2);
            strcpy(&full_ssid_buf2[full_ssid_buf2_index],&ssid_rx_buf2[0]);
            full_ssid_buf2_index += rx_cnt;
            memset(ssid_rx_buf2, 0,1000);
        }
    }
    parse_ssids(full_ssid_buf2);
    wifi_sleep((SSID_SCAN_INTERVAL-5)*1000);
#else 
    (void)parse_ssids;
    unsigned char packet_to_send2[5]= {0xFF};
    modem_send_command(MESSAGE_TYPE_JSON, 0x1, packet_to_send2, 5);
#endif    
}

// ASSET_TRACKER_V2 SSID Stuff
// void do_asset_tracker_update_ssids(struct k_work *work) {
// 	// uint8_t ssidList[][7] = {
// 	// 	{0x9c,0x3d,0xcf,0x3e,0x93,0xb6, 53},
// 	// 	{0x9e,0x3d,0xcf,0x3e,0x93,0xb7, 54},
// 	// 	{0x78,0x8c,0xb5,0x3b,0x84,0xd6, 65},
// 	// 	{0x38,0xa0,0x67,0x84,0xf5,0xa5, 42},
// 	// 	{0x38,0xa0,0x67,0x84,0xf5,0xa4, 71},
// 	// 	{0x78,0x8c,0xb5,0x3b,0x8b,0xa6, 58},
// 	// 	{0x7e,0x8c,0xb5,0x3b,0x8b,0xa6, 55}
// 	// };
// 	// LOG_DBG("Sending updated ssid list");
// 	modem_send_command(MESSAGE_TYPE_SSIDS, 1, (uint8_t*)&ssidList[0], 7*7);
// }
K_WORK_DEFINE(my_ssids_work, do_asset_tracker_update_ssids);
void my_ssids_timer_handler(struct k_timer *dummy)
{
    k_work_submit(&my_ssids_work);
}
K_TIMER_DEFINE(my_ssids_timer, my_ssids_timer_handler, NULL);


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

	/*
	   mark the currently running firmware image as OK,
	   which will install it permanently after an OTA
	 */
	boot_write_img_confirmed();

	k_timer_start(&my_fuel_gauge_timer, K_SECONDS(30), K_SECONDS(60));
	k_timer_start(&my_ssids_timer, K_SECONDS(60), K_SECONDS(60));

	while (1)
	{

		k_msleep(SLEEP_TIME_MS);
	}
}
