/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */

#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <zephyr/kernel.h>

#include "modem_interface_types.h"


// need a way to set:
//   - mqtt device name
//   - mqtt subscribe topics


int modem_get_status(modem_status_t *status);
int modem_get_version(version_response_t *ver);
int modem_get_time(char *buf, uint16_t len);

////////////////////////////////////////////////////
// modem_send_mqtt()
// Send a message to the modem to send a message to the MQTT broker
//
// Arguments:
//  topic: the topic to send the message to
//  topic_length: the length of the topic
//  message: the message to send
//  message_length: the length of the message
//  qos: the quality of service to use
//  timeout_in_ms: the timeout in milliseconds to wait for a response
//
// Returns:
//  0: success
//  -ENOTCONN: if mqtt is not connected
//  -ENOMEM: if memory allocation fails
//  -ETIMEDOUT: if a response is requested and the response times out
//  -EINVAL: if the topic or message length is less than 1
int modem_send_mqtt(
    char *topic, uint16_t topic_length, char *message, uint16_t message_length, uint8_t qos, uint16_t timeout_in_ms);
int modem_download_file_from_url(char *url, char *new_file_name);
int modem_fota_from_https(char *url, uint16_t url_length);
int modem_send_at_command(char *cmd, uint16_t cmd_length, char *response, uint16_t *response_length);

int modem_get_IMEI(uint8_t *buf, uint16_t len);
int modem_get_VERSION(uint8_t *buf, uint16_t len);
int modem_get_ICCID(uint8_t *buf, uint16_t len);
int modem_set_rffe_settings();
int modem_get_info(modem_info_t *info);

int modem_set_mqtt_params(char *host, uint16_t host_length, uint16_t port, char *client_id, uint16_t client_id_length);
int modem_set_mqtt_subscriptions(char *topics[], int qos[], uint16_t topics_length);
int modem_start_mqtt();
int modem_stop_mqtt();
int modem_send_reboot();
int modem_send_gps_enable();
int modem_send_gps_disable();
int modem_set_airplane_mode(bool on);
//
int  modem_recv(uint8_t *buf, k_timeout_t timeout);
int  modem_init();
void modem_set_rx_cb(void (*cb)(uint8_t *data, size_t len, void *user_data), void *user_data);
int  modem_power_on();
int  modem_power_off();
int  modem_send_command(modem_message_type_t type, uint8_t *data, uint16_t dataLen, bool reply_requested);
int  modem_recv_resp(uint8_t handle, uint8_t *data, uint16_t *dataLen, int timeout);
int  modem_free_reply_data(int handle);

int modem_send_gps_fakedata_disable();
int modem_send_gps_fakedata_enable();

version_response_t *get_cached_version_info();
int                 modem_get_rssi();
int                 modem_enable();
void                set_modem_power_state(bool on);
cell_info_t         modem_getCellInfo();
int                 modem_send_modemCellInfo(char *response, uint16_t *response_length);
int                 modem_stop_waiting_for_resp(int handle);
int                 modem_cancel_current_download();
int                 modem_upload_fw(char *fw_file);
bool                modem_is_powered_on();
int                 modem_set_dnsaddr(char *dns);