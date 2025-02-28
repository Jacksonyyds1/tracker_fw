/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */

#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stddef.h>
#include <zephyr/kernel.h>
#include <cJSON.h>
#include "pmic.h"

#define MAX_WIFI_OBJS   32
#define NUM_ZONES       5
#define MAX_SAVED_SSIDS 5

typedef enum
{
    MQTT_MESSAGE_TYPE_UNKNOWN             = -1,
    MQTT_MESSAGE_TYPE_INFO_TELEMETRY      = 1,
    MQTT_MESSAGE_TYPE_ONBOARDING          = 3,
    MQTT_MESSAGE_TYPE_FOTA                = 4,
    MQTT_MESSAGE_TYPE_REMOTE_FUNCTION     = 5,
    MQTT_MESSAGE_TYPE_CONN_TEST           = 10,
    MQTT_MESSAGE_TYPE_REVERSE_REMOTE_FUNC = 11,
    MQTT_MESSAGE_TYPE_DEBUG               = 12,
    MQTT_MESSAGE_TYPE_ACTIONABLE          = 13,
    MQTT_MESSAGE_TYPE_SHADOW_PROXY        = 15,
    MQTT_MESSAGE_TYPE_FOTA_LIFE           = 16,
    MQTT_MESSAGE_TYPE_MQTT_TO_HTTP        = 18,
    MQTT_MESSAGE_TYPE_SRF_NONCE           = 22,
    MQTT_MESSAGE_TYPE_SRF_FUNC            = 23,
    MQTT_MESSAGE_TYPE_CONFIG_HUB          = 25,
    MQTT_MESSAGE_TYPE_NEAR_REAL_TIME      = 29
} mqtt_message_type_t;

#define TELEMETRY_PROTOCOL_VERSION      (5)
#define ONBOARDING_PROTOCOL_VERSION     (5)
#define CONNECTIVITY_PROTOCOL_VERSION   (2)
#define REALTIME_PROTOCOL_VERSION       (1)
#define SHADOW_PROTOCOL_VERSION         (1)
#define FOTA_LIFECYCLE_PROTOCOL_VERSION (2)

#define PAIRING_SUB_TYPE       (2)
#define SAFE_ZONE_SUB_TYPE     (12)
#define WHERES_MY_DOG_SUB_TYPE (22)
#define TEMPERATURE_SUB_TYPE   (32)

typedef struct
{
    float  lon;
    float  lat;
    float  alt;
    int8_t acc;
} json_gps_obj_t;

typedef struct
{
    char   ssid[32];
    char   macstr[20];
    float  rssi;
    char   flags[100];
    int8_t channel;
} wifi_obj_t;

typedef struct
{
    wifi_obj_t wifi[MAX_WIFI_OBJS];
    size_t     count;
} wifi_arr_t;

typedef struct
{
    uint8_t idx;
    char    ssid[33];
    bool    safe;
} shadow_zone_t;

// These parameters are examples used to prove out the shadow mechanism
typedef struct
{
    uint16_t S_Norm;    // CONFIG_IOT_S_NORM_DEFAULT,
    uint16_t S_FMD;     // CONFIG_IOT_S_FMD_DEFAULT,
    uint16_t T_Norm;    // CONFIG_IOT_T_NORM_DEFAULT,
    uint16_t T_FMD;     // CONFIG_IOT_T_FMD_DEFAULT,
    uint16_t Rec;       // CONFIG_IOT_REC_VAR_DEFAULT,
    uint16_t Q;         // CONFIG_IOT_Q_VAR_DEFAULT,
    uint16_t dur;
    uint16_t ths;
    bool     mot_det;
    uint8_t  mcuVer[3];
    uint8_t  wifiVer[3];
    uint8_t  lteVer[3];
    uint32_t fota_in_progress_duration;
    // def: 10 The seconds after boot to check if the 5340 just updated

    uint32_t gps_poll_period;
    // def: 0 How often to get gps updates, 0 is off

    shadow_zone_t zones[NUM_ZONES];
} shadow_doc_t;

typedef struct
{
    char  *keys[20];
    char  *values[20];
    size_t num;
} shadow_extra_t;

typedef struct
{
    uint8_t  idx;
    char     ssid[33];
    char     password[65];
    uint16_t sec;
    uint16_t keyidx;
    uint16_t enc;
    bool     hidden;
    bool     is_safe;
    uint64_t last_time_accessed;
} wifi_saved_ap_t;

typedef enum
{
    SRF_COMMAND_UNKNOWN         = 0,
    SRF_COMMAND_WHERE_IS_MY_DOG = 1,
    SRF_COMMAND_FIND_MY_DOG     = 2,
    SRF_COMMAND_REBOOT          = 3,
    SRF_COMMAND_GPS_ENABLE      = 4,
    SRF_COMMAND_CHECK_FOTA      = 5,
    SRF_COMMAND_FACTORY_RESET   = 6,
    SRF_COMMAND_NOOP            = 7,
    // add new commands above SRF_COMMAND_COUNT
    SRF_COMMAND_COUNT = 8
} srf_command_t;

static char *const srf_command_strings[] = { [SRF_COMMAND_UNKNOWN]         = "unknown",
                                             [SRF_COMMAND_WHERE_IS_MY_DOG] = "wimd",
                                             [SRF_COMMAND_FIND_MY_DOG]     = "fmd",
                                             [SRF_COMMAND_REBOOT]          = "reboot",
                                             [SRF_COMMAND_GPS_ENABLE]      = "gps_enable",
                                             [SRF_COMMAND_CHECK_FOTA]      = "check_fota",
                                             [SRF_COMMAND_FACTORY_RESET]   = "factory_reset",
                                             [SRF_COMMAND_NOOP]            = "noop",
                                             [SRF_COMMAND_COUNT]           = "count" };

typedef struct
{
    srf_command_t command;
    char          rid[48];
    char          sub[64];
    uint64_t      exp;
    char          iss[64];
    uint64_t      nonce;
    char          aud[64];
    uint64_t      iat;
    char          param1[64];
    char          param2[64];
    char          param3[64];
    char          param4[64];
} srf_data_t;

typedef enum
{
    RADIO_TYPE_WIFI = 0,
    RADIO_TYPE_BLE  = 1,
    RADIO_TYPE_LTE  = 2,
} radio_t;
///////////////////////////////////////////////////
// radio_t_name()
// Get the name of the radio type
// radio: radio type
// returns: name of the radio type
///////////////////////////////////////////////////
static inline const char *
radio_t_name(radio_t radio)
{
    switch (radio) {
    case RADIO_TYPE_WIFI:
        return "WIFI";
    case RADIO_TYPE_BLE:
        return "BLE";
    case RADIO_TYPE_LTE:
        return "LTE";
    default:
        return "UNKNOWN";
    }
}

static inline const char *
msg_name(mqtt_message_type_t msgtype)
{
    switch (msgtype) {
    case MQTT_MESSAGE_TYPE_UNKNOWN:
        return "UNKNOWN";
    case MQTT_MESSAGE_TYPE_INFO_TELEMETRY:
        return "TELEMETRY";
    case MQTT_MESSAGE_TYPE_ONBOARDING:
        return "ONBOARDING";
    case MQTT_MESSAGE_TYPE_FOTA:
        return "FOTA";
    case MQTT_MESSAGE_TYPE_REMOTE_FUNCTION:
        return "REMOTE FUNCTION";
    case MQTT_MESSAGE_TYPE_CONN_TEST:
        return "CONN_TEST";
    case MQTT_MESSAGE_TYPE_REVERSE_REMOTE_FUNC:
        return "RRF";
    case MQTT_MESSAGE_TYPE_DEBUG:
        return "DEBUG";
    case MQTT_MESSAGE_TYPE_ACTIONABLE:
        return "ACTIONABLE";
    case MQTT_MESSAGE_TYPE_SHADOW_PROXY:
        return "SHADOW";
    case MQTT_MESSAGE_TYPE_FOTA_LIFE:
        return "FOTA_LIFE";
    case MQTT_MESSAGE_TYPE_MQTT_TO_HTTP:
        return "MQTT_TO_HTTP";
    case MQTT_MESSAGE_TYPE_SRF_NONCE:
        return "SRF_NONCE";
    case MQTT_MESSAGE_TYPE_SRF_FUNC:
        return "SRF_FUNC";
    case MQTT_MESSAGE_TYPE_CONFIG_HUB:
        return "CONFIG_HUB";
    case MQTT_MESSAGE_TYPE_NEAR_REAL_TIME:
        return "NEAR_REAL_TIME";
    default:
        return "UNKNOWN";
    }
}

typedef enum
{
    COMM_DEVICE_NONE    = 0,
    COMM_DEVICE_NRF9160 = 1,
    COMM_DEVICE_DA16200 = 2,
    COMM_DEVICE_NRF5340 = 4,
    COMM_DEVICE_ALL     = 8,
    COMM_DEVICE_ANY     = 16
} comm_device_type_t;

///////////////////////////////////////////////////
// comm_dev_str()
// Get the name of the comm device type
// device: comm device type
// returns: name of the comm device type
const char *comm_dev_str(comm_device_type_t device);

// The machine id shouldn't be more than 12 characters
#define MAX_MID_SIZE     (12)
#define REQUEST_ID_SIZE  (48)
#define DEPLOY_TYPE_SIZE (8)

///////////////////////////////////////////////////
// json_telemetry()
// Create a JSON message for the telemetry
//
// @param MID: Machine ID
// @param data: wifi ssid info
// @param batt_soc: state of charge
// @param charging : if we are charging
// @param gps : gps data
// @param radio_used: radio used
// @param da_ver: version of the DA
// @param ap_name: name of the access point
// @param ap_is_safe: is the access point safe
//
// @return: char * to json or null on error
///////////////////////////////////////////////////
int json_telemetry(
    char            **replyJson,
    char             *MID,
    fuel_gauge_info_t batt_info,
    bool              charging,
    radio_t           radio_used,
    char             *da_ver,
    char             *ap_name,
    bool              ap_is_safe,
    bool              usb_connected,
    struct k_fifo    *wifi_ssids_fifo,
    int               chunk_num,
    int               rssi);

///////////////////////////////////////////////////
// json_onboarding()
// Create a JSON message for the onboarding msg
// MID: Machine ID
// gwid: unknown string
// market: 2 character market id
// returns: char * to json or null on error
///////////////////////////////////////////////////
char *json_onboarding(char *MID, char *gwid);

///////////////////////////////////////////////////
// json_parse_mqtt_string()
// Parse a JSON string into a cJSON object
// str: JSON string
// returns: cJSON object or NULL on error  - MUST BE FREE'd BY CALLER  (cJSON_Delete(json))
///////////////////////////////////////////////////
cJSON *json_parse_mqtt_string(char *str);

///////////////////////////////////////////////////
// json_get_mqtt_type()
// Get the message type from a cJSON object
// obj: cJSON object
// returns: message type or -1 on error
///////////////////////////////////////////////////
mqtt_message_type_t json_get_mqtt_type(cJSON *obj);

///////////////////////////////////////////////////
// json_shadow_report()
// Create a JSON message for the shadow request msg
// MID: Machine ID
// gwid: unknown string
// market: 2 character market id
// returns: char * to json or null on error
///////////////////////////////////////////////////
char *json_shadow_report(char *MID, shadow_doc_t *doc, shadow_extra_t *extra);

///////////////////////////////////////////////////
// json_parse_shadow_report()
// extract a shadow_doc_t from a JSON message. Used
// to read the shadow save file
//
// @param str: JSON string
// @param newdoc: shadow_doc_t to fill
// @param olddoc: current shadow_doc_t (used as defaults)
//
// returns: 0 on success, < 0 on error
///////////////////////////////////////////////////
int json_parse_shadow_report(char *json, shadow_doc_t *newdoc, shadow_doc_t *olddoc);

///////////////////////////////////////////////////
// json_connectivity_report()
// Create a JSON message for the connectivity test
// which can be variable in size
///////////////////////////////////////////////////
char *json_connectivity_msg(char *MID, int msg_size);

///////////////////////////////////////////////////
// json_fota_check()
// Create a JSON message for the fota request msg
// MID: Machine ID
// version_major:
// version_minor:
// version_patch:
// returns: char * to json or null on error
///////////////////////////////////////////////////
char *json_fota_check(
    char              *MID,
    uint16_t           version_major,
    uint16_t           version_minor,
    uint16_t           version_patch,
    char              *nonce,
    comm_device_type_t device_type);

///////////////////////////////////////////////////
// json_near_realtime_msg()
// json_connectivity_report()
// Create a JSON message for the connectivity test
// which can be variable in size
///////////////////////////////////////////////////
char *json_near_realtime_msg(char *MID, int sub_type, char *payload);

///////////////////////////////////////////////////
// json_pair_msg()
///////////////////////////////////////////////////
char *json_pair_msg(char *MID, char *nonce, char *iccd, char *imei, char *ble_mac, char *model);

///////////////////////////////////////////////////
// json_fota_validation_msg()
///////////////////////////////////////////////////
char *json_fota_feedback_msg(char *MID, int errorCode, char *deployType, char *requestID, int64_t timeStamp, char *state);

///////////////////////////////////////////////////
// json_wheresmydog()
// Create a JSON message for the where is my dog
// MID: Machine ID
// data: wifi ssid info
// cellid: cell id
// tracking_area: tracking area
// dog_ssid: ssid of the dog
// dog_in_safe_zone: is the dog in the safe zone
// request_id: request id
// returns: char * to json or null on error
// If any of the parameters are NULL then the
// field is not included in the JSON
///////////////////////////////////////////////////
int json_wheresmydog(
    char         **returnJSON,
    char          *MID,
    int           *cellid,
    int           *tracking_area,
    char          *dog_ssid,
    bool          *dog_in_safe_zone,
    char          *request_id,
    bool           send_gps,
    struct k_fifo *wifi_ssids_fifo,
    int            chunk_num);

///////////////////////////////////////////////////
// json_safe_zone_alert()
// MID: Machine ID
// type: alert type
// ssid: ssid where we left or entered
// enter: true if we entered the safe zone, false if we left
///////////////////////////////////////////////////
char *json_safe_zone_alert(char *MID, uint64_t time, int type, char *ssid, bool enter, char *reason);

uint64_t get_unix_time();

const char *check_reset_reason(void);

char *json_srf_nonce(char *MID, int nonce);

int   json_parse_srf(char *str, srf_data_t *data);
char *json_srf_response_message(char *rid, char *st, char *fres, uint64_t currTime);
int   ml_get_json_list(cJSON *jsonObj, int16_t *remaining_space);
