
#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <zephyr/kernel.h>
#include <zephyr/sys_clock.h>
#include "d1_json.h"

// The DA can send about 2K of data for large message but most will much
// smaller. The wifi_spi will store a wifi_msg_t at the end of this buffer
// so it needs to be at least sizeof(wifi_msg_t)
#define WIFI_MSG_SIZE                 2100
#define COM_BUF_LEN                   3000
#define WIFI_SEND_DATA_PERIOD_SECONDS ((60 * 5) + 10) // EAS XXX
// When data is received, it is placed on the receive queue, and then a
// pointer and length will be passed to the callback function
typedef struct wifi_msg {
	uint16_t data_len;
	uint8_t *data;
} wifi_msg_t;

// The callback function will be called when data is received
// Note that the message passed in is lastest message received
// and is not on the message queue yet.  If the fuction returns
// true, then the message will never be placed on the queue and
// the memory for freed.
typedef bool (*wifi_on_rx_cb_t)(wifi_msg_t *msg, void *user_data);

//////////////////////////////////////////////////////////
//	Receive a message from the queue,
//
// @param msg  - pointer to the wifi_msg_t structure that
//               will be filled on success
// @param timeout - timeout for the read
//
// @return - 0 on success, -1 on error or timeout
// @note caller must call wifi_msg_free() to free the
//       memory allocated for the message
int wifi_recv(wifi_msg_t *msg, k_timeout_t timeout);

//////////////////////////////////////////////////////////
// wifi_msg_free()
//
// Free the memory allocated for a wifi_msg_t
// @param msg - pointer to the wifi_msg_t to free
void wifi_msg_free(wifi_msg_t *msg);
int wifi_send(char *data);
//////////////////////////////////////////////////////////
// wifi_send_timeout
//
// Write an AT or ESC command to the DA16200.
//
//  @param data - pointer to buffer containing the AT command
//  @param timeout - timeout for the write
//
//  @return - 0 on success, -1 on error or timeout
int wifi_send_timeout(char *data, k_timeout_t timeout);
int wifi_init();
//////////////////////////////////////////////////////////
//  wifi_flush_msgs()
//
//  Flush all messages from the receive queue
void wifi_flush_msgs();

void wifi_set_rx_cb(wifi_on_rx_cb_t cb, void *user_data);

//////////////////////////////////////////////////////////
//	Requeue a message that was recv'd
//
// @param msg  - a wifi_msg_t that was obtained
//               from wifi_recv()
//
// @note caller can call this instead of wifi_msg_free()
void wifi_requeue(wifi_msg_t *msg);

#define WIFI_MAX_WAIT_MSGS       5
#define WIFI_MAX_WAIT_MSG_PARAMS 5

typedef struct wifi_wait_array {
	uint8_t num_msgs;
	char *msgs[WIFI_MAX_WAIT_MSGS];
	uint8_t num_params_per_msg[WIFI_MAX_WAIT_MSGS];
	char *param_ptrs[WIFI_MAX_WAIT_MSGS][WIFI_MAX_WAIT_MSG_PARAMS];
	bool stop_waiting[WIFI_MAX_WAIT_MSGS];
	int num_matched[WIFI_MAX_WAIT_MSGS];
} wifi_wait_array_t;

//////////////////////////////////////////////////////////
//  wifi_add_wait_msg()
//  Add a message to the structure holding the list of
//  messages to wait for to be passed to wifi_wait_for()
//
// @param wait_msgs - pointer to the wifi_wait_array_t
//                    structure to add the message to
// @param msg - pointer to a scanf format str to match
//              incoming messages against. The format
//              string must only contain %s for each parameter
// @param stop_waiting - true if wifi_wait_for() should stop
//                       waiting after this message is received
// @param num_params - number of parameters in the format string
// @param ... - pointers to the char buffers to be filled in
//              when the message is matched
void wifi_add_wait_msg(wifi_wait_array_t *wait_msgs, char *msg, bool stop_waiting, int num_params,
		       ...);

//////////////////////////////////////////////////////////
//	Wait for one of a few message to arrive, capturing
//  parameters
//
// @param wait_msgs - pointer to the wifi_wait_array_t
//                    holding what messager to wait for
// @param timeout - timeout for the read
//
// @return - index of the message that was matched, -1 on error or timeout
int wifi_wait_for(wifi_wait_array_t *wait_msgs, k_timeout_t timeout);

//////////////////////////////////////////////////////////
//	wifi_send_and_wait_for()
//  Send an cmd and wait for one of a few messages to
//  arrive in response, capturing parameters
//
// @param cmd - the atcmd
// @param wait_msgs - pointer to the wifi_wait_array_t
//                    holding what messager to wait for
// @param timeout - timeout for the whole operation
//
// @return - index of the message that was matched,
//           -1 on error or timeout
int wifi_send_and_wait_for(char *cmd, wifi_wait_array_t *wait_msgs, k_timeout_t timeout);

void wifi_set_print_txrx(bool print);
int wifi_msg_cnt();

int  wifi_1v8_on();
int wifi_1v8_off();
int wifi_set_power_key(bool newState);
int wifi_set_3v0_enable(bool newState);
int wifi_set_wakeup(bool newState);

// Utility functions used by non-shell code
int get_da_fw_ver(char *buf, int len);
int get_wfscan(char *buf, int len);
void wifi_do_work();

wifi_arr_t *get_ssid_list(k_timeout_t timeout);
int wifi_get_otp_register(int reg, int size, k_timeout_t timeout);
///////////////////////////////////////////////////////////////////////
// wifi_set_otp_register()
//  Send a command to the DA to write OTP memory only if the value
//  already there is 0.    OTP works on the DA at a bit level so future
//  writes cause a bitwise OR of old and new value.  So we read it first
//  and don't write it, if its not 0.
//
//  @param reg - the OTP register to read
//  @param size - the size of the register to read
//  @param timeout - timeout for the write
//
//  @return - the value of the register or -1 on error
int wifi_set_otp_register(int reg, int size, int newval, k_timeout_t timeout);

///////////////////////////////////////////////////////////////////////
// wifi_get_xtal()
//  Send a command to the DA to read the current XTAL value
//
//  @param timeout - timeout for the write
//
//  @return - the value of the register or -1 on error
int wifi_get_xtal(k_timeout_t timeout);

///////////////////////////////////////////////////////////////////////
// wifi_set_xtal()
//  Send a command to the DA to set the current XTAL value, temporarily
//
//  @param timeout - timeout for the write
//
//  @return - true on success, false on error or timeout
bool wifi_set_xtal(int newval, k_timeout_t timeout);

///////////////////////////////////////////////////////////////////////
// wifi_stop_XTAL_test()
//  Reboot the DA into XTAL normal
//
void wifi_stop_XTAL_test();

///////////////////////////////////////////////////////////////////////
// wifi_start_XTAL_test()
//  Reboot the DA into XTAL test mode
//
//  @param timeout - timeout for the write
//
//  @return - the value of the register or -1 on error
int wifi_start_XTAL_test();

/////////////////////////////////////////////////////////
// connect_to_ssid
//
// Connect to a ssid
//
// <ssid>: SSID. 1 ~ 32 characters are allowed
// <key>: Passphrase. 8 ~ 63 characters are allowed   or NULL if sec is 0 or 5
// <sec>: Security protocol. 0 (OPEN), 1 (WEP), 2 (WPA), 3 (WPA2), 4 (WPA+WPA2) ), 5 (WPA3 OWE), 6
// (WPA3 SAE), 7 (WPA2 RSN & WPA3 SAE) <keyidx>: Key index for WEP. 0~3    ignored if sec is 0,2-7
// <enc>: Encryption. 0 (TKIP), 1 (AES), 2 (TKIP+AES)   ignored if sec is 0,1 or 5
// <hidden>: 1 (<ssid> is hidden), 0 (<ssid> is NOT hidden)
// <timeout>: timeout
int connect_to_ssid(char *ssid, char *key, int sec, int keyidx, int enc, int hidden,
		    k_timeout_t timeout);

char *tde0002(); // get wifi fw version
char *tde0022(); // get the wifi mac address
char *tde0026(char *ssid, char *pass);
char *tde0027();           // get the wifi signal strength
char *tde0028();           // get the wifi connected status
char *tde0058();           // get the wifi connected status
char *tde0060();           // get the XTAL value
char *tde0061(int newval); // set the XTAL value temp
char *tde0062(int newval); // set the XTAL value perm
char *tde0063(int start);  // start or stop rf test mode
