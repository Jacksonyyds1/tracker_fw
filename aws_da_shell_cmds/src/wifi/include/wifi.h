
#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <zephyr/kernel.h>
#include <zephyr/sys_clock.h>

// The DA can send about 2K of data for large message but most will much 
// smaller. The wifi_spi will store a wifi_msg_t at the end of this buffer
// so it needs to be at least sizeof(wifi_msg_t)
#define WIFI_MSG_SIZE 2100

// When data is received, it is placed on the receive queue, and then a 
// pointer and length will be passed to the callback function 
typedef struct wifi_msg
{
    uint16_t    data_len;
    uint8_t     *data;
} wifi_msg_t;

// The callback function will be called when data is received
// Note that the message passed in is a copy of the last message
// received and there may be more more messages in the queue.
// If the callback wants to consume the message, it must call 
// wifi_spi_recv() and wifi_msg_free() to free the memory.  
typedef void (*wifi_on_rx_cb_t)(wifi_msg_t *msg, void *user_data);

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
int wifi_send_timeout(char *data, k_timeout_t timeout);
int wifi_init();
//////////////////////////////////////////////////////////
//  wifi_flush_msgs()
//
//  Flush all messages from the receive queue
void wifi_flush_msgs();

void wifi_set_rx_cb(wifi_on_rx_cb_t cb, void *user_data);

int wifi_power_on();
int wifi_power_off();
int wifi_set_power_key(bool newState);
int wifi_set_3v3_enable(bool newState);
int wifi_set_wakeup(bool newState);

// Utility functions used by non-shell code
int get_da_fw_ver(char *buf, int len);
int get_wfscan(char *buf, int len);

/////////////////////////////////////////////////////////
// connect_to_ssid
//
// Connect to a ssid
//
// <ssid>: SSID. 1 ~ 32 characters are allowed
// <key>: Passphrase. 8 ~ 63 characters are allowed   or NULL if sec is 0 or 5
// <sec>: Security protocol. 0 (OPEN), 1 (WEP), 2 (WPA), 3 (WPA2), 4 (WPA+WPA2) ), 5 (WPA3 OWE), 6 (WPA3 SAE), 7 (WPA2 RSN & WPA3 SAE)
// <keyidx>: Key index for WEP. 0~3    ignored if sec is 0,2-7
// <enc>: Encryption. 0 (TKIP), 1 (AES), 2 (TKIP+AES)   ignored if sec is 0,1 or 5
// <hidden>: 1 (<ssid> is hidden), 0 (<ssid> is NOT hidden)
// <timeout>: timeout
int connect_to_ssid(char *ssid, char *key, int sec, int keyidx, int enc, int hidden, k_timeout_t timeout);

char *tde0002();    // get wifi fw version
char *tde0022();    // get the wifi mac address
char *tde0026(char *ssid, char *pass);
char *tde0027();    // get the wifi signal strength
char *tde0028();    // get the wifi connected status