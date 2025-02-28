
#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <zephyr/kernel.h>

// header
//    - P
//    - MID  (dev id/name?)
//    - MK  (US)
//    - B
//    - T

// data going to modem
// Safe zone number - when connected, maybe rssi
//    rssi (see above)
// SSID list
// cell tower(s)
// inference data
//    prob a lot of struct here

//  RF mode - (lte or wifi)
// GPS perf data
// wifi perf data
// self test info
// HW fault status
// reboot count
// logs

// events
//    battery low
//    battery critical
//    tracker connected to safe zone
///   tracker failed HW test #xxx
//    charging start/stop
//    fota started/completed

typedef enum {
	MESSAGE_TYPE_NO_OP = 0,
	MESSAGE_TYPE_RESPONSE = 1,
	MESSAGE_TYPE_COMMAND = 2,
	MESSAGE_TYPE_AT = 3,
	MESSAGE_TYPE_BATTERY_LEVEL = 4,
	MESSAGE_TYPE_JSON = 5,
	MESSAGE_TYPE_SSIDS = 6,
	MESSAGE_TYPE_DEVICE_PING = 7,
	MESSAGE_TYPE_NULL = 0xff
} modem_message_type_t;

typedef enum {
	COMMAND_NO_OP = 0,
	COMMAND_REBOOT = 1,
	COMMAND_START = 2,
	COMMAND_NULL = 0xff
} command_type_t;

typedef struct {
	uint8_t version;
	uint8_t messageType;
	uint8_t messageHandle;
	uint16_t dataLen;
	// uint8_t* data;
} __attribute__((__packed__)) message_command_v1_t;

//
int modem_recv(uint8_t *buf, k_timeout_t timeout);
int modem_send(uint8_t *buf);
int modem_send_rcv(uint8_t *buf, uint8_t *buf2);
int modem_init();
void modem_set_rx_cb(void (*cb)(uint8_t *data, size_t len, void *user_data), void *user_data);
int modem_power_on();
int modem_power_off();
int modem_send_command(modem_message_type_t type, uint8_t *data, uint16_t dataLen,
		       bool reply_requested);
int modem_recv_resp(uint8_t handle, uint8_t *data, uint16_t *dataLen, int timeout);
int modem_free_reply_data(int handle);

int modem_get_IMEI(uint8_t *buf);
int modem_get_VERSION(uint8_t *buf);
int modem_get_ICCID(uint8_t *buf);
int modem_set_rffe_settings();