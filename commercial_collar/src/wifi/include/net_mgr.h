/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
/****************************************************************
 *  net_mgr.h
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
#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include "wifi_at.h"
#include "uicr.h"
#include "d1_json.h"

// The thing name is the serial number (12 chars) + underscore + messge type (2 chars) + null
#define MAX_THING_NAME_SIZE (20)
#define DA_UICR_BACKUP_SIZE (sizeof(uicr_storage_struct_t))
#define MAX_URL_LEN         1900

typedef enum
{
    DA_BU_UNKNOWN  = -1,
    DA_BU_NONE     = 0,
    DA_BU_EXISTS   = 1,
    DA_BU_MISMATCH = 2,
} das_bu_status_t;

typedef enum da_ota_progress
{
    DA_OTA_PROGRESS_NO_OTA          = -1,
    DA_OTA_CANT_GET_MUTEX           = -2,
    DA_OTA_ERR_STARTING_DL          = -3,
    DA_OTA_TIMEOUT_GETTING_PROGRESS = -4,
    DA_OTA_ERROR_GETTING_PROGRESS   = -5,
    DA_OTA_ERROR_STOPPING           = -6,
    DA_OTA_PROGRESS_STALLED         = -7,
    DA_OTA_ERROR_RENEWING           = -8,
    DA_OTA_ASYNC_START_ERROR        = -9,
    DA_OTA_VERSION_MISMATCH         = -10,
    DA_OTA_ERROR_REBOOTING          = -11,
    DA_OTA_ERROR_PARSING_PROGRESS   = -12,
    DA_OTA_DOWNLOAD_COMPLETE        = 100,
    DA_OTA_PROGRESS_REBOOTING       = 101
} da_ota_progress_t;

typedef uint8_t da_version_t[3];

// <brand>_<serial>,  brand up to 3 char, serial is 12
#define MQTT_CLIENT_ID_LEN   17
#define DHCP_CLIENT_NAME_LEN 32
#define LAST_CMD_LEN         40

typedef struct da_state
{
    uint64_t          rtc_wake_time;
    das_tri_state_t   initialized;
    das_tri_state_t   ap_connected;
    das_tri_state_t   ap_safe;
    char              ap_disconnect_reason[40];
    char              ap_name[33];
    char              ip_address[20];
    das_tri_state_t   dpm_mode;
    das_tri_state_t   is_sleeping;
    das_tri_state_t   mqtt_enabled;
    das_tri_state_t   mqtt_broker_connected;
    uint64_t          mqtt_last_msg_time;
    das_tri_state_t   mqtt_certs_installed;
    char              mqtt_client_id[MQTT_CLIENT_ID_LEN];
    int               mqtt_sub_topic_count;
    char              mqtt_sub_topics[CONFIG_IOT_MAX_TOPIC_NUM][CONFIG_IOT_MAX_TOPIC_LENGTH + 1];
    das_tri_state_t   ntp_server_set;
    das_tri_state_t   dhcp_client_name_set;
    char              dhcp_client_name[DHCP_CLIENT_NAME_LEN];
    char              last_cmd[LAST_CMD_LEN];
    das_bu_status_t   uicr_bu_status;
    uint8_t           uicr_bu[DA_UICR_BACKUP_SIZE];
    das_tri_state_t   mac_set;
    das_tri_state_t   xtal_set;
    das_tri_state_t   onboarded;
    das_tri_state_t   mqtt_on_boot;
    da_ota_progress_t ota_progress;
    int               reboot_cnt;
    da_version_t      version;
    das_tri_state_t   ap_profile_disabled;
    das_tri_state_t   powered_on;
    int               rssi;
} da_state_t;

typedef enum da_event_type
{
    DA_EVENT_TYPE_WIFI_INIT            = 1,
    DA_EVENT_TYPE_AP_CONNECT           = (1 << 1),
    DA_EVENT_TYPE_DPM_MODE             = (1 << 2),
    DA_EVENT_TYPE_IS_SLEEPING          = (1 << 3),
    DA_EVENT_TYPE_MQTT_BROKER_CONNECT  = (1 << 4),
    DA_EVENT_TYPE_MQTT_MSG_SENT        = (1 << 5),
    DA_EVENT_TYPE_MQTT_CERTS           = (1 << 6),
    DA_EVENT_TYPE_NTP_SERVER_SET       = (1 << 7),
    DA_EVENT_TYPE_DHCP_CLIENT_NAME_SET = (1 << 8),
    DA_EVENT_TYPE_MQTT_SUB_TOP_CHANGED = (1 << 9),
    DA_EVENT_TYPE_LAST_CMD             = (1 << 10),
    DA_EVENT_TYPE_UICR_BU_STATUS       = (1 << 11),
    DA_EVENT_TYPE_MAC_SET              = (1 << 12),
    DA_EVENT_TYPE_XTAL_SET             = (1 << 13),
    DA_EVENT_TYPE_ONBOARDED            = (1 << 14),
    DA_EVENT_TYPE_BOOT_MQTT_STATE      = (1 << 15),
    DA_EVENT_TYPE_DA_RESTARTED         = (1 << 16),
    DA_EVENT_TYPE_HTTP_COMPLETE        = (1 << 17),
    DA_EVENT_TYPE_AP_SAFE              = (1 << 18),
    DA_EVENT_TYPE_OTA_PROGRESS         = (1 << 19),
    DA_EVENT_TYPE_REBOOT_CNT           = (1 << 20),
    DA_EVENT_TYPE_VERSION              = (1 << 21),
    DA_EVENT_TYPE_RTC_WAKE_TIME        = (1 << 22),
    DA_EVENT_TYPE_MQTT_ENABLED         = (1 << 23),
    DA_EVENT_TYPE_AP_PROFILE_USE       = (1 << 24),
    DA_EVENT_TYPE_POWERED_ON           = (1 << 25),
    DA_EVENT_TYPE_DISCONNECT_REASON    = (1 << 26),
    DA_EVENT_TYPE_RSSI                 = (1 << 27)
} da_event_type_t;

typedef union da_event_data_union
{
    das_tri_state_t   tri;
    uint64_t          timestamp;
    da_version_t      ver;
    das_bu_status_t   uicr_bu_status;
    int               theInt;
    int8_t            reboot_cnt;
    da_ota_progress_t ota_progress;
} da_event_data_union_t;

typedef struct da_event
{
    uint64_t              timestamp;
    uint32_t              events;
    da_event_data_union_t old;
    da_event_data_union_t new;
} da_event_t;

typedef struct da_event_watch_item
{
    struct k_work work;
    da_event_t    evt;
} sa_event_watch_item_t;

extern da_state_t                da_state;
extern const struct zbus_channel da_state_chan;
extern int                       httpresultcode;
extern long                      http_amt_written;
extern wifi_saved_ap_t           g_last_conn_attempt;
extern char                      g_last_ap_name[33];

#define DA_USER_NVRAM_BASE  (0x003AD000)
#define DA_UICR_BACKUP_FLAG (DA_USER_NVRAM_BASE)
#define DA_UICR_BACKUP_ADDR (DA_USER_NVRAM_BASE + 4)

#define DA_NV_NET_STATE_ADDR (DA_USER_NVRAM_BASE + 300)
#define DA_NV_ONBOARDED_ADDR (DA_NV_NET_STATE_ADDR + 0)

const char *tristate_str(das_tri_state_t val);

//////////////////////////////////////////////////////////////////
// net_mgr_init()
//
// Initialize the wifi application layer
//
// @return - 0 on success, -1 on error
int net_mgr_init();

//////////////////////////////////////////////////////////
// net_set_saved_bool()
//
// Set a bool into the DA NVRAM
//
// @param addr - the address in the DA NVRAM
// @param stored - a pointer to the current value
// @param event_flag - the event flag to set if the value changes
// @param val - the value to set
//
// @return - 0 on success <0 on error
//////////////////////////////////////////////////////////
int net_set_saved_bool(uint32_t addr, das_tri_state_t *localvar, uint32_t event_flag, uint8_t val);

//////////////////////////////////////////////////////////
// net_start_ota()
//	Start an OTA download and install
//
// @param url - the url to download the OTA from
// @return - 0 on success, -1 on error
int net_start_ota(char *url, uint8_t expected_version[3]);

//////////////////////////////////////////////////////////
// net_stop_ota()
//	Stop an OTA download
//
// @return - 0 on success, -1 on error
int net_stop_ota();

////////////////////////////////////////////////////////////
// net_ota_progress_str()
//
// Return a string that describes the OTA progress
//
// @param progress - the progress to describe
//
// @return - a string that describes the progress
//
const char *net_ota_progress_str(da_ota_progress_t progress);

////////////////////////////////////////////////////////////
// net_stage_ssid_for_saving()
//
// Stage a ssid for saving later when confirmed connected
//
// @param ssid - the ssid name
// @param pass - the password
// @param sec - the security type
// @param keyidx - the key index
// @param enc - the encryption type
// @param hidden - true if the ssid is hidden
//
void net_stage_ssid_for_saving(char *ssid, char *pass, uint16_t sec, uint16_t keyidx, uint16_t enc, bool hidden);

////////////////////////////////////////////////////////////
// net_unstage_ssid_for_saving()
//
// Unstage a ssid for saving later when confirmed connected
//
//
void net_unstage_ssid_for_saving();

//////////////////////////////////////////////////////////
// write_uicr_backup()
//
// Write the UICR backup to the DA NVRAM
//
// @return - 0 on success, -1 on error
int write_uicr_backup();

void net_mgr_wifi_power(bool powered);

void send_zbus_int_event(uint32_t event, int new, int *var, bool force);
void send_zbus_tri_event(uint32_t event, das_tri_state_t new, das_tri_state_t *var);
void send_zbus_timestamp_event(uint32_t event, uint64_t new, uint64_t *var);

//////////////////////////////////////////////////////////
// Public facing DA16200 state
//
// Non-wifi modules can use these accesor functions to
// get state of various attibutes of the DA16200.  Note
// That the tri_state can be unknown for some since until
// we can ask the DA, we can't know of sure.
//
// WHen any of these values change, a ZBUSS message is sent
// to subscribers of the "da_state_chan" channel

inline bool da_state_get_initialized()
{
    return da_state.initialized;
}
inline das_tri_state_t da_state_get_ap_connected()
{
    return da_state.ap_connected;
}
inline das_tri_state_t da_state_get_ap_safe()
{
    return da_state.ap_safe;
}
inline const char *da_state_get_ap_name()
{
    return da_state.ap_name;
}
inline const char *da_state_get_ip_address()
{
    return da_state.ip_address;
}
inline das_tri_state_t da_state_get_dpm_mode()
{
    return da_state.dpm_mode;
}
inline das_tri_state_t da_state_get_is_sleeping()
{
    return da_state.is_sleeping;
}
inline das_tri_state_t da_state_get_mqtt_broker_connected()
{
    return da_state.mqtt_broker_connected;
}
inline uint64_t da_state_get_mqtt_last_msg_time()
{
    return da_state.mqtt_last_msg_time;
}
inline das_tri_state_t da_state_get_mqtt_certs_installed()
{
    return da_state.mqtt_certs_installed;
}
inline const char *da_state_get_mqtt_client_id()
{
    return da_state.mqtt_client_id;
}
inline int da_state_get_mqtt_sub_topic_count()
{
    return da_state.mqtt_sub_topic_count;
}
inline const char *da_state_get_mqtt_sub_topic(int i)
{
    return da_state.mqtt_sub_topics[i];
}
inline das_tri_state_t da_state_get_ntp_server_set()
{
    return da_state.ntp_server_set;
}
inline das_tri_state_t da_state_get_dhcp_client_name_set()
{
    return da_state.dhcp_client_name_set;
}
inline const char *da_state_get_dhcp_client_name()
{
    return da_state.dhcp_client_name;
}
inline const char *da_state_get_last_cmd()
{
    return da_state.last_cmd;
}
inline das_bu_status_t da_state_get_uicr_bu_status()
{
    return da_state.uicr_bu_status;
}
inline const uint8_t *da_state_get_uicr_bu()
{
    return da_state.uicr_bu;
}
inline das_tri_state_t da_state_get_mac_set()
{
    return da_state.mac_set;
}
inline das_tri_state_t da_state_get_xtal_set()
{
    return da_state.xtal_set;
}
inline das_tri_state_t da_state_get_onboarded()
{
    return da_state.onboarded;
}
inline das_tri_state_t da_state_get_mqtt_on_boot()
{
    return da_state.mqtt_on_boot;
}
inline int8_t da_state_get_ota_progress()
{
    return da_state.ota_progress;
}
inline uint8_t da_state_get_reboot_cnt()
{
    return da_state.reboot_cnt;
}
inline da_version_t *da_state_get_version()
{
    return &(da_state.version);
}
