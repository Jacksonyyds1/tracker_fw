/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
/****************************************************************
 *  wifi_at.h
 *  SPDX-License-Identifier: LicenseRef-Proprietary
 *
 * The wifi code is layered as follows:
 *     wifi_spi.c or wifi_uart.c - Talked to hardware to transfer data to and from the DA1200
 *     wifi.c - HW independent layer to send adn receive messages to and from the DA1200
 *     wifi_at.c - AT command layer to send and receive AT commands to and from the DA1200
 *     net_mgr.c - The network (wifi and lte) api layer, manages the state of the da and
 *                 lte chip and network communication state machine and publishes zbus
 *                 message when revevent stuff happens
 *     wifi_shell.c - A collection of shell commands to test and debug during development
 */
#pragma once
#include "uicr.h"
#include "d1_json.h"
#include <zephyr/kernel.h>

#define MAX_SSIDLIST_RESPONSE ((50 * 5) + 14 + 10)
// if we are not connected then the RSSI is unavailable.  We use 100
// which shouldn't every be possible to represent this
#define RSSI_NOT_CONNECTED (100)
extern shadow_doc_t shadow_doc;
extern wifi_arr_t   g_last_ssid_list;
extern uint64_t     g_last_dpm_change;
extern bool         g_awake_on_boot;
extern uint64_t     g_last_sleep_time;

typedef enum
{
    WIFI_SLEEP_NONE = 0,
    WIFI_SLEEP_DPM_ASLEEP,
    WIFI_SLEEP_DPM_AWAKE,
    WIFI_SLEEP_RTC_ASLEEP
} wifi_sleep_state_t;

typedef enum
{
    DA_STATE_UNKNOWN     = -1,
    DA_STATE_KNOWN_FALSE = 0,
    DA_STATE_KNOWN_TRUE  = 1,
} das_tri_state_t;

/////////////////////////////////////////////////////////
// wifi_get_da_fw_ver()
//
// Get the DA firmware version
//
// @param fwver - pointer to buffer to store the version
// @param len - length of the buffer
//
//  @return - 0 on success
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping
int wifi_get_da_fw_ver(char *fwver, int len, k_timeout_t timeout);

/////////////////////////////////////////////////////////
// wifi_get_wfscan()
//
// Get an list of SSIDs seen by the DA
//
// @param buf - pointer to buffer to store the version
// @param len - length of the buffer
//
//  @return - 0 on success
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping
int wifi_get_wfscan(char *buf, int len, k_timeout_t timeout);

/////////////////////////////////////////////////////////
// wifi_refresh_ssid_list()
//
// refresh the locally cached list of SSIDs seen by the DA
// if the age of the list is older then the parameter
// passed in.
//
// @param skip_hidden - if true, don't include ssid without name
// @param max_age_sec - max age of the list in seconds
// @param timeout - timeout for the operation to complete
//
//  @return - 0 on success
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping
int wifi_refresh_ssid_list(bool skip_hidden, int max_age_sec, k_timeout_t timeout);

/////////////////////////////////////////////////////////
// wifi_get_last_ssid_list()
//
// Get the list of SSID gathered in a previous scan
wifi_arr_t *wifi_get_last_ssid_list();

///////////////////////////////////////////////////////////////////////
// wifi_send_ok_err_atcmd()
//  Send a command to the DA and wait for a OK or ERROR response.
//
//  @param cmd - the command to send
//  @param errret - a buffer to hold any error text, min size 20
//					or null
//  @param timeout - timeout for the write
//
//  @return - 0 if OK was received
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EBUSY if mutex failure
int wifi_send_ok_err_atcmd(char *cmd, char *errstr, k_timeout_t timeout);

///////////////////////////////////////////////////////////////////////
// wifi_mqtt_publish
//  Publish a message to the MQTT broker. If the wait_for_snd_conf
// is true, then the funtion call will wait for the send confirmation
// from the DA. This send confirmation is depends on the QoS level
// of the message and can take a while.
//
//  @param message_type - the type of the message 1-999
//  @param msg - the message to publishS
//  @param wait_for_snd_conf - wait for the send confirmation
//  @param timeout - timeout for the entire operation
//
//  @return - 0 on success, < 0 on error
//				errors that are transiant and can be tried again
//				-EINTR if timeout
//				-EAGAIN if wifi mutex failure
//				-ENOTCONN if broker isn't connected
//				-ENOMEM if memory allocation failed
//
//				errors in the message that are not transiant
//			    -EINVAL if message_type is invalid
//				-EFBIG if the message is too large
int wifi_mqtt_publish(uint16_t message_type, char *msg, bool wait_for_send_conf, k_timeout_t timeout);

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
// wifi_get_otp_register()
//  Send a command to the DA to read OTP memory and return it.
//  This command has a non-standard response in that it comes
//  back in multiple parts
//
//  @param reg - the OTP register to read
//  @param size - the size of the register to read
//  @param timeout - timeout for the write
//
//  @return - the value of the register or -1 on error
int64_t wifi_get_otp_register(int reg, int size, k_timeout_t timeout);

///////////////////////////////////////////////////////////////////////
// wifi_get_nvram()
//  Send a command to the DA to read a portion of nvram memory and
//  return it.
//  This command has a non-standard response in that it comes
//  back in multiple parts
//
//  @param addr - the address in nvram to read
//  @param buf - where to put the data
//  @param size - the size of the buf/data
//  @param timeout - timeout for the write
//
//  @return - 0 on success or -1 on error
int wifi_get_nvram(uint32_t addr, uint8_t *buf, int size, k_timeout_t timeout);

///////////////////////////////////////////////////////////////////////
// wifi_put_nvram()
//  Send a command to the DA to write data to DA's nvram memory.
//	Note that the data buffer is a string of the hex values
//
//  @param addr - the address in nvram to read
//  @param buf - A string containing the hex values to write
//               i.e. "0102FFAABBCC"
//  @param timeout - timeout for the write
//
//  @return - 0 on success or -1 on error
int wifi_put_nvram(uint32_t addr, uint8_t *buf, k_timeout_t timeout);

///////////////////////////////////////////////////////////////////////
// wifi_get_mac()
//  Send a command to the DA to read the current MAC Addr
//
//  @param which - the address of an int that will receive
//					which kind of a mac address is being used
//					0 = user, 1 = spoof, 2 = OTP
//  @param timeout - timeout for the write
//
//  @return - the value of the register or NULL on error
char *wifi_get_mac(int *which, k_timeout_t timeout);

///////////////////////////////////////////////////////////////////////
// wifi_set_mac()
//  Send a command to set MAC addre the DA uses
//  The docs and the behaviour doesn't match. I looked at the src and
//  I think its complicated why it doesn't.  Also close examination of
//	the docs reveal that it probably can't work that way.
//
//	However, it does work for what we need so if this code is odd,
//  don't sweat it.
//
//  @param newmac - a str holding the new mac "XX:XX:XX:XX:XX:XX"
//  @param timeout - timeout for the write
//
//  @return - 0 if succeeded, -1 on error or timeout
int wifi_set_mac(char *newmac, k_timeout_t timeout);

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
//  @param errstr - a buffer to hold any error text, min size 20
//					or null
//  @param timeout - timeout for the write
//
//  @return - 0 if OK was received
//			 -1 if timeout
//			 -2 if ERROR was received
//			 -3 if mutex failure
int wifi_set_xtal(int newval, char *err, k_timeout_t timeout);

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

/////////////////////////////////////////////////////////////////////////
// wifi_wake_no_sleep()
//
// Wake the DA from sleep and keep it from sleeping.
// If the DA is not in DPM mode nor sleeping, the it won't respond to
// the WAKEUP line, so we will time out.  Otherwise we will see a msg.
// If we were in DPM we also need to send a series of cmds to keep it
// from returning to DPM sleep.
//
//  @param timeout - timeout for the write
//
//  @return : 0 on success
//			  < 0 on error
int wifi_wake_no_sleep(k_timeout_t timeout);

////////////////////////////////////////////////////////////////////////////////
// wifi_dpm_back_to_sleep()
//
// Tell the DA it can go back to sleep if it was woken up from dpm mode
//
//  @param timeout - timeout for the write
//
//  @return - < 0 = error, 0 = success
int wifi_dpm_back_to_sleep(k_timeout_t timeout);

/////////////////////////////////////////////////////////
// wifi_set_sleep_mode()
// Set the DA16200 DPM mode or sleep state regardless of
// what the current state is.
//
// @param sleep_state - 0 = off
//					    1 = DPM mode, asleep
//				 	    2 = DPM mode, awake
//					    3 = RTC sleep for X seconds
// @param dur_ms - duration in ms for RTC sleep
//
// @return - 0 on success, -1 on error
int wifi_set_sleep_mode(wifi_sleep_state_t sleep_state, int dur_ms);

/////////////////////////////////////////////////////////
// wifi_initiate_connect_to_ssid
//
// Initiate a connect to a ssid.  This call starts a
// connection attempt to a SSID provided.  Connecting
// take 45 seconds to fail or succeed.  The timeout
// provided is only the time to wait for the command
// to be acknowledged by the DA.  An OK means that the
// DA finds all the parameters acceptable and will
// try to find and connect to that AP regardless of
// whether it is present or not.  ERROR means that
// something about the parameters is wrong.  The timeout
// for getting OK or ERROR should be less then 1 second.
//	 When the connection is made or failes, the DA will
// send us a async event which will result in a Zbus
// message being sent to the DA state channel.
//
// <ssid>: SSID. 1 ~ 32 characters are allowed
// <key>: Passphrase. 8 ~ 63 characters are allowed
//                    or NULL if sec is 0 or 5
// <sec>: Security protocol. 0 (OPEN), 1 (WEP), 2 (WPA),
//                           3 (WPA2), 4 (WPA+WPA2),
//							 5 (WPA3 OWE), 6 (WPA3 SAE),
// 							 7 (WPA2 RSN & WPA3 SAE)
// <keyidx>: Key index for WEP. 0~3
//		     ignored if sec is 0,2-7
// <enc>: Encryption. 0 (TKIP), 1 (AES), 2 (TKIP+AES)
//        			  ignored if sec is 0,1 or 5
// <hidden>: 1 (<ssid> hidden), 0 (<ssid> NOT hidden)
// <timeout>: timeout
//  @return - 0 on success
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EINVAL if new_topics if badly formed
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping or mqtt broker is connected
int wifi_initiate_connect_to_ssid(char *ssid, char *key, int sec, int keyidx, int enc, int hidden, k_timeout_t timeout);

/////////////////////////////////////////////////////////////////////////
// wifi_set_ntp_server()
//
// Set the NTP server for the DA to pool.ntp.org
//
// @return : 0 on success, -1 on error
int wifi_set_ntp_server();

////////////////////////////////////////////////////////////
// wifi_set_mqtt_sub_topics()
//
// Set the list of topics that the MQTT service subscribes
// to. This does not check if the topics are already
// to or merge with existing topics.
//
//  @param new_topics - The list of topic strings ptrs.
//                      The last ptr in the list must be null
//  @param timeout - timeout for the write
//
//  @return - 0 on success, -1 on error or timeout
int wifi_set_mqtt_sub_topics(char *new_topics[], k_timeout_t timeout);

////////////////////////////////////////////////////////////
// wifi_insure_mqtt_sub_topics()
//
// Make sure that the list of topics include those
// passed in
//
//  @param new_topics - The list of topic strings ptrs.
//                      The last ptr in the list must be null
//  @param timeout - timeout for the write
//
//  @return - 0 if sub_topics are all in subscribed to
//            <0 on error
int wifi_insure_mqtt_sub_topics(char *desired_topics[], k_timeout_t timeout);

////////////////////////////////////////////////////
// wifi_get_dpm_state()
//  Retreive the dpm state
//
//  @param timeout - timeout for the write
//
//  @return - current dmp state (0|1) or
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EBUSY if mutex failure
int wifi_get_dpm_state(k_timeout_t timeout);

////////////////////////////////////////////////////
// wifi_set_dpm_state()
//  Set the dpm state
//
//  @param timeout - timeout for the write
//  @param new_state - the new state to set
//  @param awake_on_boot - bool whether theD DA
//				 should stay away after reboot
//
//  @return - 0 on success
//			 -1 if timeout
//			 -2 if ERROR was received
//			 -3 if mutex failure
int wifi_set_dpm_state(uint8_t new_state, bool awake_on_boot, k_timeout_t timeout);

////////////////////////////////////////////////////
// wifi_get_mqtt_state()
//  Retreive the mqtt current state
//
//  @param timeout - timeout for the write
//
//  @return - current mqtt state (0|1) or
//			 -1 if timeout
//			 -2 if ERROR was received
//			 -3 if mutex failure
int wifi_get_mqtt_state(k_timeout_t timeout);

////////////////////////////////////////////////////
// wifi_set_mqtt_state()
//  Set the mqtt state. Safe to call if the mqtt
// state is already in the desired state
//
//  @param timeout - timeout for the write
//  @param new_state - the new state to set
//
//  @return - 0 on success
//			 -1 if timeout
//			 -2 if ERROR was received
//			 -3 if mutex failure
int wifi_set_mqtt_state(uint8_t new_state, k_timeout_t timeout);

////////////////////////////////////////////////////
// wifi_get_mqtt_boot_state()
//  Retreive the mqtt state on DA boot
//
//  @param timeout - timeout for the write
//
//  @return - current mqtt state (0|1) or
//			 -1 if timeout
//			 -2 if ERROR was received
//			 -3 if mutex failure
int wifi_get_mqtt_boot_state(k_timeout_t timeout);

////////////////////////////////////////////////////
// wifi_set_mqtt_boot_state()
//  Set the mqtt state on DA boot. Safe to call if
// the state is already in the desired state
//
//  @param timeout - timeout for the write
//  @param new_state - the new state to set
//
//  @return - 0 on success
//			 -1 if timeout
//			 -2 if ERROR was received
//			 -3 if mutex failure
int wifi_set_mqtt_boot_state(uint8_t new_state, k_timeout_t timeout);

///////////////////////////////////////////////////////////////////////
// wifi_get_rssi()
//  Send a command to the DA to get the current RSSI for the current
//  AP
//
//  @param rssi - a pointer to an in to return RSSI in
//  @param timeout - timeout for the write
//
//  @return >= 0  - RSSI of the register
// 			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EINVAL if new_topics if badly formed
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping or mqtt broker is connected
int wifi_get_rssi(int *rssi, k_timeout_t timeout);

/////////////////////////////////////////////////////////////////////////
// wifi_check_sleeping()
//
// Non-destructively check to see of the DA is sleeping. This does not
// discover whether the the DA is in DPM or RTC sleep mode.  It only
// checks to see if the DA is in sleeping.
//
//  @param change_state - if true, then the state of the DA will be
//						  changed to the state detected
//  @return : the state of the da sleep
das_tri_state_t wifi_check_sleeping(bool change_state);

/////////////////////////////////////////////////////////////////////////
// wifi_check_sleep_mode()
//
// Destructively check to see of the DA is in DPM mode.
// We send a wake up signal to the DA and look for the response if any.
// What happens depeneds on the mode that the DA is in:
// 1. If the DA is in DPM mode and asleep, it will respond with a
//	  \r\n+INIT:WAKEUP,<type>\r\n message.
// 2. If the DA is in DPM mode and awake, it will respond with a
//    \r\n+RUN:RTCWAKEUP\r\n
// 3. If the DA is NOT in DPM mode, then it will not respond at all
//
//  @return : 0 when not in DPM mode
//			  1 when in DPM mode and asleep
//			  2 when in DPM mode and awake
//			  3 when in RTC sleep and now awake
int wifi_check_sleep_mode();

/////////////////////////////////////////////////////////////////////////
// wifi_http_get()
//
// Retrieve the contents of a http or https get into a little fs file
//
//  @param url - the url to get.  all the way from http...<file>
//  @param filename - the file to writ the contents to in littlefs
//						note that "lfs1/" is prepended to the filename
//  @param skip_headers - if true, skip the headers
//
//  @return : 0 if it succeeded
//			  <0 on timeout or error
int wifi_http_get(char *url, char *filename, bool skip_hearders, k_timeout_t timeout);

int wifi_set_mqtt_sub_topics_by_type(int types[], int cnt, k_timeout_t timeout);

/////////////////////////////////////////////////////////////////////////
// wstrerr()
//
// Return a string representation of an error code
//
//  @param err - the error code
//
//  @return : a string representation of the error code
const char *wstrerr(int err);

/////////////////////////////////////////////////////////////////////////
// wifi_rtc_sleep()
//
// Put the DA into RTC sleep mode.  This is a low power mode that
// the DA can be put into.  It is not the same as DPM but can be
// used in conjunction with DPM.
//
//  @return : 0 on success
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EINVAL if new_topics if badly formed
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping or mqtt broker is connected
int wifi_rtc_sleep(int ms_to_sleep);

///////////////////////////////////////////////////////////////////
// wifi_set_ap_profile_use()
//
//	Enable or disable the use of the AP profile (credentials) that
//  the DA16200 has saved in it memory (the last AP connected to).
//
//  @param use_profile - true to use the profile, false to not
//
//  @return : 0 on success
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EINVAL if new_topics if badly formed
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping or mqtt broker is connected
int wifi_set_ap_profile_use(bool use_profile, k_timeout_t timeout);

/////////////////////////////////////////////////////////
// wifi_check_for_known_ssid
//
// Find the index in the "known SSID list" of an SSID
// that appears in the last SSID scan.
//
// Return the known SSID index with the strongest RSSI
// giving preference to the known SSIDs marked Safe.
//
// If no known SSID are in the scan, return -1
//
//  @return - index of the first known ssid found
//			 -1 if none are found
int wifi_check_for_known_ssid();

/////////////////////////////////////////////////////////
// wifi_find_saved_ssid()
//  Find the index of the ssid in the list the ssids the
// DA has credentials for.
//
//  @param ssid - the ssid to look for
//
//  @return - index of the ssid in the list or -1
//		 if not found
int wifi_find_saved_ssid(char *ssid);

/////////////////////////////////////////////////////////S
// wifi_num_saved_ssids()
//  Count the number of ssids the DA has credentials for
//
//  @return - the number of ssids
int wifi_num_saved_ssids();

/////////////////////////////////////////////////////////
// wifi_get_saved_ssid_by_index()
//  return the ssid name from the saved ssid list at the
// specified index
//
//  @param idx - the index into the save ssid
//
//  @return - pointe to the name of the ssid (may be null)
char *wifi_get_saved_ssid_by_index(int idx);

////////////////////////////////////////////////////////////
// wifi_clear_local_ssid_list
void wifi_clear_local_ssid_list();

/////////////////////////////////////////////////////////
// wifi_initiate_connect_by_index
//	Connect to the SSID specified by an index in the SSID
//  list held in the DA16200
//
//  @param idx - index of the SSID to connect to
//  @param timeout - timeout for the connect
int wifi_initiate_connect_by_index(int idx, k_timeout_t timeout);

//////////////////////////////////////////////////////////
// wifi_disconnect_from_ap()
//  Disconnect from the current AP
//
//  @param timeout - timeout for the disconnect
int wifi_disconnect_from_AP(k_timeout_t timeout);

/////////////////////////////////////////////////////////
// wifi_saved_ssid_name()
//  Get the safe flag for the index into the known SSID
// list
//
//  @param idx - the index of the ssid to get the name of
//
//  @return - the safe flag of the ssid
bool wifi_saved_ssid_safe(int idx);

//////////////////////////////////////////////////////////
// wifi_is_curr_AP()
//  Check if the current AP is the same as the one passed in
//
//  @param ssid - the ssid to check
//  @param password - the password to check
//  @param sec - the security to check
//  @param keyidx - the key index to check
//  @param enc - the encryption to check
//
//  @return : 0 if the current connection is the same SSID and creds
//			  1 if the current connection is the same SSID but different creds
//		      2 if the current connection is a different SSID
int wifi_is_curr_AP(char *ssid, char *password, uint16_t sec, uint16_t keyidx, uint16_t enc);

/////////////////////////////////////////////////////////
// wifi_get_ap_list()
//   Retrieve the list of SSIDs stored on the DA and
// store them locally for reference.  This call will
// not ask the DA again if it already has the list since
// it can't changes unless we change it.
//
//  @param zones - a pointer to the array of shadow zones
//  @param timeout - timeout for the get
int wifi_get_ap_list(shadow_zone_t *zones, k_timeout_t timeout);

////////////////////////////////////////////////////////////
// wifi_saved_ssids_add()
//
// Add a saved_ssid to the list of known ssids on the DA
//
// @param idx - slot index to change or -1 for first free
// @param name - the ssid name
// @param pass - the password
// @param sec - the security type
// @param keyidx - the key index
// @param enc - the encryption type
// @param hidden - true if the ssid is hidden
// @param safe - true if the ssid is in a safe zone
// @param timeout - timeout for the operation
//
// @return - 0 on success, -E2BIG if the list is full
int wifi_saved_ssids_add(
    int idx, char *name, char *pass, uint16_t sec, uint16_t keyidx, uint16_t enc, bool hidden, bool safe, k_timeout_t timeout);

////////////////////////////////////////////////////////////
// wifi_saved_ssids_del()
//
// delete a saved_ssid from the list
//
// @param idx - the index of the ssid to delete
// @param timeout - timeout for the operation
//
// @return - 0 on success, -EINVAL if the index is out of range
int wifi_saved_ssids_del(int idx, k_timeout_t timeout);

////////////////////////////////////////////////////////////
// wifi_saved_ssids_del_all()
//
// delete all saved_ssids from the list
//
// @param timeout - timeout for the operation
//
// @return - 0 on success, -EINVAL if the index is out of range
int wifi_saved_ssids_del_all(k_timeout_t timeout);

////////////////////////////////////////////////////////////
// wifi_set_zone_safe()
//
// delete a saved_ssid from the list
//
// @param idx - the index of the ssid to delete
// @param safe_flag - true to set safe, false to unset
// @param timeout - timeout for the operation
//
// @return - 0 on success, -EINVAL if the index is out of range
int wifi_set_zone_safe(int idx, bool safe_flag, k_timeout_t timeout);

/////////////////////////////////////////////////////////////////////////
// wifi_time_to_next_wake()
//   Return the number of ms before we will be allowed to wake the DA
// again.  THis could be over-cautious, but if we wake the DA too soon
// after we just slept it, it will ignore the wake up signal be cause it
// isn't sleeping and we get out of sync
//
// Use this call to check if enough time has passed to do another wake
//
//  @return : the number of ms before we can wake the DA
int wifi_time_to_next_wake();

/////////////////////////////////////////////////////////////////////////
// wifi_at_get_http_amt_downloaded()
//   Return the number of bytes downloaded in the last http get
//
//  @return : the number of bytes downloaded
int wifi_at_get_http_amt_downloaded();

/////////////////////////////////////////////////////////////////////////
// wifi_get_wfstat()
//
//	Get the wifi status of the DA
//
//  @param timeout - timeout for the command
//
//  @return : 0 if disconnected
//			  1 on success
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
int wifi_get_wfstat(k_timeout_t timeout);

//// DEV ONLY
int wifi_add_SSID_to_cached_list(char *line);