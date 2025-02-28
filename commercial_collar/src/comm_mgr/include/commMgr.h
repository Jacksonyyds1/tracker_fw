/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <zephyr/kernel.h>
#include <cJSON_os.h>
#include "d1_json.h"

////////////////////////////////////////////////////
// commMgr_queue_telemetry()
//  queue telemetry msg for sending the cloud
// when able
//
//  @param include_ssids true to include the SSIDs in the
//         telemetry message

//  @return 0 on success, <0 on error
int commMgr_queue_telemetry(bool include_ssids);

////////////////////////////////////////////////////
// commMgr_queue_pairing_nonce()
//  queue a pairing nonce msg to send to the cloud
// when able
//
//  @param nonce the nonce to send
//
//  @return 0 on success, <0 on error
int commMgr_queue_pairing_nonce(char *nonce);

////////////////////////////////////////////////////
// commMgr_queue_connectivity()
//  queue a connectivity msg for sendign to the cloud
// when able.
//
//  @param num_bytes the number of bytes to send
//
//  @return 0 on success, <0 on error
int commMgr_queue_connectivity(int num_bytes);

////////////////////////////////////////////////////
// commMgr_set_time()
//  Set the time based on a time/date string in the
// format of:
// shell   [Y-m-d] <H:M:S>
// DA -    +TIME:2024-04-03,18:52:35
// 9160  - 24/04/03,18:52:38-28
//
//  @param time the time string to set
//
//  @return 0 on success, <0 on error
int commMgr_set_time(char *time);

////////////////////////////////////////////////////
// commMgr_get_unix_time()
//  Get the current unix time. Time must be set
//
//  @return the current unix time
uint64_t commMgr_get_unix_time();

////////////////////////////////////////////////////
// commMgr_enable_S_work()
//  Enable or disable the S var work, like
// scanning for SSIDs, sending telemetry, etc
//
//  @param enable true to enable, false to disable
//
//  @return 0 on success, <0 on error
int commMgr_enable_S_work(bool enable);

////////////////////////////////////////////////////
// commMgr_enable_Q_work()
//  Enable or disable the work that sends mqtt msgs
// that are queued to send.
//
//  @param enable true to enable, false to disable
//
//  @return 0 on success, <0 on error
int commMgr_enable_Q_work(bool enable);

////////////////////////////////////////////////////
// commMgr_queue_alert()
//  queue a alert msg for sending to cloud when able
//
//  @param sub_type the type of alert
//  @param msg the message to send
//
//  @return 0 on success, <0 on error
int commMgr_queue_alert(int sub_type, char *msg);

///////////////////////////////////////////////////
// commMgr_queue_onboarding()
//  queue a onboarding msg for sending to the cloud
// when able
//
//  @return 0 on success, <0 on error
int commMgr_queue_onboarding();

///////////////////////////////////////////////////////////
// commMgr_set_vars()
//  Set the S and T related variables which affects
// the time between when we get SSIDs, telementry
// and AP reconnect
//
//  S_ values are in seconds between 15 < S < 36000
//  T_ values are in numbers of S periods between 1 < T < 10000
//  REC is in seconds between 15 < REC < 36000
//  Q is in seconds between 15 < Q < 10000

//  @param S_Norm the time between SSID scans in normal mode
//         or 0 for no change
//  @param S_FMD the time between SSID scans in FMD mode or
//         0 for no change
//  @param T_Norm number of S periods between telemetry send
//          in Normal mode or 0 for no change
//  @param T_FMD number of S periods between telemetry send
//          in FMD mode or 0 for no change
//  @param Rec the new period in seconds between reconnects
//          checks when on LTE or 0 for no change
//  @param Q the time between attempts to send queued mqtt
//          messages or 0 for no change
//  @param save true to save the new values to flash
//
//  @return 0 on success, <0 on error
int commMgr_set_vars(int S_Norm, int S_FMD, int T_Norm, int T_FMD, int Rec, int Q, bool save);

///////////////////////////////////////////////////
// commMgr_send_shadow()
//  send a shadow message to the cloud using the
// active radio. THis will report our current
// settings.   If extra is provided then
// the variable definitions in that array will
// also be sent.  THis is to allow for deleting
// old variables from the IoT Hub
//
//  @return 0 on success, <0 on error
int commMgr_send_shadow(shadow_extra_t *extra);

////////////////////////////////////////////////////
// commMgr_queue_safe_zone_alert()
//
//  @return 0 on success, <0 on error
int commMgr_queue_safe_zone_alert(uint64_t time, int type, char *ssid, bool entering);

////////////////////////////////////////////////////
// commMgr_queue_mqtt_message()
//  queue a message to be sent to the cloud at the
// next opportunity
//
//  @param msg the message to send
//  @param msg_len length of the message
//  @param topic the topic to send the message to
//  @param topic_len length of the topic
//  @param qos the quality of service to use
//  @param priority is a number indicating the order
//      to free unsent message in case of limited
//      memory. 1 means drop last, higher means drop
//      first
//
//  @return 0 on success, <0 on failure
int commMgr_queue_mqtt_message(uint8_t *msg, uint16_t msg_len, uint8_t topic_num, uint8_t qos, uint8_t priority);

///////////////////////////////////////////////////
// commMgr_queue_shadow()
//  queue a shadow message to be sent to the cloud
// using the when able. THis will report our current
// settings.   If extra is provided then
// the variable definitions in that array will
// also be sent.  THis is to allow for deleting
// old variables from the IoT Hub
//
//  @return 0 on success, <0 on error
int commMgr_queue_shadow(shadow_extra_t *extra);

////////////////////////////////////////////////////
// commMgr_fota_start()
// Make sure the DA radio is ready to do a fota.
// We need to keep the DA awake while the fota process
// is going on.
//  @param device the device to start the fota on
//
//  @return 0 on success, <0 on error
int  commMgr_fota_start(comm_device_type_t device);
void commMgr_fota_end(comm_device_type_t device);
bool commMgr_fota_in_progress();

typedef enum
{
    FMD_ACTIVE        = 0,
    FMD_TIMEOUT       = 1,
    FMD_BATTERY       = 2,
    FMD_SAFE          = 3,
    FMD_CLOUD_REQUEST = 4,
    FMD_NOT_ACTIVE,
} fmd_exit_t;

////////////////////////////////////////////////////
// Intenal/util functions
bool                is_9160_lte_connected();
bool                status_getBit(int status, int pos);
int                 write_shadow_doc();
extern shadow_doc_t shadow_doc;
int                 commMgr_switched_to_wifi();
int                 commMgr_switched_to_lte();
int                 enable_fmd_mode(int fmd_max_time);
int                 disable_fmd_mode(fmd_exit_t);
fmd_exit_t          fmd_status(void);
