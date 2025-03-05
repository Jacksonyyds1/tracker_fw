/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
/****************************************************************
 *  wifi_at.c
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
#include "wifi.h"
#include "wifi_at.h"
#include "commMgr.h"
#include "net_mgr.h"
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/zbus/zbus.h>
#include "d1_json.h"
#include <zephyr/sys/crc.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include "utils.h"

LOG_MODULE_REGISTER(wifi_at, CONFIG_WIFI_AT_LOG_LEVEL);

#define HTTPFLEN 100
#define MAXURL   1900
uint32_t         http_crc                = 0;
char             httpgetcmd[MAXURL + 40] = "";
char             httpfilename[HTTPFLEN]  = "";
struct fs_file_t httpfd;
bool             file_opened       = false;
int              httpresultcode    = 0;
bool             http_skip_headers = false;
long             http_amt_written  = 0;
uint64_t         g_last_dpm_wake   = 0;
uint64_t         g_last_sleep_time = 0;

uint64_t g_last_dpm_change = 10000;
bool     g_awake_on_boot   = false;

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
//			 -ENXIO if in sleeping state and DA won't respond
int wifi_send_ok_err_atcmd(char *cmd, char *errret, k_timeout_t timeout)
{
    int               result;
    char              errtmp[20];
    char              mutexinfo[100];
    wifi_wait_array_t wait_msgs;
    k_timepoint_t     timepoint = sys_timepoint_calc(timeout);

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("Can't send %20s while da is powered off", cmd);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("Can't send %20s while da is sleeping", cmd);
        return -ENXIO;
    }

    snprintf(mutexinfo, 99, "wifi_send_ok_err_atcmd:%s", cmd);
    if (wifi_get_mutex(timeout, mutexinfo) != 0) {
        return -EBUSY;
    }

    wifi_flush_msgs();
    timeout = sys_timepoint_timeout(timepoint);
    result  = wifi_send_timeout(cmd, timeout);
    if (result != 0) {
        LOG_WRN("'%s'(%d) sending cmd: %.20s", wstrerr(-result), result, cmd);
        goto okerr_exit;
    }

    timeout            = sys_timepoint_timeout(timepoint);
    wait_msgs.num_msgs = 0;    // Initialize the structure
    wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
    wifi_add_wait_msg(&wait_msgs, "\r\nERROR:%19s\r\n", true, 1, errtmp);
    result = -EAGAIN;
    while (1) {
        timeout = sys_timepoint_timeout(timepoint);
        int ret = wifi_wait_for(&wait_msgs, timeout);
        if (ret < 0) {
            result = ret;
            break;
        }
        if (ret == 0) {
            result = 0;
            break;
        }
        if (ret == 1) {
            if (errret != NULL) {
                strncpy(errret, errtmp, 19);
                errtmp[19] = 0;
            }
            // If there is an error code, return that
            if (errtmp[0] == '-') {
                result = -(atoi(errtmp + 1));
            } else {
                LOG_DBG("Error received: %s on cmd: %20s", errtmp, cmd);
                result = -EBADE;
            }
            break;
        }
    }

okerr_exit:
    wifi_release_mutex();
    return result;
}

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
//			 -ENXIO if in sleeping state and DA won't respond
int wifi_get_dpm_state(k_timeout_t timeout)
{
    int               result = -EAGAIN;
    char              errtmp[20];
    char              dpm_state[5];
    wifi_wait_array_t wait_msgs;

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    k_timepoint_t timepoint = sys_timepoint_calc(timeout);
    if (wifi_get_mutex(timeout, __func__) != 0) {
        return -EBUSY;
    }
    wifi_flush_msgs();
    timeout = sys_timepoint_timeout(timepoint);
    if (wifi_send_timeout("AT+DPM=?", timeout) != 0) {
        goto get_dpm_exit;
    }

    wait_msgs.num_msgs = 0;    // Initialize the structure
    wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
    wifi_add_wait_msg(&wait_msgs, "+DPM:%1s\r\n", true, 1, dpm_state);
    wifi_add_wait_msg(&wait_msgs, "\r\nERROR%19s\r\n", true, 1, errtmp);
    result = -EAGAIN;
    while (1) {
        timeout = sys_timepoint_timeout(timepoint);
        int ret = wifi_wait_for(&wait_msgs, timeout);
        if (ret < 0) {
            result = ret;
            break;
        }
        if (ret == 0 && result >= 0) {
            break;    // We have to wait for the OK lest it be
                      // interperted as the answer to the next command
        }
        if (ret == 1) {
            result = ((dpm_state[0] == '1') ? 1 : 0);
            continue;
        }
        if (ret == 2) {
            result = -EBADE;
            break;
        }
    }

get_dpm_exit:
    wifi_release_mutex();
    return result;
}

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
int wifi_set_dpm_state(uint8_t new_state, bool awake_on_boot, k_timeout_t timeout)
{
    char cmd[20];

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    snprintf(cmd, 20, "AT+DPM=%d", new_state != 0);
    int ret = wifi_send_ok_err_atcmd(cmd, NULL, timeout);
    if (ret == 0) {
        if (new_state == 1) {
            g_awake_on_boot = awake_on_boot;
        } else {
            g_awake_on_boot = false;
        }
        g_last_dpm_change = k_uptime_get();
        // LOG_DBG("Setting dpm change state time to  %lld", g_last_dpm_change);
    }
    return ret;
}

////////////////////////////////////////////////////
// wifi_get_mqtt_state()
//  Retreive the mqtt current state
//
//  @param timeout - timeout for the write
//
//  @return - current mqtt state (0|1) or
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping
int wifi_get_mqtt_state(k_timeout_t timeout)
{
    int               result;
    char              errtmp[20];
    char              mqtt_state[5];
    wifi_wait_array_t wait_msgs;

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    k_timepoint_t timepoint = sys_timepoint_calc(timeout);
    if (wifi_get_mutex(timeout, __func__) != 0) {
        return -EBUSY;
    }
    wifi_flush_msgs();
    timeout = sys_timepoint_timeout(timepoint);
    if ((result = wifi_send_timeout("AT+NWMQCL=?", timeout)) != 0) {
        goto get_mqtt_exit;
    }

    timeout            = sys_timepoint_timeout(timepoint);
    wait_msgs.num_msgs = 0;    // Initialize the structure
    wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
    wifi_add_wait_msg(&wait_msgs, "+NWMQCL:%1s\r\n", true, 1, mqtt_state);
    wifi_add_wait_msg(&wait_msgs, "\r\nERROR%19s\r\n", true, 1, errtmp);
    result = -EAGAIN;
    while (1) {
        timeout = sys_timepoint_timeout(timepoint);
        int ret = wifi_wait_for(&wait_msgs, timeout);
        if (ret < 0) {
            result = ret;
            break;
        }
        if (ret == 0 && result >= 0) {
            break;    // We have to wait for the OK lest it be
                      // interperted as the answer to the next command
        }
        if (ret == 1) {
            result = ((mqtt_state[0] == '1') ? 1 : 0);
            continue;
        }
        if (ret == 2) {
            result = -EBADE;
            break;
        }
    }

get_mqtt_exit:
    wifi_release_mutex();
    return result;
}

////////////////////////////////////////////////////
// wifi_set_mqtt_state()
//  Set the mqtt state. Safe to call if the mqtt
// state is already in the desired state
//
//  @param timeout - timeout for the write
//  @param new_state - the new state to set
//						1 = on, 0 = off,
//
//  @return - 0 on success
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping
int wifi_set_mqtt_state(uint8_t new_state, k_timeout_t timeout)
{
    char cmd[20];
    char errorstr[70] = "";
    int  result;

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    if (wifi_get_mutex(timeout, __func__) != 0) {
        return -EBUSY;
    }
    // exit from here on only with set_mqtt_exit:

    if (new_state == da_state.mqtt_broker_connected) {
        // Its already in the state we want so just in case
        // the shadow got out of sync send a request for the
        // current state to re-sync
        strncpy(cmd, "AT+NWMQCL=?", 20);
    } else {
        snprintf(cmd, 20, "AT+NWMQCL=%d", new_state != 0);
    }
    cmd[19] = 0;

    result = wifi_send_ok_err_atcmd(cmd, errorstr, timeout);
    if (result != 0) {
        LOG_WRN("'%s'(%d) setting mqtt state to %d: %s", wstrerr(-result), result, new_state, errorstr);
        goto set_mqtt_exit;
    }

    // The command was successful, set the enable state
    send_zbus_tri_event(DA_EVENT_TYPE_MQTT_ENABLED, new_state, &(da_state.mqtt_enabled));

    if (new_state == 0) {
        // Turning off the MQTT should be immediate, so publish new state
        // We will get the "mqtt off" message eventually, but its ok to
        // set that in the shadow now so we don't have gap where we think
        // it is on and it actually isn't
        send_zbus_tri_event(DA_EVENT_TYPE_MQTT_BROKER_CONNECT, DA_STATE_KNOWN_FALSE, &(da_state.mqtt_broker_connected));
    } else {
        // Turning on the MQTT can take time and may never happen.
        // We wait until we get the async message indicating its on
        // before chaning the flas that says it on
    }

set_mqtt_exit:
    wifi_release_mutex();
    return result;
}

////////////////////////////////////////////////////
// wifi_get_mqtt_boot_state()
//  Get the mqtt state on DA boot. Safe to call if
// the state is already in the desired state
//
//  @param timeout - timeout for the write
//
//  @return - current mqtt state (0|1) or
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping
int wifi_get_mqtt_boot_state(k_timeout_t timeout)
{
    int               result;
    char              errtmp[20];
    char              mqtt_state[5];
    wifi_wait_array_t wait_msgs;

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    k_timepoint_t timepoint = sys_timepoint_calc(timeout);
    if (wifi_get_mutex(timeout, __func__) != 0) {
        return -EBUSY;
    }
    wifi_flush_msgs();
    timeout = sys_timepoint_timeout(timepoint);
    if ((result = wifi_send_timeout("AT+NWMQAUTO=?", timeout)) != 0) {
        goto get_mqtt_boot_exit;
    }

    timeout            = sys_timepoint_timeout(timepoint);
    wait_msgs.num_msgs = 0;    // Initialize the structure
    wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
    wifi_add_wait_msg(&wait_msgs, "+NWMQAUTO:%1s\r\n", true, 1, mqtt_state);
    wifi_add_wait_msg(&wait_msgs, "\r\nERROR%19s\r\n", true, 1, errtmp);
    result = -EAGAIN;
    while (1) {
        timeout = sys_timepoint_timeout(timepoint);
        int ret = wifi_wait_for(&wait_msgs, timeout);
        if (ret < 0) {
            result = ret;
            break;
        }
        if (ret == 0 && result >= 0) {
            break;    // We have to wait for the OK lest it be
                      // interperted as the answer to the next command
        }
        if (ret == 1) {
            result = ((mqtt_state[0] == '1') ? 1 : 0);
            continue;
        }
        if (ret == 2) {
            LOG_DBG("Error getting mqtt boot state: %s", errtmp);
            result = -EBADE;
            break;
        }
    }

get_mqtt_boot_exit:
    wifi_release_mutex();
    return result;
}

////////////////////////////////////////////////////
// wifi_set_mqtt_boot_state()
//  Set the mqtt state on DA boot
//
//  @param timeout - timeout for the write
//  @param new_state - the new state to set
//
//  @return - 0 on success
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping
int wifi_set_mqtt_boot_state(uint8_t new_state, k_timeout_t timeout)
{
    char cmd[20];
    int  ret;

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    if (wifi_get_mutex(timeout, __func__) != 0) {
        return -EBUSY;
    }

    if (new_state == da_state.mqtt_on_boot) {
        // Its already in the state we want so just in case
        // the shadow got out of sync send a request for the
        // current state to re-sync
        strncpy(cmd, "AT+NWMQAUTO=?", 20);
    } else {
        snprintf(cmd, 20, "AT+NWMQAUTO=%d", new_state != 0);
    }
    cmd[19] = 0;

    ret = wifi_send_ok_err_atcmd(cmd, NULL, timeout);
    if (ret == 0) {
        send_zbus_tri_event(DA_EVENT_TYPE_BOOT_MQTT_STATE, new_state, &(da_state.mqtt_on_boot));
    }

    wifi_release_mutex();
    return ret;
}

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
int wifi_get_da_fw_ver(char *fwver, int len, k_timeout_t timeout)
{
    int               ret;
    char              vertmp[40] = "";
    char              errtmp[20];
    wifi_wait_array_t wait_msgs;

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    k_timepoint_t timepoint = sys_timepoint_calc(timeout);
    if (wifi_get_mutex(timeout, __func__) != 0) {
        return -EBUSY;
    }

    wifi_flush_msgs();
    timeout = sys_timepoint_timeout(timepoint);
    ret     = wifi_send_timeout("AT+VER", timeout);
    if (ret != 0) {
        goto verexit;
    }

    wait_msgs.num_msgs = 0;    // Initialize the structure
    wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
    wifi_add_wait_msg(&wait_msgs, "+VER:%40s\r\n", true, 1, vertmp);
    wifi_add_wait_msg(&wait_msgs, "\r\nERROR%19s\r\n", true, 1, errtmp);
    while (1) {
        timeout = sys_timepoint_timeout(timepoint);
        ret     = wifi_wait_for(&wait_msgs, timeout);
        if (ret < 0) {
            break;
        }
        if (ret == 0 && vertmp[0] != 0) {
            break;    // We have to wait for the OK lest it be
                      // interperted as the answer to the next command
        }
        if (ret == 1) {
            strncpy(fwver, vertmp, len);
            fwver[len - 1] = 0;
            continue;
        }
        if (ret == 2) {
            LOG_DBG("Error getting mqtt boot state: %s", errtmp);
            ret = -EBADE;
            break;
        }
    }

verexit:
    wifi_release_mutex();
    return ret;
}

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
int wifi_get_wfscan(char *buf, int len, k_timeout_t timeout)
{
    int               ret;
    wifi_wait_array_t wait_msgs;
    char              scanmsg[40];
    char              errtmp[20];
    bool              got_ok   = false;
    bool              got_scan = false;

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    k_timepoint_t timepoint = sys_timepoint_calc(timeout);
    if (wifi_get_mutex(timeout, __func__) != 0) {
        return -EBUSY;
    }

    wifi_flush_msgs();
    timeout = sys_timepoint_timeout(timepoint);
    ret     = wifi_send_timeout("AT+WFSCAN", timeout);
    if (ret != 0) {
        LOG_ERR("Failed to send wfscan %d", ret);
        goto wfscanexit;
    }

    snprintf(scanmsg, 40, "%%%ds\r\n", len);
    buf[0] = 0;

    wait_msgs.num_msgs = 0;    // Initialize the structure
    wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
    wifi_add_wait_msg(&wait_msgs, "\r\nERROR%19s\r\n", true, 1, errtmp);
    wifi_add_wait_msg(&wait_msgs, scanmsg, true, 1, buf);
    while (1) {
        timeout = sys_timepoint_timeout(timepoint);
        ret     = wifi_wait_for(&wait_msgs, timeout);
        if (ret < 0) {
            break;
        }
        if (ret == 0) {
            got_ok = true;
            if (got_scan) {
                break;
            }
        }
        if (ret == 2) {
            // We have to wait for the OK lest it be
            // interperted as the answer to the next command
            char *start = strstr(buf, "+WFSCAN:");
            if (start) {
                memcpy(buf, start + 8, len - (start - buf));
                got_scan = true;
                if (got_ok) {
                    break;
                }
            } else {
                if (!got_scan) {
                    buf[0] = 0;
                }
            }
        }
        if (ret == 1) {
            LOG_DBG("Error getting mqtt boot state: %s", errtmp);
            ret = -EBADE;
            break;
        }
    }

wfscanexit:
    wifi_release_mutex();
    return ret;
}

static int freq_to_channel(int freq)
{
    int freq_conv[21][3] = { // ch, start_freq, end_freq
                             { 1, 2401, 2423 },   { 2, 2406, 2428 },   { 3, 2411, 2433 },   { 4, 2416, 2438 },
                             { 5, 2421, 2443 },   { 6, 2426, 2448 },   { 7, 2431, 2453 },   { 8, 2436, 2458 },
                             { 9, 2441, 2463 },   { 10, 2446, 2468 },  { 11, 2451, 2473 },  { 12, 2456, 2478 },
                             { 13, 2461, 2483 },  { 14, 2473, 2495 },  { 184, 4910, 4930 }, { 188, 4930, 4950 },
                             { 192, 4950, 4970 }, { 196, 4970, 4990 }, { 8, 5030, 5050 },   { 12, 5050, 5070 },
                             { 16, 5070, 5090 }
    };
    for (int i = 0; i < 21; i++) {
        if (freq >= freq_conv[i][1] && freq <= freq_conv[i][2]) {
            return freq_conv[i][0];
        }
    }
    return 0;
}

uint64_t   g_last_ssid_scan_time = 0;
wifi_arr_t g_last_ssid_list      = { .count = 0 };
static int parse_wfscan(char *sub, bool skip_hidden)
{
    char  flagstr[100];
    int   rssi;
    int   freq;
    char *nsub;
    // sub points to \r\n+WFSCAN:...

    sub += 10;
    g_last_ssid_list.count = 0;
    wifi_obj_t *entry      = &g_last_ssid_list.wifi[g_last_ssid_list.count];

    while (strlen(sub) > 10) {
        nsub = strstr(sub, "\n");
        if (nsub != NULL) {
            *nsub = 0;
        }
        if (sscanf(sub, "%s\t%d\t%d\t%s\t%s", entry->macstr, &freq, &rssi, flagstr, entry->ssid) == 5) {
            // Need to catch names with spaces at the end and beginning, since the last entry is a ssid
            char *end = strrchr(sub, 9);    // search from the end backwards for a tab (9), everything after is the ssid
            if (end != NULL) {
                end += 1;
                strncpy(entry->ssid, end, 32);
            }
            entry->rssi    = rssi;
            entry->channel = freq_to_channel(freq);
            strncpy(entry->flags, flagstr, 99);
            entry->flags[99] = 0;
            g_last_ssid_list.count++;
            entry = &g_last_ssid_list.wifi[g_last_ssid_list.count];
        } else if (sscanf(sub, "%s\t%d\t%d\t%s\t", entry->macstr, &freq, &rssi, flagstr) == 4) {
            if (skip_hidden == false) {
                entry->ssid[0] = 0;
                strncpy(entry->flags, flagstr, 99);
                entry->flags[99] = 0;
                entry->channel   = freq_to_channel(freq);
                g_last_ssid_list.count++;
                entry = &g_last_ssid_list.wifi[g_last_ssid_list.count];
            }
        } else {
            LOG_ERR("Failed to parse:");
            LOG_HEXDUMP_DBG(sub, strlen(sub), "data");
        }
        if (nsub == NULL) {
            break;
        }
        sub = nsub + 1;
    }

    return g_last_ssid_list.count;
}

//////////////////////////////////////////////////////////////
// wifi_get_last_ssid_list()
//
// Get the list of SSID gathered in a previous scan
wifi_arr_t *wifi_get_last_ssid_list()
{
    return &g_last_ssid_list;
}

int wifi_find_ssid_in_scan(char *ssid)
{
    for (int i = 0; i < g_last_ssid_list.count; i++) {
        if (strcmp(ssid, g_last_ssid_list.wifi[i].ssid) == 0) {
            return i;
        }
    }
    return -1;
}

/////////////////////////////////////////////////////////
// wifi_refresh_ssid_list()
//
// refresh the locally cached list of SSIDs seen by the DA
// if the age of the list is older then the parameter
// passed in.
//
// @param skip_hidden - if true, don't include ssid without name
// @param max_age_ms - max age of the list in ms
// @param timeout - timeout for the operation to complete
//
//  @return - 0 on success
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping
int wifi_refresh_ssid_list(bool skip_hidden, int max_age_sec, k_timeout_t timeout)
{
    int           ret = 0;
    char         *sub;
    wifi_msg_t    msg;
    k_timepoint_t timepoint = sys_timepoint_calc(timeout);
    bool          got_ok    = false;
    bool          got_scan  = false;
    uint64_t      now       = utils_get_utc();
    uint64_t      uptime    = k_uptime_get();
    uint64_t      delta     = now - g_last_ssid_scan_time;

    if (uptime > (max_age_sec * 1000) && delta < max_age_sec) {
        LOG_DBG("Skipping scan, list is %lld seconds old, max specified %d", delta, max_age_sec);
        return 0;
    }
    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    if (wifi_get_mutex(timeout, __func__) != 0) {
        LOG_ERR("Failed to get mutex in get_ssid");
        return -EBUSY;
    }
    timeout = sys_timepoint_timeout(timepoint);
    wifi_flush_msgs();

    ret = wifi_send_timeout("AT+WFSCAN", timeout);
    if (ret != 0) {
        LOG_ERR("'%s'(%d) sending at+wfscan", wstrerr(-ret), ret);
        goto scan_release_exit;
    }

    while (1) {
        timeout = sys_timepoint_timeout(timepoint);
        ret     = wifi_recv(&msg, timeout);
        if (ret < 0) {
            LOG_ERR("'%s'(%d) while receiving ssids", wstrerr(-ret), ret);
            goto scan_release_exit;
        }
        if (strstr(msg.data, "\r\nERROR") != NULL) {
            LOG_ERR("Got 'ERROR' while receiving ssids");
            wifi_msg_free(&msg);
            ret = -EBADE;
            goto scan_release_exit;
        }
        if (strstr(msg.data, "\r\nOK\r\n") != NULL) {
            wifi_msg_free(&msg);
            got_ok = true;
            if (got_scan) {
                ret = 0;
                goto scan_release_exit;
            }
        }
        if ((sub = strstr(msg.data, "\r\n+WFSCAN:")) != NULL) {
            int num = parse_wfscan(sub, skip_hidden);
            LOG_DBG("Parsed %d ssids", num);
            wifi_msg_free(&msg);
            g_last_ssid_scan_time = utils_get_utc();
            got_scan              = true;
            if (got_ok) {
                ret = 0;
                goto scan_release_exit;
            }
        }
    }

scan_release_exit:
    wifi_release_mutex();
    return ret;
}

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
int wifi_insure_mqtt_sub_topics(char *desired_topics[], k_timeout_t timeout)
{
    char *merged_topics[CONFIG_IOT_MAX_TOPIC_NUM + 1];
    int   j, i;
    int   desired_cnt = 0;

    // start with the new list = desired list
    for (j = 0; j < CONFIG_IOT_MAX_TOPIC_NUM; j++) {
        if (desired_topics[j] != 0 && desired_topics[j][0] != 0) {
            merged_topics[j] = desired_topics[j];
            desired_cnt++;
        } else {
            break;
        }
    }

    // if the list isn't full, fill it with the existing topics
    // that doen't match any new topics
    if (desired_cnt < CONFIG_IOT_MAX_TOPIC_NUM) {
        for (i = 0; i < CONFIG_IOT_MAX_TOPIC_NUM; i++) {
            if (da_state.mqtt_sub_topics[i][0] == 0) {
                break;
            }
            bool match = false;
            for (j = 0; j < desired_cnt; j++) {
                if (strncmp(desired_topics[j], da_state.mqtt_sub_topics[i], CONFIG_IOT_MAX_TOPIC_LENGTH) == 0) {
                    match = true;
                    break;
                }
            }
            if (match == false) {
                merged_topics[desired_cnt++] = da_state.mqtt_sub_topics[i];
                if (desired_cnt >= CONFIG_IOT_MAX_TOPIC_NUM) {
                    break;
                }
            }
        }
    }
    merged_topics[desired_cnt] = 0;
    return wifi_set_mqtt_sub_topics(merged_topics, timeout);
}

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
//  @return - 0 on success
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EINVAL if new_topics if badly formed
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping or mqtt broker is connected
int wifi_set_mqtt_sub_topics(char *new_topics[], k_timeout_t timeout)
{
    int               ret = -1;
    wifi_wait_array_t wait_msgs;
    char              errorstr[70];
    int               i, j, num_topics = 0;
    char             *cmd       = NULL;
    k_timepoint_t     timepoint = sys_timepoint_calc(timeout);
    char             *topic_copy[CONFIG_IOT_MAX_TOPIC_NUM + 1];

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    if (da_state.mqtt_broker_connected == 1) {
        LOG_ERR("Can't set topics while connected to the broker");
        return -ENXIO;
    }

    for (j = 0; j < CONFIG_IOT_MAX_TOPIC_NUM; j++) {
        if (new_topics[j] != 0) {
            if (strlen(new_topics[j]) >= CONFIG_IOT_MAX_TOPIC_LENGTH) {
                LOG_ERR("Topic too long");
                return -EINVAL;
            }
            num_topics++;
        } else {
            break;
        }
    }

    if (wifi_get_mutex(timeout, __func__) != 0) {
        LOG_ERR("Can't get mutex for setting topics");
        return -EBUSY;
    }
    timeout = sys_timepoint_timeout(timepoint);

    // 65 = max topic len + 1 for comma or null
    int cmdlen = sizeof("AT+NWMQTS=%d") + num_topics * 65;
    cmd        = k_calloc(cmdlen, 1);
    if (cmd == NULL) {
        LOG_ERR("Can't get memory for setting topics");
        goto release_and_exit;
    }

    snprintf(cmd, cmdlen, "AT+NWMQTS=%d", num_topics);
    char *cptr = cmd + strlen(cmd);
    for (i = 0; i < num_topics; i++) {
        strncat(cptr++, ",", 2);
        // In addition making the cmd, we are making copies of all
        // the topics. Because some of the char * in new_topics may
        // be actually pointing to da_state.mqtt_sub_topics, we can
        // cause hard to debug bugs when we later copy the new_topics
        // into the da_state.mqtt_sub_topics.
        // We solve this by makign a new array that points to the
        // cmd copy of the strings and use that as a source or topics.
        // To do that, we need to null terminate the topics in cmd
        // after the cmd is sent
        strncat(cptr, new_topics[i], CONFIG_IOT_MAX_TOPIC_LENGTH);
        topic_copy[i] = cptr;
        cptr += strlen(new_topics[i]);
    }

    // LOG_ERR("Setting %d topics: %s", num_topics, cmd);
    if ((ret = wifi_send_timeout(cmd, timeout)) != 0) {
        goto release_and_exit;
    }
    timeout = sys_timepoint_timeout(timepoint);
    // Null terimate the copies
    for (i = 0; i < num_topics; i++) {
        topic_copy[i][strlen(new_topics[i])] = 0;
    }

    wait_msgs.num_msgs = 0;    // Initialize the structure
    wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
    wifi_add_wait_msg(&wait_msgs, "\r\nERROR%19s\r\n", true, 1, errorstr);
    while (!OUT_OF_TIME(timeout, timepoint)) {
        ret = wifi_wait_for(&wait_msgs, timeout);
        if (ret == 0) {
            // set the topics in the da_state
            for (i = 0; i < CONFIG_IOT_MAX_TOPIC_NUM; i++) {
                if (i < num_topics) {
                    strncpy(da_state.mqtt_sub_topics[i], topic_copy[i], CONFIG_IOT_MAX_TOPIC_LENGTH);
                    da_state.mqtt_sub_topics[i][CONFIG_IOT_MAX_TOPIC_LENGTH] = 0;
                } else {
                    da_state.mqtt_sub_topics[i][0] = 0;
                }
            }
            send_zbus_int_event(DA_EVENT_TYPE_MQTT_SUB_TOP_CHANGED, num_topics, &(da_state.mqtt_sub_topic_count), true);
            break;
        } else if (ret < 0) {
            break;
        } else {
            LOG_ERR("Error on received setting mqtt topics: %s", errorstr);
            ret = -EBADE;
            break;
        }
    }

release_and_exit:
    wifi_release_mutex();
    if (cmd != NULL) {
        k_free(cmd);
    }
    return ret;
}

////////////////////////////////////////////////////////////
// wifi_set_mqtt_sub_topics_by_type()
//
// Set the list of topics that the MQTT service subscribes
// to by specifying an array of message types.  This will
// construct the messages we subscribe to.
//
//  @param new_topics - The list of topic strings ptrs.
//                      The last ptr in the list must be null
//  @param timeout - timeout for the write
//
//  @return - 0 on success
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EINVAL if new_topics if badly formed
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping or mqtt broker is connected
char topic_buf[(CONFIG_IOT_MAX_TOPIC_NUM * 64) + 100];
int  wifi_set_mqtt_sub_topics_by_type(int types[], int cnt, k_timeout_t timeout)
{
    char *nt[CONFIG_IOT_MAX_TOPIC_NUM + 1];
    int   i;

    if (cnt > CONFIG_IOT_MAX_TOPIC_NUM) {
        LOG_ERR("Too many topics %d", cnt);
        return -EINVAL;
    }

    for (i = 0; i < cnt; i++) {
        int type = types[i];
        if (type < 0 || type > 30) {
            LOG_ERR("Invalid type %d", type);
            return -EINVAL;
        }
        snprintf(
            &(topic_buf[i * 64]),
            ((CONFIG_IOT_MAX_TOPIC_NUM - i) * 64),
            "messages/%d/%d/%s/c2d",
            CONFIG_IOT_MQTT_BRAND_ID,
            type,
            da_state.mqtt_client_id);
        nt[i] = &(topic_buf[i * 64]);
    }
    nt[cnt] = 0;
    int ret = wifi_set_mqtt_sub_topics(nt, timeout);
    return ret;
}

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
int wifi_mqtt_publish(uint16_t message_type, char *msg, bool wait_for_send_conf, k_timeout_t timeout)
{
    char              pub_topic[90];
    char              errorstr[25];
    wifi_wait_array_t wait_msgs;
    char             *buf       = NULL;
    int               result    = -EINVAL;
    k_timepoint_t     timepoint = sys_timepoint_calc(timeout);
    int               msglen    = strlen(msg);

    if (message_type > 999) {
        LOG_ERR("message_type must be less than 1000");
        return -EINVAL;
    }

    if (da_state.mqtt_broker_connected != 1) {
        LOG_ERR("MQTT broker is not connected, connect before sending an mqtt msg");
        return -ENXIO;
    }

    if (da_state.mqtt_certs_installed != 1) {
        LOG_ERR("MQTT certs not installed, install before sending an mqtt msg");
        return -ENXIO;
    }

    if (da_state.ntp_server_set != 1) {
        LOG_ERR("NTP Server not set, it should be set on boot!");
        return -ENXIO;
    }

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    if (msglen > WIFI_MSG_SIZE - 100) {    // Leave room for cmd and topic
        LOG_ERR("mqtt msg is too large to send to the DA (%d)", msglen);
        char *temp_string = (char *)k_malloc(1000);
        if (temp_string != NULL) {
            strncpy(temp_string, msg, 999);
            temp_string[999] = 0;
            LOG_ERR("msg %.*s", 999, temp_string);
            LOG_PANIC();
            strncpy(temp_string, &msg[1000], 999);
            temp_string[999] = 0;
            LOG_ERR("msg %.*s", 999, temp_string);
            LOG_PANIC();
            k_free(temp_string);
        }
        return -EFBIG;
    }

    snprintf(pub_topic, 90, "messages/%d/%d/%s/d2c", CONFIG_IOT_MQTT_BRAND_ID, message_type, da_state.mqtt_client_id);

    if (wifi_get_mutex(timeout, __func__) != 0) {
        return -EBUSY;
    }

#define MQTT_PUB_HEAD "AT+NWMQMSG='"
#define MQTT_PUB_MID  "',"
    int buflen = sizeof(MQTT_PUB_HEAD) + msglen + sizeof(MQTT_PUB_MID) + strlen(pub_topic) + 1;
    buf        = k_calloc(buflen, 1);
    if (buf == NULL) {
        result = -ENOMEM;
        goto mqtt_pub_exit;
    }

    strncpy(buf, MQTT_PUB_HEAD, buflen);
    buflen -= sizeof(MQTT_PUB_HEAD);
    strncat(buf, msg, buflen);
    buflen -= msglen;
    strncat(buf, MQTT_PUB_MID, buflen);
    buflen -= sizeof(MQTT_PUB_MID);
    strncat(buf, pub_topic, buflen);

    timeout = sys_timepoint_timeout(timepoint);
    if ((result = wifi_send_timeout(buf, timeout)) != 0) {
        goto mqtt_pub_exit;
    }

    buf[0]             = 0;    // This will hold the data we receive
    wait_msgs.num_msgs = 0;    // Initialize the structure
    wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
    wifi_add_wait_msg(&wait_msgs, "\r\nERROR:%19s\r\n", true, 1, errorstr);
    wifi_add_wait_msg(&wait_msgs, "+NWMQMSGSND:%20s", true, 1, buf);
    while (!OUT_OF_TIME(timeout, timepoint)) {
        int ret = wifi_wait_for(&wait_msgs, timeout);
        if (ret < 0) {
            result = ret;
            break;
        }
        if (ret == 0) {
            if (wait_for_send_conf == true) {
                continue;
            }
            result = 0;
            break;
        }
        if (ret == 1) {
            LOG_ERR("Error on received publishing mqtt msg: %s", errorstr);
            result = strtol(errorstr, NULL, 10);
            if (result == 0 && errorstr[0] != '0') {
                // We could not convert to a number
                result = -EBADE;
            }
            break;
        }
        if (ret == 2) {
            if (buf[0] == '1') {
                // success
                result = 0;
            } else {
                // "0,<errcode>"
                result = strtol(buf + 2, NULL, 10);
                if (result > 0) {
                    result = -result;
                }
            }
            break;
        }
    }

mqtt_pub_exit:
    if (buf != NULL) {
        k_free(buf);
    }
    wifi_release_mutex();
    return result;
}

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
//  @return - 0 on success
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EINVAL if new_topics if badly formed
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping or mqtt broker is connected
int wifi_set_otp_register(int reg, int size, int newval, k_timeout_t timeout)
{
    int  ret;
    char cmd[50];

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    if (newval > ((1 << (size * 8)) - 1)) {
        LOG_ERR("New value %d too big for OTP register %x", newval, reg);
        wifi_release_mutex();
        return -EINVAL;
    }

    k_timepoint_t timepoint = sys_timepoint_calc(timeout);
    if (wifi_get_mutex(timeout, __func__) != 0) {
        return -EBUSY;
    }

    // EAS TODO I am not sure why I read before writing. Investigate
    timeout = sys_timepoint_timeout(timepoint);
    ret     = wifi_get_otp_register(reg, size, timeout);
    if (ret != 0) {
        wifi_release_mutex();
        return ret;
    }

    snprintf(cmd, 50, "AT+UOTPWRASC=%x,%d,%x", reg, size, newval);
    timeout = sys_timepoint_timeout(timepoint);
    ret     = wifi_send_ok_err_atcmd(cmd, NULL, timeout);
    wifi_release_mutex();
    return ret;
}

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
//  @return - 0 on success
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EINVAL if new_topics if badly formed
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping or mqtt broker is connected
int64_t wifi_get_otp_register(int reg, int size, k_timeout_t timeout)
{
    int               ret;
    wifi_wait_array_t wait_msgs;
    char              cmd[50];
    char              data[50];
    char              errstr[50];

    snprintf(cmd, 50, "AT+UOTPRDASC=%x,%d", reg, size);

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    k_timepoint_t timepoint = sys_timepoint_calc(timeout);
    if (wifi_get_mutex(timeout, __func__) != 0) {
        LOG_ERR("Failed to get mutex in get_otp");
        return -EBUSY;
    }
    timeout = sys_timepoint_timeout(timepoint);
    if ((ret = wifi_send_timeout(cmd, timeout)) != 0) {
        wifi_release_mutex();
        return ret;
    }

    cmd[0]             = 0;    // This will hold the data we receive
    timeout            = sys_timepoint_timeout(timepoint);
    wait_msgs.num_msgs = 0;    // Initialize the structure
    wifi_add_wait_msg(&wait_msgs, "\r\nERROR%50s\r\n", true, 1, errstr);
    wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
    wifi_add_wait_msg(&wait_msgs, "\r\n", false, 0);
    wifi_add_wait_msg(&wait_msgs, "\r", false, 0);
    wifi_add_wait_msg(&wait_msgs, "%50s", true, 1, data);
    while (1) {
        ret     = wifi_wait_for(&wait_msgs, timeout);
        timeout = sys_timepoint_timeout(timepoint);
        if (ret <= 0) {
            wifi_release_mutex();
            return ret;
        }
        if (ret == 4) {
            int len = strlen(data);
            strncat(cmd, data, 50 - len);    // Add the digits to the ones received so far
            continue;
        }
        if (ret == 1) {
            // OK ends the response
            wifi_release_mutex();
            return strtol(cmd, NULL, 16);
        }
    }
    wifi_release_mutex();
    return -EAGAIN;
}

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
int wifi_get_nvram(uint32_t addr, uint8_t *buf, int size, k_timeout_t timeout)
{
    wifi_wait_array_t wait_msgs;
    char              cmd[50];
    char              datafmt[20];
    int               ret, result = -1;
    char              errstr[50];

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    snprintf(cmd, 50, "AT+FLASHDUMP=%X,%d", addr, size);

    k_timepoint_t timepoint = sys_timepoint_calc(timeout);
    if (wifi_get_mutex(timeout, __func__) != 0) {
        return -EBUSY;
    }
    timeout = sys_timepoint_timeout(timepoint);
    if ((ret = wifi_send_timeout(cmd, timeout)) != 0) {
        wifi_release_mutex();
        return ret;
    }

    snprintf(datafmt, 20, "%%%ds", size);
    timeout            = sys_timepoint_timeout(timepoint);
    wait_msgs.num_msgs = 0;    // Initialize the structure
    wifi_add_wait_msg(&wait_msgs, "\r\nERROR%50s\r\n", true, 1, errstr);
    wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
    wifi_add_wait_msg(&wait_msgs, "\r\n", false, 0);
    wifi_add_wait_msg(&wait_msgs, datafmt, true, 1, buf);    // This will catch the data
    while (1) {
        ret     = wifi_wait_for(&wait_msgs, timeout);
        timeout = sys_timepoint_timeout(timepoint);
        if (ret < 0) {    // Timeout
            result = ret;
            break;
        }
        if (ret == 0) {    // Error
            result = -EBADE;
            break;
        }
        if (ret == 1) {    // OK
            result = 0;
            break;
        }
        if (ret == 3) {    // Data
            continue;
        }
    }
    wifi_release_mutex();
    return result;
}

///////////////////////////////////////////////////////////////////////
// wifi_put_nvram()
//  Send a command to the DA to write data to DA's nvram memory.
//	Note that the data buffer is a string of the hex values
//
//  @param addr - the address in nvram to read
//  @param buf - where to put the data
//  @param timeout - timeout for the write
//
//  @return - 0 on success
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EINVAL if new_topics if badly formed
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping or mqtt broker is connected
int wifi_put_nvram(uint32_t addr, uint8_t *buf, k_timeout_t timeout)
{
    wifi_wait_array_t wait_msgs;
    int               len, ret = 0;
    char              errstr[50];

    if (addr < 0x003AD000 || addr >= 0x003ED000) {
        LOG_ERR("NVRAM address must be >= 0x003AD000 && addr < 0x003ED000");
        return -EINVAL;
    }
    len = strlen(buf);
    if (len > WIFI_MSG_SIZE - 50) {
        LOG_ERR("data size limited to < %d", WIFI_MSG_SIZE - 50);
        return -EINVAL;
    }

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    k_timepoint_t timepoint = sys_timepoint_calc(timeout);
    if (wifi_get_mutex(timeout, __func__) != 0) {
        return -EBUSY;
    }

    char *at_cmd_buf = k_calloc(len + 50, 1);
    if (at_cmd_buf == NULL) {
        ret = -ENOMEM;
        goto put_nvram_exit;
    }
    snprintf(at_cmd_buf, 50, "AT+FLASHWRITE=%X,", addr);
    strncat(at_cmd_buf, buf, len + 1);
    timeout = sys_timepoint_timeout(timepoint);
    if ((ret = wifi_send_timeout(at_cmd_buf, timeout)) != 0) {
        goto put_nvram_exit;
    }

    timeout            = sys_timepoint_timeout(timepoint);
    wait_msgs.num_msgs = 0;    // Initialize the structure
    wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
    wifi_add_wait_msg(&wait_msgs, "\r\nERROR%50s\r\n", true, 1, errstr);
    while (1) {
        ret     = wifi_wait_for(&wait_msgs, timeout);
        timeout = sys_timepoint_timeout(timepoint);
        if (ret < 0) {    // Timeout
            break;
        }
        if (ret == 0) {    // OK
            break;
        }
        if (ret == 1) {    // error
            LOG_ERR("Error on received writing nvram: %s", errstr);
            ret = -EBADE;
            break;
        }
    }

put_nvram_exit:
    if (at_cmd_buf) {
        k_free(at_cmd_buf);
    }
    wifi_release_mutex();
    return ret;
}

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
char *wifi_get_mac(int *which, k_timeout_t timeout)
{
    int               ret;
    char             *retstr = NULL;
    wifi_wait_array_t wait_msgs;
    static char       cmd[50];
    char              errstr[50];

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return NULL;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return NULL;
    }

    k_timepoint_t timepoint = sys_timepoint_calc(timeout);
    if (wifi_get_mutex(timeout, __func__) != 0) {
        return NULL;
    }
    timeout = sys_timepoint_timeout(timepoint);
    strncpy(cmd, "AT+WFMAC=?", 49);
    cmd[49] = 0;
    if (wifi_send_timeout(cmd, timeout) != 0) {
        LOG_ERR("Timed out sending %s", cmd);
        wifi_release_mutex();
        return NULL;
    }

    cmd[0]             = 0;    // This will hold the data we receive
    timeout            = sys_timepoint_timeout(timepoint);
    wait_msgs.num_msgs = 0;    // Initialize the structure
    wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
    wifi_add_wait_msg(&wait_msgs, "\r\nERROR%50s\r\n", true, 1, errstr);
    wifi_add_wait_msg(&wait_msgs, "+WFMAC:%50s", true, 1, cmd);
    wifi_add_wait_msg(&wait_msgs, "+WFSPF:%50s", true, 1, cmd);
    wifi_add_wait_msg(&wait_msgs, "+WFOTP:%50s", true, 1, cmd);
    while (1) {
        ret     = wifi_wait_for(&wait_msgs, timeout);
        timeout = sys_timepoint_timeout(timepoint);
        if (ret < 0) {
            break;
        }
        if (ret == 0) {
            retstr = cmd;
            break;
        }
        if (ret == 1) {
            break;
        }
        if (ret > 1) {
            *which = ret - 2;
            continue;
        }
    }
    wifi_release_mutex();
    return retstr;
}

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
//  @return - 0 on success
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EINVAL if new_topics if badly formed
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping or mqtt broker is connected
int wifi_set_mac(char *newmac, k_timeout_t timeout)
{
    int               ret;
    wifi_wait_array_t wait_msgs;
    char              cmd[50];
    char              errstr[50];

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    k_timepoint_t timepoint = sys_timepoint_calc(timeout);
    snprintf(cmd, 50, "AT+WFSPF=%s", newmac);
    if (wifi_get_mutex(timeout, __func__) != 0) {
        return -EBUSY;
    }
    timeout = sys_timepoint_timeout(timepoint);
    if ((ret = wifi_send_timeout(cmd, timeout)) != 0) {
        wifi_release_mutex();
        return ret;
    }

    cmd[0]             = 0;    // This will hold the data we receive
    timeout            = sys_timepoint_timeout(timepoint);
    wait_msgs.num_msgs = 0;    // Initialize the structure
    wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
    wifi_add_wait_msg(&wait_msgs, "\r\nERROR%50s\r\n", true, 1, errstr);
    wifi_add_wait_msg(&wait_msgs, "+WFMAC:%50s", false, 1, cmd);
    wifi_add_wait_msg(&wait_msgs, "+WFSPF:%50s", false, 1, cmd);
    wifi_add_wait_msg(&wait_msgs, "+WFOTP:%50s", false, 1, cmd);
    while (1) {
        ret     = wifi_wait_for(&wait_msgs, timeout);
        timeout = sys_timepoint_timeout(timepoint);
        if (ret < 0) {
            break;
        }
        if (ret == 0) {
            break;
        }
        if (ret == 1) {
            ret = -EBADE;
            break;
        }
        if (ret > 1) {
            continue;
        }
    }
    wifi_release_mutex();
    return ret;
}

///////////////////////////////////////////////////////////////////////
// wifi_get_xtal()
//  Send a command to the DA to read the current XTAL value
//
//  @param timeout - timeout for the write
//
//  @return - 0 on success
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EINVAL if new_topics if badly formed
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping or mqtt broker is connected
int wifi_get_xtal(k_timeout_t timeout)
{
    int               ret;
    wifi_wait_array_t wait_msgs;
    char              cmd[50] = "AT+XTALRD";
    char              errstr[50];

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    //  This command has a non-standard response in that it comes
    //  back in multiple parts

    k_timepoint_t timepoint = sys_timepoint_calc(timeout);
    if (wifi_get_mutex(timeout, __func__) != 0) {
        return -EBUSY;
    }
    timeout = sys_timepoint_timeout(timepoint);
    if ((ret = wifi_send_timeout(cmd, timeout)) != 0) {
        goto gxtalexit;
    }

    cmd[0]             = 0;    // This will hold the data we receive
    timeout            = sys_timepoint_timeout(timepoint);
    wait_msgs.num_msgs = 0;    // Initialize the structure
    wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
    wifi_add_wait_msg(&wait_msgs, "\r\nERROR%50s\r\n", true, 1, errstr);
    wifi_add_wait_msg(&wait_msgs, "0x%50s", false, 1, cmd);    // This will catch the return value
    while (1) {
        ret     = wifi_wait_for(&wait_msgs, timeout);
        timeout = sys_timepoint_timeout(timepoint);
        if (ret < 0) {
            break;
        }
        if (ret == 0) {
            ret = strtol(cmd, NULL, 16);
            break;
        }
        if (ret == 1) {
            ret = -EBADE;
            break;
        }
    }

gxtalexit:
    wifi_release_mutex();
    return ret;
}

///////////////////////////////////////////////////////////////////////
// wifi_set_xtal()
//  Send a command to the DA to set the current XTAL value, temporarily
//
//  @param errret - a buffer to hold any error text, min size 20
//					or null
//  @param timeout - timeout for the write
//
//  @return - 0 on success
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EINVAL if new_topics if badly formed
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping or mqtt broker is connected
int wifi_set_xtal(int newval, char *errret, k_timeout_t timeout)
{
    char cmd[50];
    snprintf(cmd, 50, "AT+XTALWR=%x", newval);
    return wifi_send_ok_err_atcmd(cmd, errret, timeout);
}

///////////////////////////////////////////////////////////////////////
// wifi_stop_XTAL_test()
//  Reboot the DA into XTAL normal
//
//  @return - 0 on success
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EINVAL if new_topics if badly formed
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping or mqtt broker is connected
void wifi_stop_XTAL_test()
{
    int ret;

    ret = wifi_send_ok_err_atcmd("AT+TMRFNOINIT=0", NULL, K_MSEC(100));
    if (ret != 0) {
        LOG_ERR("Error turning off RF Test mode");
        return;
    }
    // This may timeout because the DA just boots and doesn't respond
    wifi_send_ok_err_atcmd("AT+RESTART", NULL, K_MSEC(100));
}

///////////////////////////////////////////////////////////////////////
// wifi_start_XTAL_test()
//  Reboot the DA into XTAL test mode
//
//  @param timeout - timeout for the write
//
//  @return - 0 on success
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EINVAL if new_topics if badly formed
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping or mqtt broker is connected
int wifi_start_XTAL_test()
{
    int ret = -1;

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    if (wifi_get_mutex(K_NO_WAIT, __func__) != 0) {
        return -EBUSY;
    }

    if ((ret = wifi_send_ok_err_atcmd("AT+TMRFNOINIT=1", NULL, K_MSEC(1000))) < 0) {
        goto sxtalt;
    }
    if ((ret = wifi_send_ok_err_atcmd("AT+RESTART", NULL, K_MSEC(1000))) < 0) {
        goto sxtalt;
    }

    // Wait for the DA to come back up
    k_sleep(K_MSEC(3000));

    if ((ret = wifi_send_ok_err_atcmd("AT+RFTESTSTART", NULL, K_MSEC(1000))) < 0) {
        wifi_stop_XTAL_test();
        goto sxtalt;
    }
    if ((ret = wifi_send_ok_err_atcmd("AT+RFCWTEST=2412,0,0", NULL, K_MSEC(1000))) < 0) {
        wifi_stop_XTAL_test();
        goto sxtalt;
    }

sxtalt:
    wifi_release_mutex();
    return ret;
}

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
int wifi_rtc_sleep(int ms_to_sleep)
{
    int  ret;
    char cmd[50];

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (ms_to_sleep < 0 || ms_to_sleep > 2097151000) {
        LOG_ERR("Invalid sleep time %d", ms_to_sleep);
        return -EINVAL;
    }

    if (wifi_get_mutex(K_MSEC(ms_to_sleep), __func__) != 0) {
        return -EBUSY;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        // reset the rtc wake time
        ret = wifi_wake_no_sleep(K_MSEC(400));
        if (ret < 0) {
            goto rtcs_exit;
        }
    }

    snprintf(cmd, 50, "AT+SETSLEEP3EXT=%d", ms_to_sleep);
    ret = wifi_send_ok_err_atcmd(cmd, NULL, K_MSEC(100));
    if (ret < 0) {
        goto rtcs_exit;
    }

    send_zbus_tri_event(DA_EVENT_TYPE_IS_SLEEPING, DA_STATE_KNOWN_TRUE, &(da_state.is_sleeping));
    uint64_t now = k_uptime_get();
    send_zbus_timestamp_event(DA_EVENT_TYPE_RTC_WAKE_TIME, (now + ms_to_sleep), &(da_state.rtc_wake_time));

rtcs_exit:
    wifi_release_mutex();
    return ret;
}

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
das_tri_state_t wifi_check_sleeping(bool change_state)
{
    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    // Because the DA can sleep without telling us we check by if it is
    // sleeping by sending an "AT" command.  If it is sleeping we should
    // get a timeout and if it's awake we should get an "OK" response
    // within 3 ms. There are other reasons for a timeout, but  none of
    // them should happen normally

    // make sure that wifi_send_ok_err_atcmd doesn't think we aer sleeping
    // since that is what we are trying to determine
    das_tri_state_t old_sleeping = da_state.is_sleeping;
    da_state.is_sleeping         = DA_STATE_KNOWN_FALSE;
    int ret                      = wifi_send_ok_err_atcmd("AT", NULL, K_MSEC(80));
    da_state.is_sleeping         = old_sleeping;
    if (ret == -EAGAIN) {
        // Timed out, the DA is asleep
        if (change_state) {
            send_zbus_tri_event(DA_EVENT_TYPE_IS_SLEEPING, DA_STATE_KNOWN_TRUE, &(da_state.is_sleeping));
        }
        return DA_STATE_KNOWN_TRUE;
    } else if (ret == 0) {
        // If it responded with OK then it is awake
        if (change_state) {
            send_zbus_tri_event(DA_EVENT_TYPE_IS_SLEEPING, DA_STATE_KNOWN_FALSE, &(da_state.is_sleeping));
        }
        return DA_STATE_KNOWN_TRUE;
    } else {
        // If it responded with ERROR or some other problem,
        // then we are unsure
        LOG_ERR("'%s'(%d) received checking if DA is sleeping", wstrerr(-ret), ret);
        if (change_state) {
            send_zbus_tri_event(DA_EVENT_TYPE_IS_SLEEPING, DA_STATE_UNKNOWN, &(da_state.is_sleeping));
        }
        return ret;
    }
}

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
int wifi_time_to_next_wake()
{
    uint64_t now   = k_uptime_get();
    int64_t  delta = now - g_last_sleep_time;

    if (delta < CONFIG_WIFI_AFTER_SLEEP_WAIT_DEFAULT) {
        return (CONFIG_WIFI_AFTER_SLEEP_WAIT_DEFAULT - delta);
    } else {
        return 0;
    }
}

/////////////////////////////////////////////////////////////////////////
// wifi_check_sleep_mode()
//
// Semi-non-destructively check to see of the DA is in DPM mode.
// We send a wake up signal to the DA and look for the response if any.
// What happens depeneds on the mode that the DA is in:
// 1. If the DA is in DPM mode and asleep, it will respond with a
//	  \r\n+INIT:WAKEUP,<type>\r\n message.
// 2. If the DA is in DPM mode and awake, it will respond with a
//    \r\n+RUN:RTCWAKEUP\r\n
// 3. If the DA is NOT in DPM mode, then it will not respond at all
// 4. If the DA is in RTC sleep, it will respond with a \r\n+INIT:DONE\r\n
//    but no longer be asleep.
//
//  @return : 0 when not in DPM mode and awake
//			  1 when in DPM mode and was asleep
//			  2 when in DPM mode and was awake
//			  3 when in RTC sleep and now awake
int wifi_check_sleep_mode()
{
    k_timeout_t       timeout   = K_MSEC(400);
    k_timepoint_t     timepoint = sys_timepoint_calc(timeout);
    wifi_wait_array_t wait_msgs;
    int               ret, result = -EBADE;
    uint64_t          now = k_uptime_get();

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (now - g_last_sleep_time < CONFIG_WIFI_AFTER_SLEEP_WAIT_DEFAULT) {
        // too soon after sleep
        return -EAGAIN;
    }

    if (wifi_get_mutex(timeout, __func__) != 0) {
        return -EBUSY;
    }
    timeout = sys_timepoint_timeout(timepoint);

    // There seems to be a bug in the DA where it ignores the first wake pulse we send
    // when the DA was just woken up from DPM sleep.  All subsequent pulses are responded to.
    // So we track when we tell it to wake and send an extra pulse if we think we are in DPM
    // mode and just told it to wake up.
    if (da_state.dpm_mode == DA_STATE_KNOWN_TRUE && g_last_dpm_wake) {
        // EAS XXX I have seen the DA react to the second pulse a minute later.  If we have
        // to write code to account for inconsistent behavior, we should do the "right
        // thing" and handle the problems with that so we can complain to Renesys about it.
        g_last_dpm_wake = 0;
    }
    wifi_wake_DA(5);
    k_sleep(K_MSEC(5));    // leave it down for long enough to be seen as two pulses
    wifi_wake_DA(5);

    wait_msgs.num_msgs = 0;                                          // Initialize the structure
    wifi_add_wait_msg(&wait_msgs, "+INIT:WAKEUP", true, 0);          // Returned in DPM, asleep
    wifi_add_wait_msg(&wait_msgs, "+RUN:RTCWAKEUP", true, 0);        // Returned in DPM, awake
    wifi_add_wait_msg(&wait_msgs, "+INIT:DONE,0,DPM=0", true, 0);    // Returned in RTC sleep
    wifi_add_wait_msg(
        &wait_msgs, "+INIT:DONE,0,DPM=1", true, 0);        // Returned in DPM, was sleeping, now awake, AP not connected
    wifi_add_wait_msg(&wait_msgs, "+RUN:POR", true, 0);    // Returned in DPM, was awake now awake, AP ?? connected
    ret = wifi_wait_for(&wait_msgs, timeout);
    if (ret < 0) {
        // No response, we are not in DPM or asleep
        if (da_state.dpm_mode == DA_STATE_KNOWN_TRUE && da_state.ap_connected == DA_STATE_KNOWN_FALSE) {
            // There is a bug in the DA where if it was in DPM and not connected to the AP
            // then it responds to the first wake up pulse but NOT the second but DOES respond to the 3rd+
            // EAS XXX once we feel like we are tracking the shadow state correctly, and Don't need to call
            // this routine to double check, we can ignore it, just warn for now since we can't know
            LOG_WRN(
                "The DA has a bug when its in DPM without a AP where it responds to the 1sr and 3rd+ wakeup "
                "pulses, but not the second.  HEADS UP");
        }
        send_zbus_tri_event(DA_EVENT_TYPE_DPM_MODE, DA_STATE_KNOWN_FALSE, &(da_state.dpm_mode));
        send_zbus_tri_event(DA_EVENT_TYPE_IS_SLEEPING, DA_STATE_KNOWN_FALSE, &(da_state.is_sleeping));
        result = 0;
    }
    if (ret == 0) {    //"+INIT:WAKEUP", true, 0);		// Returned in DPM, asleep
        send_zbus_tri_event(DA_EVENT_TYPE_DPM_MODE, DA_STATE_KNOWN_TRUE, &(da_state.dpm_mode));
        send_zbus_tri_event(DA_EVENT_TYPE_IS_SLEEPING, DA_STATE_KNOWN_TRUE, &(da_state.is_sleeping));
        result = 1;
    }
    if (ret == 3) {    //"+INIT:DONE,0,DPM=1", true, 0);	// Returned in DPM, was sleeping, now awake, AP not connected
        send_zbus_tri_event(DA_EVENT_TYPE_DPM_MODE, DA_STATE_KNOWN_TRUE, &(da_state.dpm_mode));
        send_zbus_tri_event(DA_EVENT_TYPE_IS_SLEEPING, DA_STATE_KNOWN_FALSE, &(da_state.is_sleeping));
        result = 2;
    }
    if (ret == 1 ||    //"+RUN:RTCWAKEUP", true, 0);	// Returned in DPM, awake
        ret == 4) {    //"+RUN:POR", true, 0);			// Returned in DPM, was awake now awake, AP ?? connected
        send_zbus_tri_event(DA_EVENT_TYPE_DPM_MODE, DA_STATE_KNOWN_TRUE, &(da_state.dpm_mode));
        send_zbus_tri_event(DA_EVENT_TYPE_IS_SLEEPING, DA_STATE_KNOWN_FALSE, &(da_state.is_sleeping));
        result = 2;
    }
    if (ret == 2) {    //"+INIT:DONE,0,DPM=0", true, 0);	// Returned in RTC sleep
        send_zbus_tri_event(DA_EVENT_TYPE_DPM_MODE, DA_STATE_KNOWN_FALSE, &(da_state.dpm_mode));
        send_zbus_tri_event(DA_EVENT_TYPE_IS_SLEEPING, DA_STATE_KNOWN_FALSE, &(da_state.is_sleeping));
        send_zbus_timestamp_event(DA_EVENT_TYPE_RTC_WAKE_TIME, now, &(da_state.rtc_wake_time));
        result = 3;
    }

    wifi_release_mutex();
    return result;
}

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
int wifi_wake_no_sleep(k_timeout_t timeout)
{
    int      ret = 0;
    uint64_t now = k_uptime_get();

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (now - g_last_sleep_time < CONFIG_WIFI_AFTER_SLEEP_WAIT_DEFAULT) {
        // too soon after sleep
        return -EAGAIN;
    }

    if (wifi_get_mutex(timeout, __func__) != 0) {
        return -EBUSY;
    }
    wifi_flush_msgs();    // Sometimes the DA wakes up and sends us a
                          // INIT:WAKEUP message so there may be one
                          // in the queue. flush it

    // wifi_check_sleep_mode() will leave the DA awake for a short
    // time.  If it was in DPM mode and sleeping, if will soon go
    // back to sleep. If it was in RTC sleep, it will stay awake.
    // If it was in DPM mode and awake, it will stay awake.
    // If it was in DPM mode and abnormal sleep it will sleep soon.
    int state = wifi_check_sleep_mode();
    if (state == 0 || state == 3) {    // states 0 and 3 are not dpm and won't sleep
        LOG_DBG("DA is not in DPM mode and will stay awake");
        goto wns_exit;
    }

    // Either the DA was asleep and we just woke it or it was in DPM
    // and awake and we re-woke it

    // The DA may sleep soon so we have to send the commands to
    // keep the DA from doing that.

    // Being awake may be temp so the is_sleeping may be currently set
    // so we need to unset it to talk to the DA without errors
    das_tri_state_t old  = da_state.is_sleeping;
    da_state.is_sleeping = DA_STATE_KNOWN_FALSE;

    // Even if the DA is already prevented from sleeping, this is ok to do
    if ((ret = wifi_send_ok_err_atcmd("AT+MCUWUDONE", NULL, K_MSEC(50))) != 0) {
        LOG_ERR("'%s'(%d) sending a MCUWUDONE", wstrerr(-ret), ret);
        da_state.is_sleeping = DA_STATE_KNOWN_TRUE;    // if we fail we will sleep
        goto wns_exit;
    }

    if ((ret = wifi_send_ok_err_atcmd("AT+CLRDPMSLPEXT", NULL, K_MSEC(50))) != 0) {
        LOG_ERR("'%s'(%d) sending CLRDPMSLPEXT to keep DA awake", wstrerr(-ret), ret);
        da_state.is_sleeping = DA_STATE_KNOWN_TRUE;    // if we fail we will sleep
        wifi_release_mutex();
        goto wns_exit;
    }
    LOG_DBG("DA is in DPM awake and will stay awake");
    g_last_dpm_wake      = k_uptime_get();
    da_state.is_sleeping = old;    // resdtore the old value send_zbus.. will send the event properly
    send_zbus_tri_event(DA_EVENT_TYPE_IS_SLEEPING, DA_STATE_KNOWN_FALSE, &(da_state.is_sleeping));

wns_exit:
    wifi_release_mutex();
    return ret;
}

////////////////////////////////////////////////////////////////////////////////
// wifi_dpm_back_to_sleep()
//
// Tell the DA it can go back to sleep if it was woken up from dpm mode
//
//  @param timeout - timeout for the write
//
//  @return - < 0 = error, 0 = success
int wifi_dpm_back_to_sleep(k_timeout_t timeout)
{
    // At spi speed of 4Mhz, my tests show this never takes more then 10ms
    int ret;

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    // If we are not in DPM mode, we can't sleep, so set DPM
    if (da_state.dpm_mode != DA_STATE_KNOWN_TRUE) {
        ret = wifi_set_dpm_state(1, false, timeout);
        if (ret != 0) {
            LOG_ERR("Can't set DPM mode to go to sleep: %d", ret);
            return ret;
        }
    } else {
        ret = wifi_send_ok_err_atcmd("AT+SETDPMSLPEXT", NULL, timeout);
        if (ret != -EAGAIN && ret < 0) {
            LOG_ERR("Error allowing dmp sleep");
        } else {
            ret = 0;
            // The main reason for a time
            send_zbus_tri_event(DA_EVENT_TYPE_IS_SLEEPING, DA_STATE_KNOWN_TRUE, &(da_state.is_sleeping));
        }
    }
    return ret;
}

static bool wait_for_sleep_state(das_tri_state_t state)
{
    int cnt = 0;
    while (cnt < 8) {
        int ret = wifi_check_sleeping(false);
        // If we get error checking the sleep state, stop trying
        if (ret < -1) {
            return false;
        }
        if (da_state.is_sleeping == state) {
            return true;
        }
        k_sleep(K_MSEC(10));
        cnt++;
    }
    return false;
}

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
int wifi_set_sleep_mode(wifi_sleep_state_t sleep_state, int dur_ms)
{
    int   ret = 0;
    char *op;

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    ret = wifi_get_mutex(K_SECONDS(1), __func__);
    if (ret != 0) {
        LOG_ERR("'%s'(%d) getting wifi mutex to change sleep mode", wstrerr(-ret), ret);
        return ret;
    }
    // Verify the sleep state before continuing
    wifi_check_sleeping(true);

    LOG_DBG(
        "Setting sleep state to %d, curr state: dpm: %d, sleep: %d", sleep_state, da_state.dpm_mode, da_state.is_sleeping);

    switch (sleep_state) {
    case WIFI_SLEEP_NONE:    // !dpm&!sleep
        if (da_state.dpm_mode == DA_STATE_KNOWN_FALSE && da_state.is_sleeping == DA_STATE_KNOWN_FALSE) {
            op = "wait_for_sleep_state";
            if (!wait_for_sleep_state(DA_STATE_KNOWN_FALSE)) {
                LOG_ERR("We thought we were awake but can't talk to DA");
                ret = -ENXIO;
            }
            break;
        }
        // in dpm&!sleep or !dpm&sleep or dpm&sleep
        if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
            op  = "wifi_wake_no_sleep";
            ret = wifi_wake_no_sleep(K_MSEC(400));
        }
        // The DA will reboot regardless if it is already in the DPM mode we desire
        // so we need to know for sure what DPM mode it is in before we tell it to
        // change or we cause thrash
        if (ret == 0) {
            op  = "wifi_get_dpm_state";
            ret = wifi_get_dpm_state(K_MSEC(1000));
        }
        // ret now holds dpm state or < 0 if error
        // in dpm&!sleep or err
        if (ret == 1) {
            // The DA throws errors if it is connected to an AP when we change modes.
            // So we need to disconnect first and let the system reconnect
            if (da_state.ap_connected == DA_STATE_KNOWN_TRUE) {
                op  = "wifi_disconnect_from_AP";
                ret = wifi_disconnect_from_AP(K_MSEC(1000));
            }
            op  = "wifi_set_dpm_state";
            ret = wifi_set_dpm_state(false, false, K_MSEC(1000));
            // give the da time to boot
            k_sleep(K_MSEC(1500));
        }
        if (ret == 0) {
            op = "wait_for_sleep_state";
            if (wait_for_sleep_state(DA_STATE_KNOWN_FALSE) == false) {
                LOG_ERR("Failed to set sleep state to none");
                ret = -ENXIO;
            }
        }
        break;

    case WIFI_SLEEP_DPM_ASLEEP:    // dpm&sleep
        if (da_state.dpm_mode == DA_STATE_KNOWN_TRUE && da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
            op = "wait_for_sleep_state";
            if (!wait_for_sleep_state(DA_STATE_KNOWN_TRUE)) {
                LOG_ERR("We thought we were asleep but can talk to DA");
                ret = -ENXIO;
            }
            break;
        }
        // in dpm&!sleep or !dpm&sleep or !dpm&!sleep
        if (da_state.dpm_mode == DA_STATE_KNOWN_TRUE) {
            op  = "wifi_dpm_back_to_sleep";
            ret = wifi_dpm_back_to_sleep(K_MSEC(4000));
        } else {
            // in !dpm&!sleep or !dpm&sleep
            if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
                op  = "wifi_wake_no_sleep";
                ret = wifi_wake_no_sleep(K_MSEC(400));
            }
            // The DA will reboot regardless if it is already in the DPM mode we desire
            // so we need to know for sure what DPM mode it is in before we tell it to
            // change or we cause thrash
            if (ret == 0) {
                op  = "wifi_get_dpm_state";
                ret = wifi_get_dpm_state(K_MSEC(1000));
            }
            // ret now holds dpm state or < 0 if error
            // in !dpm&!sleep || err
            if (ret == 0) {
                // Wasn't in DPM mode, change it
                // The DA throws errors if it is connected to an AP when we change modes.
                // So we need to disconnect first and let the system reconnect
                if (da_state.ap_connected == DA_STATE_KNOWN_TRUE) {
                    op  = "wifi_disconnect_from_AP";
                    ret = wifi_disconnect_from_AP(K_MSEC(1000));
                }
                op  = "wifi_set_dpm_state";
                ret = wifi_set_dpm_state(true, false, K_MSEC(1000));
                // give the da time to boot
                k_sleep(K_MSEC(1500));
            } else {
                // Was in dpm mode, just tell it to sleep
                op  = "wifi_dpm_back_to_sleep";
                ret = wifi_dpm_back_to_sleep(K_MSEC(300));
            }
        }
        if (ret == 0) {
            op = "wait_for_sleep_state";
            if (wait_for_sleep_state(DA_STATE_KNOWN_TRUE) == false) {
                LOG_ERR("Failed to set sleep state to DPM sleep");
                ret = -ENXIO;
            }
            g_last_sleep_time = k_uptime_get();
        }
        break;

    case WIFI_SLEEP_DPM_AWAKE:    // dpm&!sleep
        if (da_state.dpm_mode == DA_STATE_KNOWN_TRUE && da_state.is_sleeping == DA_STATE_KNOWN_FALSE) {
            op = "wait_for_sleep_state";
            if (!wait_for_sleep_state(DA_STATE_KNOWN_FALSE)) {
                LOG_ERR("We thought we were awake but can't talk to DA");
                ret = -ENXIO;
            }
            break;
        }
        // in dpm&sleep or !dpm&sleep or !dpm&!sleep
        if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
            op  = "wifi_wake_no_sleep";
            ret = wifi_wake_no_sleep(K_MSEC(400));
        }
        // The DA will reboot regardless if it is already in the DPM mode we desire
        // so we need to know for sure what DPM mode it is in before we tell it to
        // change or we cause thrash
        if (ret == 0) {
            op  = "wifi_get_dpm_state";
            ret = wifi_get_dpm_state(K_MSEC(1000));
        }
        // ret now holds dpm state or < 0 if error
        // in !dpm&!sleep or err
        if (ret == 0) {
            // The DA throws errors if it is connected to an AP when we change modes.
            // So we need to disconnect first and let the system reconnect
            if (da_state.ap_connected == DA_STATE_KNOWN_TRUE) {
                op  = "wifi_disconnect_from_AP";
                ret = wifi_disconnect_from_AP(K_MSEC(1000));
            }
            op  = "wifi_set_dpm_state";
            ret = wifi_set_dpm_state(true, true, K_MSEC(1000));
            // give the da time to boot
            k_sleep(K_MSEC(1000));
        } else {
            // We are in the right dpm mode, so no need to change it
            ret = 0;
        }
        if (ret == 0) {
            op = "wait_for_sleep_state";
            if (wait_for_sleep_state(DA_STATE_KNOWN_FALSE) == false) {
                LOG_ERR("Failed to set sleep state to DPM awake");
                ret = -ENXIO;
            }
        }
        break;

    case WIFI_SLEEP_RTC_ASLEEP:    // !dpm&sleep
        if (da_state.dpm_mode == DA_STATE_KNOWN_FALSE && da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
            op = "wait_for_sleep_state";
            if (!wait_for_sleep_state(DA_STATE_KNOWN_TRUE)) {
                LOG_ERR("We thought we were asleep but can talk to DA");
                ret = -ENXIO;
            }
            break;
        }
        // in dpm&!sleep or !dpm&sleep or !dpm&!sleep
        if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
            op = "wifi_wake_no_sleep";
            LOG_DBG("wifi_wake_no_sleep");
            ret = wifi_wake_no_sleep(K_MSEC(400));
        }
        if (ret == 0) {
            op = "wifi_rtc_sleep";
            LOG_DBG("wifi_rtc_sleep");
            ret = wifi_rtc_sleep(dur_ms);
        }
        if (ret == 0) {
            op = "wait_for_sleep_state";
            if (wait_for_sleep_state(DA_STATE_KNOWN_TRUE) == false) {
                LOG_ERR("Failed to set sleep state to rtc sleep");
                ret = -ENXIO;
            }
            g_last_sleep_time = k_uptime_get();
        }
        break;
    }

    if (ret < 0) {
        LOG_ERR("'%s'(%d) from %s", wstrerr(-ret), ret, op);
        // If an operation failed, try to detect our current sleep state
        wifi_check_sleeping(true);
    }
    LOG_DBG("done");
    wifi_release_mutex();
    return ret;
}

/////////////////////////////////////////////////////////////////////////
// wifi_set_ntp_server()
//
// Set the NTP server for the DA to pool.ntp.org
//
//  @return - 0 on success
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EINVAL if new_topics if badly formed
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping or mqtt broker is connected
int wifi_set_ntp_server()
{
    int ret = wifi_send_ok_err_atcmd("AT+NWSNTP=1,pool.ntp.org,86400", NULL, K_MSEC(1000));
    if (ret < 0) {
        LOG_ERR("'%s'(%d) Setting the ntp server", wstrerr(-ret), ret);
        return ret;
    }
    send_zbus_tri_event(DA_EVENT_TYPE_NTP_SERVER_SET, DA_STATE_KNOWN_TRUE, &(da_state.ntp_server_set));
    return 0;
}

/////////////////////////////////////////////////////////
// connect_param_check
//   Check the parameters passed in for connect and
// return 0 if they are ok to use.
static int connect_param_check(char *ssid, char *key, int sec, int keyidx, int enc, int hidden, char *cmd)
{
    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("DA is in DPM mode and asleep, can't connect to ssid");
        return -ENXIO;
    }

    if (strlen(ssid) > 32) {
        LOG_ERR("SSID too long");
        return -EINVAL;
    }
    if (sec < 0 || sec > 7) {
        LOG_ERR("Invalid sec");
        return -EINVAL;
    }
    if (hidden < 0 || hidden > 1) {
        LOG_ERR("Invalid hidden");
        return -EINVAL;
    }

    switch (sec) {
    case 0:
    case 5:
        snprintf(cmd, 128, "AT+WFJAP='%s',%d,%d", ssid, sec, hidden);
        break;
    case 1:
        if (strlen(key) > 63) {
            LOG_ERR("Key too long");
            return -EINVAL;
        }
        if (keyidx < 0 || keyidx > 3) {
            LOG_ERR("Invalid keyidx");
            return -EINVAL;
        }
        snprintf(cmd, 128, "AT+WFJAP='%s',%d,%d,%s,%d", ssid, sec, keyidx, key, hidden);
        break;
    default:
        if (strlen(key) > 63) {
            LOG_ERR("Key too long");
            return -EINVAL;
        }
        if (enc < 0 || enc > 2) {
            LOG_ERR("Invalid enc");
            return -EINVAL;
        }
        snprintf(cmd, 128, "AT+WFJAP='%s',%d,%d,%s,%d", ssid, sec, enc, key, hidden);
        break;
    }
    return 0;
}

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
int wifi_initiate_connect_to_ssid(char *ssid, char *key, int sec, int keyidx, int enc, int hidden, k_timeout_t timeout)
{
    int  ret = 0;
    char cmd[128];

    ret = connect_param_check(ssid, key, sec, keyidx, enc, hidden, cmd);
    if (ret != 0) {
        return ret;
    }

    ret = wifi_send_ok_err_atcmd(cmd, NULL, timeout);
    if (ret == 0) {
        if (wifi_num_saved_ssids() < MAX_SAVED_SSIDS) {
            // The DA is trying to connect.  We only want to add
            // this AP to the list of safe AP if it works and if
            // we never connected to this AP before.  So it this
            // SSID is not in the list, save the info so that if
            // it successfully connects, we can store the info
            int i = wifi_find_saved_ssid(ssid);
            // Not in the list and not too many
            if (i < 0) {
                LOG_DBG("issued a connect for new ap, staging for adding to saved if succeeds");
                net_stage_ssid_for_saving(ssid, key, sec, keyidx, enc, hidden);
            }
        }
    }
    return ret;
}

typedef struct
{
    int scan_idx;
    int comparison_score;
} qsort_ssids_t;
#define NO_SCORE (-300)

static qsort_ssids_t sorted_ssids[MAX_SAVED_SSIDS + 1];
static wifi_arr_t   *wifis;
// This is a comparison function for qsort.  It will sort
// the elements based on their comparison_score.
static int comp(const void *elem1, const void *elem2)
{
    qsort_ssids_t *f = (qsort_ssids_t *)elem1;
    qsort_ssids_t *s = (qsort_ssids_t *)elem2;

    if (f->comparison_score > s->comparison_score)
        return -1;
    if (f->comparison_score < s->comparison_score)
        return 1;
    return 0;
}

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
int wifi_check_for_known_ssid()
{
    shadow_zone_t zones[MAX_SAVED_SSIDS];
    int           widx;
    wifis   = wifi_get_last_ssid_list();
    int ret = wifi_get_ap_list(zones, K_SECONDS(1));
    if (ret < 0) {
        LOG_ERR("Failed to get the SSID list");
        return -1;
    }

    // Make a copy of the known SSIDs list that are also in the last
    // ssid scan

    // Set the comparison score to the rssi found and add 100 to that
    // so that the safe known SSIDs will have a higher score then
    // non-safe ones

    // We call qsort() to sort based on comparison_score

    // Once sorted, the SSID at the top of the sorted list is the
    // the safest known SSID with the strongest RSSI.
    int ssids_in_scan = 0;
    for (int i = 0; i < MAX_SAVED_SSIDS; i++) {
        if (zones[i].ssid[0] != 0) {
            widx = wifi_find_ssid_in_scan(zones[i].ssid);
            if (widx >= 0) {
                sorted_ssids[ssids_in_scan].scan_idx         = widx;
                sorted_ssids[ssids_in_scan].comparison_score = wifis->wifi[widx].rssi;
                if (zones[i].safe) {
                    sorted_ssids[ssids_in_scan].comparison_score += 100;
                }
                ssids_in_scan++;
            }
        }
    }

    // Sort by is_safe and rssi
    qsort(sorted_ssids, ssids_in_scan, sizeof(qsort_ssids_t), comp);

    // return the index into the known ssid list of the first entry in the
    // sorted ssid list
    if (ssids_in_scan > 0) {
        widx = sorted_ssids[0].scan_idx;
        return wifi_find_saved_ssid(wifis->wifi[widx].ssid);
    }

    // No known SSIDs were found in the scan
    return -1;
}

static void add_to_list(char *line, shadow_zone_t *zones)
{
    char *sub;
    int   len = strlen(line);
    for (int i = 0; i < MAX_SAVED_SSIDS; i++) {
        if (line[0] == ('0' + i) && line[1] == ',') {
            zones[i].idx = i;
            char *ssid   = strtok_r(line + 2, ",", &sub);
            if (ssid != NULL) {
                strncpy(zones[i].ssid, ssid, 32);
                zones[i].ssid[32] = 0;
                strtok_r(NULL, ",", &sub);    // sec
                strtok_r(NULL, ",", &sub);    // key
                strtok_r(NULL, ",", &sub);    // enc
                strtok_r(NULL, ",", &sub);    // hidden
                char *safe = strtok_r(NULL, ",", &sub);
                if (safe != NULL && (safe - line) < len) {
                    zones[i].safe = safe[0] == '1';
                }
            }
        }
    }
}

static shadow_zone_t cached_zones[MAX_SAVED_SSIDS];
static bool          cached_zones_valid = false;
/////////////////////////////////////////////////////////
int wifi_add_SSID_to_cached_list(char *line)
{
    add_to_list(line, cached_zones);
    cached_zones_valid = true;
    return 0;
}
/////////////////////////////////////////////////////////
// wifi_get_ap_list()
//   Retrieve the list of SSIDs stored on the DA and
// store them locally for reference.  This call will
// not ask the DA again if it already has the list since
// it can't changes unless we change it.
//
//  @param zones - a pointer to the array of shadow zones
//  @param timeout - timeout for the get
// Example max response:
//      \r\n+SSIDLIST:\r\n
//		1,AB012345678901234567890123456789,3,0,1,0,aaaa\r\n
//      2,AB012345678901234567890123456789,3,0,1,0,bbbb\r\n
//      3,AB012345678901234567890123456789,3,0,1,0,cccc\r\n
//      4,AB012345678901234567890123456789,3,0,1,0,dddd\r\n
//      5,AB012345678901234567890123456789,3,0,1,0,eeee\r\n
//
// linelen * 5 + header + pad
int wifi_get_ap_list(shadow_zone_t *zones, k_timeout_t timeout)
{
    int ret;
    if (cached_zones_valid == true) {
        memcpy(zones, cached_zones, sizeof(shadow_zone_t) * MAX_SAVED_SSIDS);
        return 0;
    }
    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    k_timepoint_t timepoint = sys_timepoint_calc(timeout);
    if (wifi_get_mutex(timeout, __func__) != 0) {
        return -EBUSY;
    }

    memset(zones, 0, sizeof(shadow_zone_t) * MAX_SAVED_SSIDS);
    for (int i = 0; i < MAX_SAVED_SSIDS; i++) {
        zones[i].idx = i;
    }
    char *list = k_calloc(MAX_SSIDLIST_RESPONSE, 1);
    if (list == NULL) {
        LOG_ERR("Failed to allocate memory for SSID list");
        ret = -ENOMEM;
        goto aplist_exit;
    }

    char              errstr[64];
    wifi_wait_array_t wait_msgs;
    timeout = sys_timepoint_timeout(timepoint);
    if ((ret = wifi_send_timeout("AT+SSIDLIST=", timeout)) != 0) {
        LOG_ERR("'%s'(%d) sending at+SSIDLIST=", wstrerr(-ret), ret);
        goto aplist_exit;
    }

    wait_msgs.num_msgs = 0;    // Initialize the structure
    wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
    wifi_add_wait_msg(&wait_msgs, "\r\nERROR:%19s\r\n", true, 1, errstr);
    wifi_add_wait_msg(&wait_msgs, "%300s", true, 1, list);
    bool got_ok   = false;
    bool got_list = false;
    while (1) {
        timeout = sys_timepoint_timeout(timepoint);
        ret     = wifi_wait_for(&wait_msgs, timeout);
        if (ret < 0) {
            LOG_ERR("'%s'(%d) waiting for at+SSIDLIST", wstrerr(-ret), ret);
            goto aplist_exit;
        }
        if (ret == 0) {
            got_ok = true;
        }
        if (ret == 1) {
            // If there is an error code, return that
            if (errstr[0] == '-') {
                ret = -(atoi(errstr + 1));
            } else {
                LOG_ERR("Unknown error str received: %s ", errstr);
                ret = -EBADE;
            }
            goto aplist_exit;
        }
        if (ret == 2) {
            char *sub = strstr(list, "+SSIDLIST:");
            if (sub != NULL) {
                char *line = strtok_r(list, "\n", &sub);
                if (line == NULL) {
                    LOG_ERR("No lines in SSIDLIST response");
                    ret = -EBADE;
                    goto aplist_exit;
                }
                while (line != NULL && (line - list) < MAX_SSIDLIST_RESPONSE) {
                    add_to_list(line, zones);
                    line = strtok_r(NULL, "\n", &sub);
                }
                got_list = true;
            }
        }
        if (got_list && got_ok) {
            break;
        }
    }

    if (got_list) {
        memcpy(cached_zones, zones, sizeof(shadow_zone_t) * MAX_SAVED_SSIDS);
        cached_zones_valid = true;
    }
aplist_exit:
    wifi_release_mutex();
    if (list != NULL) {
        k_free(list);
    }
    return ret;
}

/////////////////////////////////////////////////////////
// wifi_saved_ssid_name()
//  Get the safe flag for the index into the known SSID
// list
//
//  @param idx - the index of the ssid to get the name of
//
//  @return - the safe flag of the ssid
bool wifi_saved_ssid_safe(int idx)
{
    if (cached_zones_valid == false) {
        LOG_ERR("Cached zones not valid when getting safe flag");
        return false;
    }
    if (idx < 0 || idx >= MAX_SAVED_SSIDS) {
        return false;
    }
    return cached_zones[idx].safe;
}

/////////////////////////////////////////////////////////
// wifi_find_saved_ssid()
//  Find the index of the ssid in the list the ssids the
// DA has credentials for.
//
//  @param ssid - the ssid to look for
//
//  @return - index of the ssid in the list or -1
//		 if not found
int wifi_find_saved_ssid(char *ssid)
{
    if (cached_zones_valid == false) {
        LOG_ERR("Cached zones not valid when looking for %s", ssid);
        return -1;
    }
    for (int i = 0; i < MAX_SAVED_SSIDS; i++) {
        if (strncmp(ssid, cached_zones[i].ssid, 32) == 0) {
            return i;
        }
    }
    return -1;
}

/////////////////////////////////////////////////////////
// wifi_get_saved_ssid_by_index()
//  return the ssid name from the saved ssid list at the
// specified index
//
//  @param idx - the index into the save ssid
//
//  @return - pointe to the name of the ssid (may be null)
char *wifi_get_saved_ssid_by_index(int idx)
{
    if (cached_zones_valid == false) {
        LOG_ERR("Cached zones not valid when getting ssid %d", idx);
        return NULL;
    }
    if (idx < 0 || idx >= MAX_SAVED_SSIDS) {
        return NULL;
    }
    if (cached_zones[idx].ssid[0] == 0) {
        return NULL;
    }
    return cached_zones[idx].ssid;
}

/////////////////////////////////////////////////////////S
// wifi_num_saved_ssids()
//  Count the number of ssids the DA has credentials for
//
//  @return - the number of ssids
int wifi_num_saved_ssids()
{
    int cnt = 0;
    if (cached_zones_valid == false) {
        LOG_ERR("Cached zones not valid when counting ssids");
        return 0;
    }
    for (int i = 0; i < MAX_SAVED_SSIDS; i++) {
        if (cached_zones[i].ssid[0] != 0) {
            cnt++;
        }
    }
    return cnt;
}

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
    int idx, char *name, char *pass, uint16_t sec, uint16_t keyidx, uint16_t enc, bool hidden, bool safe, k_timeout_t timeout)
{
    int i;
    if (cached_zones_valid == false) {
        LOG_ERR("Cached zones not valid when adding ssids");
        return -EINVAL;
    }

    if (idx == -1) {
        for (i = 0; i < MAX_SAVED_SSIDS; i++) {
            if (cached_zones[i].ssid[0] == 0) {
                idx = i;
            }
        }
    }

    if (idx < 0 || idx >= MAX_SAVED_SSIDS) {
        return -EINVAL;
    }

    char cmd[150];
    sprintf(cmd, "AT+SSIDINSERT=%d,%s,%s,%d,%d,%d,%d,%d", idx, name, pass, sec, keyidx, enc, hidden, safe);
    int ret = wifi_send_ok_err_atcmd(cmd, NULL, timeout);
    if (ret < 0) {
        LOG_ERR("'%s'(%d) inserting new ssids", wstrerr(-ret), ret);
        return ret;
    }
    cached_zones[idx].idx  = idx;
    cached_zones[idx].safe = safe;
    strncpy(cached_zones[idx].ssid, name, 32);
    cached_zones[idx].ssid[32] = 0;

    return 0;
}

////////////////////////////////////////////////////////////
// wifi_saved_ssids_del()
//
// delete a saved_ssid from the list
//
// @param idx - the index of the ssid to delete
// @param timeout - timeout for the operation
//
// @return - 0 on success, -EINVAL if the index is out of range
int wifi_saved_ssids_del(int idx, k_timeout_t timeout)
{
    int ret;
    if (cached_zones_valid == false) {
        LOG_ERR("Cached zones not valid when deleting ssids");
        return -EINVAL;
    }

    if (idx < 0 || idx >= MAX_SAVED_SSIDS) {
        return -EINVAL;
    }

    char cmd[20];
    sprintf(cmd, "AT+SSIDDELETE=%d", idx);
    ret = wifi_send_ok_err_atcmd(cmd, NULL, timeout);
    if (ret < 0) {
        LOG_ERR("'%s'(%d) deleting ssids %d", wstrerr(-ret), ret, idx);
        return ret;
    }

    cached_zones[idx].ssid[0] = 0;
    cached_zones[idx].safe    = false;

    return 0;
}

////////////////////////////////////////////////////////////
// wifi_saved_ssids_del_all()
//
// delete all saved_ssids from the DA
//
// @param timeout - timeout for the operation
//
// @return - 0 on success, -EINVAL if the index is out of range
int wifi_saved_ssids_del_all(k_timeout_t timeout)
{
    int ret;

    char cmd[20];
    sprintf(cmd, "AT+SSIDDELALL");

    ret = wifi_send_ok_err_atcmd(cmd, NULL, timeout);
    if (ret < 0) {
        LOG_ERR("'%s'(%d) deleting all ssids", wstrerr(-ret), ret);
        return ret;
    }
    wifi_clear_local_ssid_list();

    return 0;
}

////////////////////////////////////////////////////////////
// wifi_clear_local_ssid_list
void wifi_clear_local_ssid_list()
{
    for (int idx = 0; idx < MAX_SAVED_SSIDS; idx++) {
        cached_zones[idx].ssid[0] = 0;
        cached_zones[idx].safe    = false;
    }
}

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
int wifi_set_zone_safe(int idx, bool safe_flag, k_timeout_t timeout)
{
    int ret;
    if (cached_zones_valid == false) {
        LOG_ERR("Cached zones not valid when changing ssid flags");
        return -EINVAL;
    }

    if (idx < 0 || idx >= MAX_SAVED_SSIDS) {
        return -EINVAL;
    }

    char cmd[30];
    sprintf(cmd, "AT+SSIDCHANGEFLAG=%d,%d", idx, safe_flag);
    ret = wifi_send_ok_err_atcmd(cmd, NULL, timeout);
    if (ret < 0) {
        LOG_ERR("'%s'(%d) changing ssid flags %d", wstrerr(-ret), ret, idx);
        return ret;
    }

    cached_zones[idx].safe = false;

    return 0;
}

/////////////////////////////////////////////////////////
// wifi_initiate_connect_by_index
//	Connect to the SSID specified by an index in the SSID
//  list held in the DA16200
//
//  @param idx - index of the SSID to connect to
//  @param timeout - timeout for the connect
int wifi_initiate_connect_by_index(int idx, k_timeout_t timeout)
{
    char cmd[20];

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    if (idx < 0 || idx >= MAX_SAVED_SSIDS) {
        LOG_ERR("SSID index %d invalid", idx);
        return -EINVAL;
    }

    if (cached_zones[idx].ssid[0] == 0) {
        LOG_ERR("SSID at index %d is empty", idx);
        return -EINVAL;
    }

    sprintf(cmd, "AT+SSIDIDX=%d", idx);
    int ret = wifi_send_ok_err_atcmd(cmd, NULL, timeout);
    return ret;
}

//////////////////////////////////////////////////////////
// wifi_disconnect_from_ap()
//  Disconnect from the current AP
//
//  @param timeout - timeout for the disconnect
int wifi_disconnect_from_AP(k_timeout_t timeout)
{
    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    if (wifi_get_mutex(timeout, __func__) != 0) {
        return -EBUSY;
    }

    // Optional
    wifi_send_ok_err_atcmd("AT+WFDIS=1", NULL, timeout);

    int ret = wifi_send_ok_err_atcmd("AT+WFQAP", NULL, timeout);
    if (ret != 0) {
        goto disconnect_exit;
    }

disconnect_exit:
    wifi_release_mutex();
    return ret;
}

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
int wifi_is_curr_AP(char *ssid, char *password, uint16_t sec, uint16_t keyidx, uint16_t enc)
{
    char cmd[150];

    if (strncmp(ssid, da_state.ap_name, 32) != 0) {
        return 2;
    }
    int i = wifi_find_saved_ssid(ssid);
    if (i < 0) {
        return 2;
    }
    // We know that the DA knows about this AP
    sprintf(cmd, "AT+SSIDCHECKCREDS=%s,%s,%d,%d,%d", ssid, password, sec, keyidx, enc);
    int ret = wifi_send_ok_err_atcmd(cmd, NULL, K_MSEC(1000));
    if (ret == 0) {
        return 0;
    }
    return 1;
}

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
int wifi_get_rssi(int *rssi, k_timeout_t timeout)
{
    int               ret, result = -EAGAIN;
    wifi_wait_array_t wait_msgs;
    static char       cmd[20];
    char              errstr[50];

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    k_timepoint_t timepoint = sys_timepoint_calc(timeout);
    if (wifi_get_mutex(timeout, __func__) != 0) {
        return -EBUSY;
    }

    timeout = sys_timepoint_timeout(timepoint);
    strncpy(cmd, "AT+WFRSSI", 20);
    if ((ret = wifi_send_timeout(cmd, timeout)) != 0) {
        result = ret;
        goto getrssi_exit;
    }

    cmd[0]             = 0;    // This will hold the data we receive
    wait_msgs.num_msgs = 0;    // Initialize the structure
    wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
    wifi_add_wait_msg(&wait_msgs, "+RSSI:%20s", true, 1, cmd);
    wifi_add_wait_msg(&wait_msgs, "\r\nERROR%50s\r\n", true, 1, errstr);
    bool got_ok   = false;
    bool got_rssi = false;
    while (1) {
        timeout = sys_timepoint_timeout(timepoint);
        ret     = wifi_wait_for(&wait_msgs, timeout);
        if (ret < 0) {
            result = ret;
            break;
        }
        if (ret == 0) {
            got_ok = true;
        }
        if (ret == 1) {
            got_rssi = true;
            if (rssi != NULL) {    // RSSI is now part of da_state so its optional to return it
                if (strncmp(cmd, "NOT_CONN", 8) == 0) {
                    LOG_ERR("RSSI Not conn");
                    *rssi = 0;
                    ret   = -EBADE;
                }
                *rssi = strtol(cmd, NULL, 10);
            }
        }
        if (ret == 2) {
            result = -EBADE;
            break;
        }
        if (got_ok && got_rssi) {
            result = 0;
            break;
        }
    }
getrssi_exit:
    wifi_release_mutex();
    return result;
}

/////////////////////////////////////////////////////////////////////////
// wifi_http_get()
//
// Retrieve the contents of a http or https get into a little fs file
//
//  @return - 0 on success
//			 -EAGAIN if timeout
//			 -EBADE if ERROR was received
//			 -EINVAL if new_topics if badly formed
//			 -EBUSY if mutex failure
//			 -ENXIO if in sleeping or mqtt broker is connected
int wifi_http_get(char *url, char *filename, bool skip_hearders, k_timeout_t timeout)
{
    int               ret, flen, ulen;
    char              errstr[50];
    struct fs_statvfs vfsbuf;
    k_timepoint_t     timepoint = sys_timepoint_calc(timeout);

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    ulen = strlen(url);
    if (ulen > MAXURL - 40) {
        LOG_ERR("url too long");
        return -EINVAL;
    }

    flen = strlen(filename);
    if (flen > HTTPFLEN - 7) {
        LOG_ERR("file name too long");
        return -EINVAL;
    }

    if (file_opened) {
        LOG_DBG("HTTP download already in progress");
        return -EBUSY;
    }

    ret = fs_statvfs("/lfs1", &vfsbuf);
    if (ret != 0) {
        LOG_ERR("Error getting fs stats");
        return ret;
    }

    if ((vfsbuf.f_bsize * vfsbuf.f_bfree) < 1024) {
        LOG_ERR("File system is full! < 1k left");
        return -ENOSPC;
    }

    snprintf(httpfilename, HTTPFLEN, "/lfs1/%s", filename);
    http_skip_headers = skip_hearders;
    fs_file_t_init(&httpfd);
    ret = fs_open(&httpfd, httpfilename, FS_O_CREATE | FS_O_RDWR);
    if (ret < 0) {
        LOG_ERR("FAIL: open %s: %d", httpfilename, ret);
        return -EIO;
    }
    file_opened      = true;
    http_crc         = 0;
    http_amt_written = 0;
    if (wifi_get_mutex(timeout, __func__) != 0) {
        fs_close(&httpfd);
        file_opened = false;
        return -EBUSY;
    }

    timeout = sys_timepoint_timeout(timepoint);
    snprintf(httpgetcmd, MAXURL + 40, "AT+NWHTCH=%s,get", url);
    ret = wifi_send_ok_err_atcmd(httpgetcmd, errstr, timeout);
    if (ret != 0) {
        fs_close(&httpfd);
        file_opened = false;
        wifi_release_mutex();
        return ret;
    }

    wifi_release_mutex();
    return 0;
}

int wifi_at_get_http_amt_downloaded()
{
    return http_amt_written;
}

void wifi_at_http_status(wifi_msg_t *msg)
{
    char *sub = strstr(msg->data, "+NWHTCSTATUS:");
    if (sub == NULL) {
        LOG_ERR("No status found in http response");
        wifi_msg_free(msg);
        return;
    }
    if (file_opened == false) {
        LOG_ERR("File not open for writing");
        wifi_msg_free(msg);
        return;
    }
    // +NWHTCSTATUS:14
    int newcode = strtol(sub + 13, NULL, 10);

    send_zbus_int_event(DA_EVENT_TYPE_HTTP_COMPLETE, newcode, &httpresultcode, true);

    fs_close(&httpfd);
    file_opened = false;
    LOG_DBG("HTTP DL Status: %d", newcode);
    wifi_msg_free(msg);
    return;
}

void wifi_at_http_write(wifi_msg_t *msg)
{
    char *sub = strstr(msg->data, "+NWHTCDATA:");
    int   writelen, datalen;

    if (sub == NULL) {
        LOG_ERR("No data found in http response");
        wifi_msg_free(msg);
        return;
    }
    if (file_opened == false) {
        LOG_ERR("File not open for writing");
        wifi_msg_free(msg);
        return;
    }
    sub += 11;
    char *datastart = strchr(sub, ',');
    if (datastart == NULL) {
        LOG_ERR("No comma found in http response");
        wifi_msg_free(msg);
        return;
    }
    *datastart = 0;
    datastart++;
    datalen = strtol(sub, NULL, 10);
    if (datalen < 1) {
        LOG_ERR("Data length is 0");
        wifi_msg_free(msg);
        return;
    }
    char *packetend = datastart + datalen;

    if (http_skip_headers) {
        sub = strstr(datastart, "\r\n\r\n");
        if (sub == NULL) {
            // If the is not \r\n\r\n then this first chunk is all headers
            LOG_DBG("Skipping HTTP headers");
            wifi_msg_free(msg);
            return;
        }
        datastart         = sub + 4;
        http_skip_headers = false;
    }

    writelen = packetend - datastart;
    if (writelen < 1) {
        LOG_DBG("No data left after skipping HTTP headers, no error");
        wifi_msg_free(msg);
        return;
    }
    if (http_amt_written == 0) {
        http_crc = crc32_ieee(datastart, writelen);
    } else {
        http_crc = crc32_ieee_update(http_crc, datastart, writelen);
    }
    //LOG_DBG("Writing %d bytes to file @ %ld, crc 0x%X", writelen, http_amt_written, http_crc);
    http_amt_written += writelen;
    if (fs_write(&httpfd, datastart, writelen) != writelen) {
        LOG_ERR("Error writing to file");
        fs_close(&httpfd);
        file_opened = false;
    }
    LOG_DBG("HTTP DL: %ld bytes written", http_amt_written);
    wifi_msg_free(msg);
}

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
int wifi_set_ap_profile_use(bool use_profile, k_timeout_t timeout)
{
    char cmd[20];
    // Note the our api and the DA flag are opposite
    sprintf(cmd, "AT+WFDIS=%d", use_profile == false);
    int ret = wifi_send_ok_err_atcmd(cmd, NULL, timeout);
    if (ret < 0) {
        LOG_ERR("'%s'(%d) Setting the AP profile use", wstrerr(-ret), ret);
    }
    return ret;
}

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
int wifi_get_wfstat(k_timeout_t timeout)
{
    int               ret;
    bool              got_ok     = false;
    bool              got_status = false;
    char              status[100];
    char              errtmp[20];
    wifi_wait_array_t wait_msgs;
    k_timepoint_t     timepoint = sys_timepoint_calc(timeout);

    if (da_state.powered_on == DA_STATE_KNOWN_FALSE) {
        LOG_ERR("%s: da is powered off", __func__);
        return -ENODEV;
    }

    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("%s: while da is sleeping", __func__);
        return -ENXIO;
    }

    if (wifi_get_mutex(timeout, __func__) != 0) {
        return -EBUSY;
    }

    wifi_flush_msgs();

    timeout = sys_timepoint_timeout(timepoint);
    ret     = wifi_send_timeout("AT+WFSTAT", timeout);
    if (ret != 0) {
        goto getwfstat_exit;
    }

    timeout            = sys_timepoint_timeout(timepoint);
    wait_msgs.num_msgs = 0;    // Initialize the structure
    wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
    wifi_add_wait_msg(&wait_msgs, "\r\nERROR%19s\r\n", true, 1, errtmp);
    wifi_add_wait_msg(&wait_msgs, "wpa_state=%99s", true, 1, status);
    while (1) {
        timeout = sys_timepoint_timeout(timepoint);
        ret     = wifi_wait_for(&wait_msgs, timeout);
        if (ret < 0) {
            goto getwfstat_exit;
        }
        if (ret == 0) {
            got_ok = true;
        }
        if (ret == 1) {
            LOG_DBG("Error response on wfstat: %20s", errtmp);
            ret = -EBADE;
            goto getwfstat_exit;
        }
        if (ret == 0) {
            got_status = true;
        }
        if (got_ok && got_status) {
            if (strstr(status, "DISCONNECTED") != NULL) {
                ret = 0;
            } else {
                ret = 1;
            }
            break;
        }
    }

getwfstat_exit:
    wifi_release_mutex();
    return ret;
}
