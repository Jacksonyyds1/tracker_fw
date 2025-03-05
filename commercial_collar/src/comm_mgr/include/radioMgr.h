/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <zephyr/kernel.h>
#include <cJSON_os.h>
#include "commMgr.h"

extern bool g_bt_connected;
extern bool g_usb_connected;
extern bool g_usb_bt_prepped_radio;    // If we ever prepped the radio based on USB/BT state
////////////////////////////////////////////////////
// rm_get_active_mqtt_radio()
//  Return the radio that is ready to be used for
// MQTT communications to the server.  Since the
// state of the radios can change at any time, any
// message sent could still fail to send.
//
//  @return the radio that is ready to be used for
// MQTT communications to the server
comm_device_type_t rm_get_active_mqtt_radio();

////////////////////////////////////////////////////////////
// rm_prepare_radio_for_use()
//   Make sure the radio is ready to use.
//
// For the DA:
//   If it was sleeping, wake it. If MQTT is needed, check
// if the broker is connected. This is done in a way that
// respects if sleep is enabled so that if we choose not to
// use sleep mode, this will keep the DA awake.
//
// For LTE:
//   Wait for the MQTT to be connected before returning. In
// stable LTE conditions, this should be immediate, but
// in unstable conditions, waiting for it makes the next
// message send more likely to succeed.
//
// This prep work should be done once be group of messages
// sent at time since waking and sleeping or waiting can
// use power and time. Most message sends are one offs so
// to allow lower level message send functions to use this
// for individual message and higher level message grouping
// code to use it, we keep a reference count of the number
// of times this is called and only put the radio back to
// sleep when the last call is done.
//
// The ref counts are per radio, as such, a call to
// rm_prepare_radio_for_use() must be matched one for one
// with a call to rm_done_with_radio() for each radio
//
//  @param device the device to prepare
//  @param need_mqtt if true, then we need to make sure
// MQTT is connected
//  @param timeout how long to wait for the radio to be ready
//
//  @return  bool if the wifi is ready to use
bool rm_prepare_radio_for_use(comm_device_type_t device, bool need_mqtt, k_timeout_t timeout);

////////////////////////////////////////////////////
// rm_done_with_radio()
// Called when we are done with the radio. If its
// the DA, we wantto put it back to sleep (if dpm
// is enabled), if its the 9160, we do nothing
//
//  @return 0 on success, -1 on failure
int rm_done_with_radio(comm_device_type_t device);

////////////////////////////////////////////////////
// rm_ready_for_mqtt()
//  Return true if there is a radio that is ready
// to send MQTT messages.  This checks not only if
// there is an active radio, but whether the radio
// reports it is connected to the MQTT broker at
// the moment.
//
//  @return bool if there is a radio ready to send MQTT
bool rm_ready_for_mqtt();

////////////////////////////////////////////////////
// rm_switch_to()
//  Switch to the radio specified.  If the radio is
// already active, then nothing is done.  If the radio
// is not active, then the radio is switched to. if
// the radio being switch to is different then the
// newly requested radio, then we start trying to
// switch to the new radio.
//
// @param radio the radio to switch toS
// @param clear_existing_radio set active radio to None if true
// @param force_switch if true, then set all vars as if
//        the switch succeeded (dev only)
//
//
//  @return 0 on success, -1 on failure
int rm_switch_to(comm_device_type_t radio, bool clear_existing_radio, bool force_switch);

////////////////////////////////////////////////////
// rm_is_switching_radios()
//  Return whether the radio manager is switch
bool rm_is_switching_radios(void);

////////////////////////////////////////////////////
// rm_set_reconnect_timer()
//  Set the timer parameters for the when reconnect
// work is done.
//
//  @param new_dur the new duration of the work
//  @param new_per the new period of the work
//  @param save true to save the new values to flash
//
//  @return 0 on success, <0 on error
int rm_set_reconnect_timer(int new_dur, int new_per, bool save);

////////////////////////////////////////////////////
// rm_enable()
//  Enable or disable the radio manager state
// machine and reconnect task.
//
//  @param enable true to enable, false to disable
void rm_enable(bool enable);

////////////////////////////////////////////////////
// rm_is_enabled()
//  Return if the radio manager is enabled
//
bool rm_is_enable();

////////////////////////////////////////////////////
// rm_is_wifi_enabled()
//  Return if the radio manager is enabled to use
// the DA
//
bool rm_is_wifi_enabled();

////////////////////////////////////////////////////
// rm_wifi_enable()
//  Enable or disable the radio manager from using
// the DA.  commMgr also checks this value for ssid
// scans
//
//  @param enable true to enable, false to disable
void rm_wifi_enable(bool use_wifi);

////////////////////////////////////////////////////
// rm_use_sleep()
//  Enable or disable the radio manager to sleep the
// radios when not in use
//
//  @param enable true to enable, false to disable
void rm_use_sleep(bool enable);

////////////////////////////////////////////////////
// rm_uses_sleep()
//  Return if the radio manager is using sleep
//
bool rm_uses_sleep();

////////////////////////////////////////////////////
// rm_wifi_is_connecting()
//  Return if the radio manager is using sleep
//
bool rm_wifi_is_connecting();

////////////////////////////////////////////////////
// Development
char *rm_op_str();

////////////////////////////////////////////////////
// rm_is_active_radio_mqtt_connected()
//  Return if the active radio is connected to the
// MQTT broker
bool rm_is_active_radio_mqtt_connected();

void rm_got_UC_from_AP();

////////////////////////////////////////////////////
// rm_connect_to_AP()
//  Connect to the AP with the given ssid and password
//
//  @param ssid the ssid of the AP to connect to
//  @param password the password of the AP to connect to
//  @param sec the security type of the AP
//  @param keyidx the key index of the AP
//  @param enc the encryption type of the AP
//
int rm_connect_to_AP(char *ssid, char *password, uint16_t sec, uint16_t keyidx, uint16_t enc);
int rm_connect_to_AP_by_index(int idx);