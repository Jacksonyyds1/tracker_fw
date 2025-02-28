/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#include "d1_json.h"
#include <cJSON_os.h>
#include "app_version.h"
#include "hal/nrf_power.h"
#include "app_version.h"
#include <zephyr/sys/base64.h>

#include <zephyr/sys/timeutil.h>
#include <zephyr/posix/time.h>
#include "modem_interface_types.h"
#include "modem.h"
#include "radioMgr.h"
#include <zephyr/logging/log.h>
#include "fqueue.h"
#include "imu.h"
#include "ml.h"
#include "ml_types.h"
#include "ml_decode.h"
#include "utils.h"
LOG_MODULE_REGISTER(d1_json, CONFIG_D1_JSON_LOG_LEVEL);
#if !CONFIG_BUILDING_MFG_SHELL
#include "wifi_at.h"
#include "log_telemetry.h"
#endif
#if !NRF_POWER_HAS_RESETREAS
#include <hal/nrf_reset.h>
#endif
#include <math.h>
#include "gps.h"

#define D1_JSON_MESSAGE_BUFFER_SIZE 6000
#define XTR_BUFF_SIZE               1024
#define MAX_RF_OBJS                 8
#define MAX_NUM_EV_OBJS             1

#define EV_MOD "ModelA\0"
#define EV_HWR "0.1.0\0"
#define EV_FWR "0.8.1\0"
#define EV_TZ  "Europe/Rome\0"

#define ML_MINIMUM_ELEMENTS (8)

#define json_schema_ver "1.4.0"

// define C structs for every part of the final JSON message
// define C structs that combine those structs/arrays for each level of the JSON message.

// define zephyr-JSON-macro structs for each of the structs defined above.

char            json_message_buffer[D1_JSON_MESSAGE_BUFFER_SIZE];
char           *json_message_buffer2;
uint32_t        telemetry_count     = 0;
uint32_t        fmd_telemetry_count = 0;
extern bool     usb_connected;
extern uint64_t g_last_ssid_scan_time;
#define MAX_SSIDS_TO_INCLUDE 15
uint64_t get_unix_time()
{
    return utils_get_utc();
}

static char reset_reason_string[64];
bool        rest_reason_found_once = false;
const char *check_reset_reason(void)
{
    uint32_t reas;
    bool     reason_found = false;

    if (rest_reason_found_once) {
        return NULL;
    }
#if CONFIG_NRF_POWER_HAS_RESETREAS

    reas = nrf_power_resetreas_get(NRF_POWER);
    nrf_power_resetreas_clear(NRF_POWER, reas);
    if (reas & NRF_POWER_RESETREAS_NFC_MASK) {
        strncpy(reset_reason_string, "Wake up by NFC field detect", sizeof(reset_reason_string));
        reason_found = true;
    } else if (reas & NRF_POWER_RESETREAS_RESETPIN_MASK) {
        strncpy(reset_reason_string, "Reset by pin-reset", sizeof(reset_reason_string));
        reason_found = true;
    } else if (reas & NRF_POWER_RESETREAS_SREQ_MASK) {
        strncpy(reset_reason_string, "Reset by soft-reset", sizeof(reset_reason_string));
        reason_found = true;
    } else if (reas & NRF_RESET_RESETREAS_VBUS_MASK) {
        strncpy(reset_reason_string, "Reset by VBUS", sizeof(reset_reason_string));
        reason_found = true;
    } else if (reas & NRF_RESET_RESETREAS_DOG0_MASK) {
        strncpy(reset_reason_string, "Reset by internal watchdog0", sizeof(reset_reason_string));
        reason_found = true;
    } else if (reas & NRF_RESET_RESETREAS_DOG1_MASK) {
        strncpy(reset_reason_string, "Reset by internal watchdog1", sizeof(reset_reason_string));
        reason_found = true;
    } else if (reas) {
        snprintk(reset_reason_string, sizeof(reset_reason_string), "Reset by a different source %x", reas);
        reason_found = true;
    }
    nrf_power_resetreas_clear(NRF_POWER, reas);
#else

    reas = nrf_reset_resetreas_get(NRF_RESET);
    if (reas & NRF_RESET_RESETREAS_NFC_MASK) {
        strncpy(reset_reason_string, "Wake up by NFC field detect", sizeof(reset_reason_string));
        reason_found = true;
    } else if (reas & NRF_RESET_RESETREAS_RESETPIN_MASK) {
        strncpy(reset_reason_string, "Reset by pin-reset", sizeof(reset_reason_string));
        reason_found = true;
    } else if (reas & NRF_RESET_RESETREAS_SREQ_MASK) {
        strncpy(reset_reason_string, "Reset by soft-reset", sizeof(reset_reason_string));
        reason_found = true;
    } else if (reas & NRF_RESET_RESETREAS_VBUS_MASK) {
        strncpy(reset_reason_string, "Reset by VBUS", sizeof(reset_reason_string));
        reason_found = true;
    } else if (reas & NRF_RESET_RESETREAS_DOG0_MASK) {
        strncpy(reset_reason_string, "Reset by internal watchdog0", sizeof(reset_reason_string));
        reason_found = true;
    } else if (reas & NRF_RESET_RESETREAS_DOG1_MASK) {
        strncpy(reset_reason_string, "Reset by internal watchdog1", sizeof(reset_reason_string));
        reason_found = true;
    } else if (reas) {
        snprintk(reset_reason_string, sizeof(reset_reason_string), "Reset by a different source %x", reas);
        reason_found = true;
    }
    nrf_reset_resetreas_clear(NRF_RESET, reas);
#endif

    if (!rest_reason_found_once && !reason_found && reas != 0) {
        snprintk(reset_reason_string, 64, "Reset reason unknown: %x", reas);
        reason_found = true;
    }
    rest_reason_found_once = true;
    if (!reason_found) {
        return pmic_get_reset_reason();
    }
    return reset_reason_string;
}

double roundf_val(float input, int decimal)
{
    int _pow = 1;
    for (int i = 1; i < decimal; i++) {
        _pow = _pow * 10;
    }

    double value;
    if (input >= 0) {
        value = (int)(input * _pow + 0.5f);
    } else {
        value = (int)(input * _pow - 0.5f);
    }

    return value / _pow;
}

int add_ssids_to_json(cJSON *parentObj, struct k_fifo *wifi_ssids_fifo, int16_t *remaining_space)
{
    if (!wifi_ssids_fifo || k_fifo_is_empty(wifi_ssids_fifo)) {
        return 0;
    }

    if (*remaining_space < 128) /* Hand-wavey estimate of minimum packet size */ {
        return -ENOMEM;
    }

    cJSON *wifiArray = cJSON_AddArrayToObject(parentObj, "SSIDS");
    *remaining_space -= sizeof("SSIDS") + 5;    // for 2x" 2x[ and 1x:
    char tempSpace[256];
    while (!k_fifo_is_empty(wifi_ssids_fifo)) {
        wifi_obj_t *data     = k_fifo_peek_head(wifi_ssids_fifo);
        cJSON      *newEntry = cJSON_CreateObject();
        bool        all_ok   = true;
        all_ok &= (bool)cJSON_AddStringToObject(newEntry, "macAddress", data->macstr);
        all_ok &= (bool)cJSON_AddNumberToObject(newEntry, "signalStrength", data->rssi);
        all_ok &= (bool)cJSON_AddNumberToObject(newEntry, "channel", data->channel);
        cJSON_PrintPreallocated(newEntry, tempSpace, 256, false);
        LOG_DBG("New entry size: %d    remaing_space: %d", strlen(tempSpace), *remaining_space);
        if (!all_ok || (*remaining_space < strlen(tempSpace))) {
            cJSON_Delete(newEntry);
            return 1;    // we're out of space and need to continue on the next loop
        }
        data = k_fifo_get(wifi_ssids_fifo, K_NO_WAIT);    // pop the message off the fifo
        *remaining_space -= strlen(tempSpace);
        cJSON_AddItemToArray(wifiArray, newEntry);
    }
    return 0;
}

///////////////////////////////////////////////////
// json_telemetry()
// Create a JSON message for the telemetry
//
// @param replyJson: pointer to the json message
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
// @return: int: 0 on success, <0 on error, >0 if more data to send
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
    int               rssi)
{
    int finalReturn = 0;
    int midlen      = strlen(MID);
    if (midlen == 0 || midlen > MAX_MID_SIZE) {
        return -EINVAL;
    }
    struct timespec tp;
    clock_gettime(CLOCK_REALTIME, &tp);
    cJSON_Init();

    // change to loop until all data sub-objects report 0 for success or < 0 for error. positive
    // returns are index/counts that will need to be passed back into the same function on the
    // next loop to get the next batch of data. calls to each sub-object will contain this index
    // (or zero at teh start) AND a length_remaining which tells the function how much space is
    // left in the message buffer. This means the order of calling the sub-objects determines
    // the order of the data in the final message(s).

    // Run the loop until a message has little/no space left, and queue the message for sending.

    // return codes for sub-objects:
    // 0: success, no more data to send
    // > 0: success, more data to send, pass this value back into the function to get the next
    // batch of data
    //          this can still NOT append any new values to the message if it determines there
    //          is not enough space left for even 1 item. for this reason the index returned is
    //          1 based, not 0 based. as 0 is an error code. (not important, just pass it back
    //          in)
    // < 0: error, stop processing and return NULL

    // cJSON *wifiObjs[MAX_WIFI_OBJS];

    cJSON *topLevel = cJSON_CreateObject();
    cJSON_AddNumberToObject(topLevel, "P", TELEMETRY_PROTOCOL_VERSION);
    cJSON_AddStringToObject(topLevel, "MID", MID);
    cJSON_AddStringToObject(topLevel, "MK", CONFIG_IOT_MQTT_REGION_ID);
    cJSON_AddNumberToObject(topLevel, "T", MQTT_MESSAGE_TYPE_INFO_TELEMETRY);
    cJSON_AddNumberToObject(topLevel, "B", CONFIG_IOT_MQTT_BRAND_ID);
    cJSON *mObject   = cJSON_AddObjectToObject(topLevel, "M");
    cJSON *dviObject = cJSON_AddObjectToObject(mObject, "DVI");
    cJSON_AddStringToObject(dviObject, "MOD", "DogCollar");
    cJSON_AddStringToObject(dviObject, "HWR", "0.1.0");
    char fw_version[32];
    snprintf(fw_version, 32, "%d.%d.%d", APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_VERSION_PATCH);
    cJSON_AddStringToObject(dviObject, "FWR", fw_version);
    cJSON_AddStringToObject(dviObject, "TZ", "US/Pacific");
    cJSON *xtrObject = cJSON_AddObjectToObject(dviObject, "XTR");
    cJSON_AddStringToObject(xtrObject, "JSON_VER", json_schema_ver);
    cJSON_AddNumberToObject(xtrObject, "SUB", 4);
    cJSON_AddNumberToObject(xtrObject, "CRON", get_unix_time());

    // POWER
    cJSON *pwrObject = cJSON_AddObjectToObject(xtrObject, "POWER");
    cJSON_AddNumberToObject(pwrObject, "BATT_S", roundf_val(batt_info.soc, 1));
    cJSON_AddNumberToObject(pwrObject, "BATT_V", roundf_val(batt_info.voltage, 2));
    cJSON_AddNumberToObject(pwrObject, "BATT_T", roundf_val(batt_info.temp, 2));
    cJSON_AddBoolToObject(pwrObject, "CHARGING", charging);
    cJSON_AddBoolToObject(pwrObject, "PLUG", usb_connected);

    // VERSIONS
    const char *rr = check_reset_reason();
    if (rr != NULL) {
        char   ver_str[32];
        cJSON *verObject = cJSON_AddObjectToObject(xtrObject, "VER");
        cJSON_AddStringToObject(pwrObject, "R_REASON", rr);
        snprintf(ver_str, 32, "%d.%d.%d", APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_VERSION_PATCH);
        cJSON_AddStringToObject(verObject, "5340_VER", ver_str);
#if !defined(CONFIG_BUILDING_MFG_SHELL)
        version_response_t *lte_ver = get_cached_version_info();
        snprintf(ver_str, 32, "%d.%d.%d", lte_ver->major, lte_ver->minor, lte_ver->patch);
#endif
        cJSON_AddStringToObject(verObject, "9160_VER", ver_str);
        if (da_ver != NULL) {
            snprintf(ver_str, 32, "%d.%d.%d", da_ver[0], da_ver[1], da_ver[2]);
            cJSON_AddStringToObject(verObject, "WIFI_VER", ver_str);
        }
    }

    // DEBUG
    cJSON *dbgObject = cJSON_AddObjectToObject(xtrObject, "DEBUG");
    if (chunk_num == 0) {
        int movement = imu_get_trigger_count();
        cJSON_AddNumberToObject(dbgObject, "MOVEMENT", movement);
        cJSON_AddNumberToObject(dbgObject, "UPTIME", k_uptime_get());
    }
    cJSON_AddNumberToObject(dbgObject, "MSG_NUM", telemetry_count);
    telemetry_count++;
    cJSON_AddNumberToObject(dbgObject, "CHUNK", chunk_num);

    // RADIO
    switch (radio_used) {
    case RADIO_TYPE_WIFI:
    {
        if (ap_name == NULL || strlen(ap_name) == 0) {
            LOG_ERR("WIFI_TYPE is WIFI, but no ap/aps - Bail!");
            finalReturn = -EINVAL;
            goto cleanup_and_exit;

        } else {
            cJSON_AddStringToObject(xtrObject, "RADIO", "WIFI");
            cJSON_AddStringToObject(xtrObject, "AP", ap_name);
            cJSON_AddBoolToObject(xtrObject, "APS", ap_is_safe);
        }
        if (rssi != RSSI_NOT_CONNECTED) {
            cJSON_AddNumberToObject(xtrObject, "RSSI", rssi);
        }
        break;
    };
    case RADIO_TYPE_LTE:
        if (modem_is_powered_on()) {
            cJSON_AddStringToObject(xtrObject, "RADIO", "LTE");
#if !defined(CONFIG_BUILDING_MFG_SHELL)
            cJSON_AddNumberToObject(xtrObject, "RSSI", modem_get_rssi());
#endif
        } else {
            cJSON_AddStringToObject(xtrObject, "RADIO", "UNKNOWN");
        }
        break;
    case RADIO_TYPE_BLE:
        cJSON_AddStringToObject(xtrObject, "RADIO", "BLE");
        break;
    default:
        cJSON_AddStringToObject(xtrObject, "RADIO", "UNKNOWN");
        break;
    }

    // thats all the required data, now we can add optional data
    // first see how much space we have left.
    // we need to leave space for the closing brackets and commas
    cJSON_PrintPreallocated(topLevel, json_message_buffer, D1_JSON_MESSAGE_BUFFER_SIZE, false);
    int     A_LITTE_PADDING_TO_BE_SAFE = 10;
    int16_t remaining_json_size = (CONFIG_MAX_MQTT_MSG_SIZE - A_LITTE_PADDING_TO_BE_SAFE) - strlen(json_message_buffer);

    // PUT THE FOLLOWING CALLS IN PRIORITY ORDER, SO THE MOST IMPORTANT DATA IS SENT IN THE
    // FIRST MESSAGE
    // TODO: handle the <0 return from each of these functions
    // LOG_WRN("TRY SSIDS");
    // SSIDS
    if (add_ssids_to_json(xtrObject, wifi_ssids_fifo, &remaining_json_size) > 0) {
        LOG_WRN("SSID data did not fit in the message");
        finalReturn = 1;    // indicate we have more to process
    }

#if defined(CONFIG_ML_ENABLE)
    LOG_DBG("TRY ML");
    // ML
    if (ml_get_json_list(xtrObject, &remaining_json_size) > 0) {
        LOG_WRN("ML data did not fit in the message");
        finalReturn = 1;    // indicate we have more to process
    }
#endif

    // LOG_WRN("TRY LOGS");
    //  LOGS
    if (telem_log_get_json_list(xtrObject, &remaining_json_size) > 0) {
        LOG_WRN("Log data did not fit in the message");
        finalReturn = 1;    // indicate we have more to process
    }

    cJSON_PrintPreallocated(topLevel, json_message_buffer, D1_JSON_MESSAGE_BUFFER_SIZE, false);

    *replyJson = json_message_buffer;

cleanup_and_exit:
    cJSON_Delete(topLevel);
    return finalReturn;
}

///////////////////////////////////////////////////
// json_onboarding()
// Create a JSON message for the onboarding msg
// MID: Machine ID
// gwid: unknown string
// market: 2 character market id
// returns: char * to json or null on error
///////////////////////////////////////////////////
char *json_onboarding(char *MID, char *gwid)
{
    int midlen = strlen(MID);
    if (midlen == 0 || midlen > MAX_MID_SIZE) {
        return NULL;
    }
    cJSON_Init();

    cJSON *topLevel = cJSON_CreateObject();
    cJSON_AddNumberToObject(topLevel, "P", ONBOARDING_PROTOCOL_VERSION);
    cJSON_AddStringToObject(topLevel, "MID", MID);
    cJSON_AddStringToObject(topLevel, "MK", CONFIG_IOT_MQTT_REGION_ID);
    cJSON_AddNumberToObject(topLevel, "T", MQTT_MESSAGE_TYPE_ONBOARDING);
    cJSON_AddNumberToObject(topLevel, "B", CONFIG_IOT_MQTT_BRAND_ID);
    cJSON *mObject   = cJSON_AddObjectToObject(topLevel, "M");
    cJSON *gwiObject = cJSON_AddObjectToObject(mObject, "GWI");
    cJSON_AddStringToObject(gwiObject, "GWID", gwid);

    cJSON_PrintPreallocated(topLevel, json_message_buffer, D1_JSON_MESSAGE_BUFFER_SIZE, false);
    cJSON_Delete(topLevel);

    return json_message_buffer;
}

///////////////////////////////////////////////////
// json_shadow_report()
// Create a JSON message for the shadow request msg
// MID: Machine ID
// gwid: unknown string
// market: 2 character market id
// returns: char * to json or null on error
///////////////////////////////////////////////////
char *json_shadow_report(char *MID, shadow_doc_t *doc, shadow_extra_t *extra)
{
    int midlen = strlen(MID);
    if (midlen == 0 || midlen > MAX_MID_SIZE) {
        return NULL;
    }
    cJSON_Init();

    cJSON *topLevel = cJSON_CreateObject();
    cJSON_AddNumberToObject(topLevel, "P", SHADOW_PROTOCOL_VERSION);
    cJSON_AddStringToObject(topLevel, "MID", MID);
    cJSON_AddStringToObject(topLevel, "MK", CONFIG_IOT_MQTT_REGION_ID);
    cJSON_AddNumberToObject(topLevel, "B", CONFIG_IOT_MQTT_BRAND_ID);
    cJSON_AddNumberToObject(topLevel, "T", MQTT_MESSAGE_TYPE_SHADOW_PROXY);
    cJSON *mObject = cJSON_AddObjectToObject(topLevel, "M");
    cJSON_AddNumberToObject(mObject, "S_Norm", doc->S_Norm);
    cJSON_AddNumberToObject(mObject, "S_FMD", doc->S_FMD);
    cJSON_AddNumberToObject(mObject, "T_Norm", doc->T_Norm);
    cJSON_AddNumberToObject(mObject, "T_FMD", doc->T_FMD);
    cJSON_AddNumberToObject(mObject, "Rec", doc->Rec);
    cJSON_AddNumberToObject(mObject, "Q", doc->Q);
    cJSON_AddNumberToObject(mObject, "THS", doc->ths);
    cJSON_AddNumberToObject(mObject, "DUR", doc->dur);
    cJSON_AddNumberToObject(mObject, "MOT_DET", doc->mot_det);
    char ver_str[32];
    snprintf(ver_str, 32, "%d.%d.%d", doc->mcuVer[0], doc->mcuVer[1], doc->mcuVer[2]);
    cJSON_AddStringToObject(mObject, "MV", ver_str);
    snprintf(ver_str, 32, "%d.%d.%d", doc->wifiVer[0], doc->wifiVer[1], doc->wifiVer[2]);
    cJSON_AddStringToObject(mObject, "WV", ver_str);
    snprintf(ver_str, 32, "%d.%d.%d", doc->lteVer[0], doc->lteVer[1], doc->lteVer[2]);
    cJSON_AddStringToObject(mObject, "LV", ver_str);
    cJSON_AddNumberToObject(mObject, "F_P_DUR", doc->fota_in_progress_duration);
    cJSON_AddNumberToObject(mObject, "GPS_PER", doc->gps_poll_period);
    if (extra != NULL) {
        for (int i = 0; i < extra->num; i++) {
            if (extra->values[i] != NULL) {
                cJSON_AddStringToObject(mObject, extra->keys[i], extra->values[i]);
            } else {
                cJSON_AddNullToObject(mObject, extra->keys[i]);
            }
        }
    }
    cJSON *zArray = cJSON_AddArrayToObject(mObject, "ZONES");
    for (int i = 0; i < NUM_ZONES; i++) {
        cJSON *zObj = cJSON_CreateObject();
        cJSON_AddNumberToObject(zObj, "I", doc->zones[i].idx);
        cJSON_AddStringToObject(zObj, "SSID", doc->zones[i].ssid);
        cJSON_AddBoolToObject(zObj, "SAFE", doc->zones[i].safe);
        cJSON_AddItemToArray(zArray, zObj);
    }

    cJSON_PrintPreallocated(topLevel, json_message_buffer, D1_JSON_MESSAGE_BUFFER_SIZE, false);
    cJSON_Delete(topLevel);

    return json_message_buffer;
}

///////////////////////////////////////////////////
// json_parse_shadow_report()
// extract a shadow_doc_t from a JSON message. Used
// to read the shadow save file and to parse the
// incoming shadow message.  In the case of incoming
// shadow message, not all the fields may be present
//
// @param str: JSON string
// @param newdoc: shadow_doc_t to fill
// @param olddoc: current shadow_doc_t (used as defaults)
//
// returns: 0 on success, < 0 on error
///////////////////////////////////////////////////
int json_parse_shadow_report(char *json_buf, shadow_doc_t *doc, shadow_doc_t *olddoc)
{
    int ret = -EINVAL;    // If the doc they send has no values we care about, we return this
    if (json_buf == NULL || doc == NULL || olddoc == NULL) {
        return -EINVAL;
    }
    // Start the values using the existing value because the backend only sends
    // us deltas
    doc->S_Norm                    = olddoc->S_Norm;
    doc->S_FMD                     = olddoc->S_FMD;
    doc->T_Norm                    = olddoc->T_Norm;
    doc->T_FMD                     = olddoc->T_FMD;
    doc->Rec                       = olddoc->Rec;
    doc->Q                         = olddoc->Q;
    doc->ths                       = olddoc->ths;
    doc->dur                       = olddoc->dur;
    doc->mot_det                   = olddoc->mot_det;
    doc->mcuVer[0]                 = 0;
    doc->mcuVer[1]                 = 0;
    doc->mcuVer[2]                 = 0;
    doc->wifiVer[0]                = 0;
    doc->wifiVer[1]                = 0;
    doc->wifiVer[2]                = 0;
    doc->lteVer[0]                 = 0;
    doc->mcuVer[1]                 = 0;
    doc->mcuVer[2]                 = 0;
    doc->fota_in_progress_duration = olddoc->fota_in_progress_duration;
    doc->gps_poll_period           = olddoc->gps_poll_period;
    for (int i = 0; i < NUM_ZONES; i++) {
        doc->zones[i].idx = olddoc->zones[i].idx;
        strncpy(doc->zones[i].ssid, olddoc->zones[i].ssid, 32);
        doc->zones[i].ssid[32] = '\0';
        doc->zones[i].safe     = olddoc->zones[i].safe;
    }
    cJSON *mObj, *obj;
    cJSON *json = cJSON_Parse(json_buf);
    if (json == NULL) {
        return -ENOMSG;
    }

    if ((mObj = cJSON_GetObjectItem(json, "M")) == NULL) {
        cJSON_Delete(json);
        return -EINVAL;
    }

    if ((obj = cJSON_GetObjectItem(mObj, "S_Norm")) != NULL) {
        doc->S_Norm = cJSON_GetNumberValue(obj);
        ret         = 0;
        if (doc->S_Norm < 10) {
            LOG_ERR("S_Norm too small in json, using 10: %d", doc->S_Norm);
            doc->S_Norm = 10;
        }
        if (doc->S_Norm > 65534) {
            LOG_ERR("S_Norm too big in json, using 65534: %d", doc->S_Norm);
            doc->S_Norm = 65534;
        }
    }

    if ((obj = cJSON_GetObjectItem(mObj, "S_FMD")) != NULL) {
        doc->S_FMD = cJSON_GetNumberValue(obj);
        ret        = 0;
        if (doc->S_FMD < 10) {
            LOG_ERR("S_FMD too small in json, using 10: %d", doc->S_FMD);
            doc->S_FMD = 10;
        }
        if (doc->S_FMD > 65534) {
            LOG_ERR("S_FMD too big in json, using 65534: %d", doc->S_FMD);
            doc->S_FMD = 65534;
        }
    }

    if ((obj = cJSON_GetObjectItem(mObj, "T_Norm")) != NULL) {
        doc->T_Norm = cJSON_GetNumberValue(obj);
        ret         = 0;
        if (doc->T_Norm < 1) {
            LOG_ERR("T_Norm too small in json, using 1: %d", doc->T_Norm);
            doc->T_Norm = 1;
        }
        if (doc->T_Norm > 100) {
            LOG_ERR("T_Norm too big in json, using 100: %d", doc->T_Norm);
            doc->T_Norm = 100;
        }
    }

    if ((obj = cJSON_GetObjectItem(mObj, "T_FMD")) != NULL) {
        doc->T_FMD = cJSON_GetNumberValue(obj);
        ret        = 0;
        if (doc->T_FMD < 1) {
            LOG_ERR("T_FMD too small in json, using 1: %d", doc->T_FMD);
            doc->T_FMD = 1;
        }
        if (doc->T_FMD > 100) {
            LOG_ERR("T_FMD too big in json, using 100: %d", doc->T_FMD);
            doc->T_FMD = 100;
        }
    }

    if ((obj = cJSON_GetObjectItem(mObj, "Rec")) != NULL) {
        doc->Rec = cJSON_GetNumberValue(obj);
        ret      = 0;
        if (doc->Rec < 10) {
            LOG_ERR("Rec too small in json, using 10: %d", doc->Rec);
            doc->Rec = 10;
        }
        if (doc->Rec > 65534) {
            LOG_ERR("Rec too big in json, using 65534: %d", doc->Rec);
            doc->Rec = 65534;
        }
    }

    if ((obj = cJSON_GetObjectItem(mObj, "Q")) != NULL) {
        doc->Q = cJSON_GetNumberValue(obj);
        ret    = 0;
        if (doc->Q < 1) {
            LOG_ERR("Q too small in json, using 1: %d", doc->Q);
            doc->Q = 1;
        }
        if (doc->Q > 65534) {
            LOG_ERR("Q too big in json, using 65534: %d", doc->Q);
            doc->Q = 65534;
        }
    }

    if ((obj = cJSON_GetObjectItem(mObj, "THS")) != NULL) {
        doc->ths = cJSON_GetNumberValue(obj);
        ret      = 0;
        if (doc->ths < 0) {
            LOG_ERR("THS too small in json, using 0: %d", doc->ths);
            doc->ths = 0;
        }
        if (doc->ths > 7) {
            LOG_ERR("THS too big in json, using 7: %d", doc->ths);
            doc->ths = 7;
        }
    }

    if ((obj = cJSON_GetObjectItem(mObj, "DUR")) != NULL) {
        doc->dur = cJSON_GetNumberValue(obj);
        ret      = 0;
        if (doc->dur < 0) {
            LOG_ERR("DUR too small in json, using 0: %d", doc->dur);
            doc->dur = 0;
        }
        if (doc->dur > 65535) {
            LOG_ERR("DUR too big in json, using 65535: %d", doc->dur);
            doc->dur = 65535;
        }
    }

    if ((obj = cJSON_GetObjectItem(mObj, "MV")) != NULL) {
        char *ver = cJSON_GetStringValue(obj);
        if (sprintf(ver, "%d.%d.%d", doc->mcuVer[0], doc->mcuVer[1], doc->mcuVer[2]) != 7) {
            LOG_WRN("Failed to get MCU version from shadow json");
        }
    }

    if ((obj = cJSON_GetObjectItem(mObj, "WV")) != NULL) {
        char *ver = cJSON_GetStringValue(obj);
        if (sprintf(ver, "%d.%d.%d", doc->wifiVer[0], doc->wifiVer[1], doc->wifiVer[2]) != 7) {
            LOG_WRN("Failed to get Wifi version from shadow json");
        }
    }

    if ((obj = cJSON_GetObjectItem(mObj, "LV")) != NULL) {
        char *ver = cJSON_GetStringValue(obj);
        if (sprintf(ver, "%d.%d.%d", doc->lteVer[0], doc->lteVer[1], doc->lteVer[2]) != 7) {
            LOG_WRN("Failed to get LTE version from shadow json");
        }
    }

    if ((obj = cJSON_GetObjectItem(mObj, "F_P_DUR")) != NULL) {
        ret                            = 0;
        doc->fota_in_progress_duration = cJSON_GetNumberValue(obj);
    }

    if ((obj = cJSON_GetObjectItem(mObj, "GPS_PER")) != NULL) {
        ret                  = 0;
        doc->gps_poll_period = cJSON_GetNumberValue(obj);
    }

    if ((obj = cJSON_GetObjectItem(mObj, "MOT_DET")) != NULL) {
        ret                  = 0;
        doc->mot_det = cJSON_GetNumberValue(obj);
    }

    if ((obj = cJSON_GetObjectItem(mObj, "ZONES")) != NULL) {
        ret         = 0;
        cJSON *zval = NULL;
        int    i    = -1;
        cJSON_ArrayForEach(zval, obj)
        {
            i++;
            cJSON *iobj = cJSON_GetObjectItem(zval, "I");
            if (iobj == NULL || !cJSON_IsNumber(iobj)) {
                LOG_ERR("Got ZONE entry without a valid 'I' field at json idx %d", i);
                continue;
            }
            int di = cJSON_GetNumberValue(iobj);
            if (di < 0 || di >= NUM_ZONES) {
                LOG_ERR("ZONE stored index out of range: %d", di);
                continue;
            }

            cJSON *zobj = cJSON_GetObjectItem(zval, "SSID");
            cJSON *sobj = cJSON_GetObjectItem(zval, "SAFE");
            if (zobj == NULL && sobj == NULL) {
                LOG_ERR(
                    "Got ZONE entry with 'I' of %d without a SSID or SAFE "
                    "field at json idx %d",
                    di,
                    i);
                continue;
            }

            doc->zones[di].idx = di;
            if (cJSON_IsString(zobj)) {
                strncpy(doc->zones[di].ssid, zobj->valuestring, 32);
                doc->zones[di].ssid[32] = 0;
            } else {
                LOG_ERR("SSID with 'I' %d at json idx %d is not string", di, i);
                continue;
            }

            if (cJSON_IsBool(sobj)) {
                bool safe           = cJSON_IsTrue(sobj);
                doc->zones[di].safe = safe;
            } else {
                LOG_ERR("SAFE with 'I' %d at json idx %d is not a bool", di, i);
                LOG_ERR("SAFE is the wrong type ");
                continue;
            }
        }
    } else {
        LOG_DBG("No ZONES in shadow json");
    }
    cJSON_Delete(json);
    return ret;
}

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
    char *MID, uint16_t version_major, uint16_t version_minor, uint16_t version_patch, char *nonce, comm_device_type_t device_type)
{
    // "{\"MID\":\"%s\",\"MK\":\"CH\",\"T\":4,\"P\":5,\"M\":{\"DM\":0,\"LM\":0,\"REV\":[{\"HWR\":\"1.0.0\",\"DT\":\"recipe\",\"FWR\":\"1.0.3\"}],\"FV\":\"2.1\",\"N\":\"12345678\"},\"B\":%d}";

    int midlen = strlen(MID);
    if (midlen == 0 || midlen > MAX_MID_SIZE) {
        return NULL;
    }
    cJSON_Init();

    cJSON *topLevel = cJSON_CreateObject();
    cJSON_AddNumberToObject(topLevel, "P", 5);
    cJSON_AddStringToObject(topLevel, "MID", MID);
    cJSON_AddStringToObject(topLevel, "MK", CONFIG_IOT_MQTT_REGION_ID);
    cJSON_AddNumberToObject(topLevel, "T", MQTT_MESSAGE_TYPE_FOTA);
    cJSON_AddNumberToObject(topLevel, "B", CONFIG_IOT_MQTT_BRAND_ID);
    cJSON *mObject = cJSON_AddObjectToObject(topLevel, "M");
    cJSON_AddNumberToObject(mObject, "DM", 0);
    cJSON_AddNumberToObject(mObject, "LM", 0);
    cJSON *revArray = cJSON_AddArrayToObject(mObject, "REV");
    cJSON *rev0     = cJSON_CreateObject();
    cJSON_AddStringToObject(rev0, "HWR", "1.0.0");
    LOG_DBG("device_type: %d", device_type);
    switch (device_type) {
    case COMM_DEVICE_NRF9160:
        cJSON_AddStringToObject(rev0, "DT", "nrf9160");
        break;
    case COMM_DEVICE_DA16200:
        cJSON_AddStringToObject(rev0, "DT", "da16200");
        break;
    case COMM_DEVICE_NRF5340:
        cJSON_AddStringToObject(rev0, "DT", "nrf5340");
        break;
    case COMM_DEVICE_ALL:
        // cJSON_AddStringToObject(rev0, "DT", "all");
        break;
    case COMM_DEVICE_NONE:
    case COMM_DEVICE_ANY:
    default:
        break;
    }
    // cJSON_AddStringToObject(rev0, "DT", "recipe");
    char fwr[32];
    snprintf(fwr, 32, "%d.%d.%d", version_major, version_minor, version_patch);
    cJSON_AddStringToObject(rev0, "FWR", fwr);
    cJSON_AddItemToArray(revArray, rev0);
    cJSON_AddStringToObject(mObject, "FV", "2.1");
    cJSON_AddStringToObject(mObject, "N", nonce);

    cJSON_PrintPreallocated(topLevel, json_message_buffer, D1_JSON_MESSAGE_BUFFER_SIZE, false);
    cJSON_Delete(topLevel);

    return json_message_buffer;
}

///////////////////////////////////////////////////
// json_parse_mqtt_string(char *str)
// Parse a string into a cJSON object
// str: string to parse
// returns: cJSON object or NULL on error
///////////////////////////////////////////////////
cJSON *json_parse_mqtt_string(char *str)
{
    cJSON *json = cJSON_Parse(str);
    if (json == NULL) {
        return NULL;
    }
    return json;
}

///////////////////////////////////////////////////
// json_get_mqtt_type(cJSON *obj)
// Get the type of the MQTT message
// obj: cJSON object
// returns: mqtt_message_type_t
///////////////////////////////////////////////////
mqtt_message_type_t json_get_mqtt_type(cJSON *obj)
{
    cJSON *type = cJSON_GetObjectItem(obj, "T");
    if (type == NULL) {
        return MQTT_MESSAGE_TYPE_UNKNOWN;
    }
    return (mqtt_message_type_t)type->valueint;
}

///////////////////////////////////////////////////
// json_connectivity_msg()
// json_connectivity_report()
// Create a JSON message for the connectivity test
// which can be variable in size
///////////////////////////////////////////////////
char *json_connectivity_msg(char *MID, int msg_size)
{
    int midlen = strlen(MID);
    if (midlen == 0 || midlen > MAX_MID_SIZE) {
        return NULL;
    }
    if (msg_size < 200 || msg_size > D1_JSON_MESSAGE_BUFFER_SIZE) {
        return NULL;
    }
    cJSON_Init();

    cJSON *topLevel = cJSON_CreateObject();
    cJSON_AddNumberToObject(topLevel, "P", CONNECTIVITY_PROTOCOL_VERSION);
    cJSON_AddStringToObject(topLevel, "MID", MID);
    cJSON_AddStringToObject(topLevel, "MK", CONFIG_IOT_MQTT_REGION_ID);
    cJSON_AddNumberToObject(topLevel, "B", CONFIG_IOT_MQTT_BRAND_ID);
    cJSON_AddNumberToObject(topLevel, "T", MQTT_MESSAGE_TYPE_CONN_TEST);
    cJSON *mObject = cJSON_AddObjectToObject(topLevel, "M");
    // The JSON without the M object is about 100 bytes
    // so we add Sx objects until we read the size desired
    int curr_size = 100;
    int i         = 0;
    while (curr_size < msg_size) {
        char s[12];
        char d[100];
        if (i == 0) {
            strncpy(s, "S", 2);
        } else {
            snprintf(s, 12, "S%d", i);
        }
        i++;
        int amt = (msg_size - curr_size) % 100;
        if (amt == 0) {
            amt = 100;
        }
        for (int j = 0; j < amt; j++) {
            d[j] = 'a' + rand() % 26;
        }
        d[amt] = '\0';
        cJSON_AddStringToObject(mObject, s, d);
        curr_size += amt;
    }
    cJSON_PrintPreallocated(topLevel, json_message_buffer, D1_JSON_MESSAGE_BUFFER_SIZE, false);
    cJSON_Delete(topLevel);

    return json_message_buffer;
}

///////////////////////////////////////////////////
// json_near_realtime_msg()
// json_connectivity_report()
// Create a JSON message for the connectivity test
// which can be variable in size
///////////////////////////////////////////////////
char *json_near_realtime_msg(char *MID, int sub_type, char *payload)
{
    int midlen = strlen(MID);
    if (midlen == 0 || midlen > MAX_MID_SIZE) {
        return NULL;
    }
    int msg_size = strlen(payload);
    if (msg_size > D1_JSON_MESSAGE_BUFFER_SIZE) {
        return NULL;
    }
    cJSON_Init();

    cJSON *topLevel = cJSON_CreateObject();
    cJSON_AddNumberToObject(topLevel, "P", REALTIME_PROTOCOL_VERSION);
    cJSON_AddStringToObject(topLevel, "MID", MID);
    cJSON_AddStringToObject(topLevel, "MK", CONFIG_IOT_MQTT_REGION_ID);
    cJSON_AddNumberToObject(topLevel, "B", CONFIG_IOT_MQTT_BRAND_ID);
    cJSON_AddNumberToObject(topLevel, "T", MQTT_MESSAGE_TYPE_NEAR_REAL_TIME);
    cJSON *mObject = cJSON_AddObjectToObject(topLevel, "M");
    cJSON_AddNumberToObject(mObject, "SUB", sub_type);
    cJSON_AddStringToObject(mObject, "PL", payload);

    cJSON_PrintPreallocated(topLevel, json_message_buffer, D1_JSON_MESSAGE_BUFFER_SIZE, false);
    cJSON_Delete(topLevel);

    return json_message_buffer;
}

///////////////////////////////////////////////////
// json_safe_zone_alert()
// MID: Machine ID
// type: alert type
// ssid: ssid where we left or entered
// enter: true if we entered the safe zone, false if we left
///////////////////////////////////////////////////
char *json_safe_zone_alert(char *MID, uint64_t time, int type, char *ssid, bool enter, char *reason)
{
    char buf[64];
    int  midlen = strlen(MID);
    if (midlen == 0 || midlen > MAX_MID_SIZE) {
        return NULL;
    }
    cJSON_Init();

    cJSON *topLevel = cJSON_CreateObject();
    cJSON_AddNumberToObject(topLevel, "P", REALTIME_PROTOCOL_VERSION);
    cJSON_AddStringToObject(topLevel, "MID", MID);
    cJSON_AddStringToObject(topLevel, "MK", CONFIG_IOT_MQTT_REGION_ID);
    cJSON_AddNumberToObject(topLevel, "B", CONFIG_IOT_MQTT_BRAND_ID);
    cJSON_AddNumberToObject(topLevel, "T", MQTT_MESSAGE_TYPE_NEAR_REAL_TIME);
    cJSON *mObject = cJSON_AddObjectToObject(topLevel, "M");
    cJSON_AddNumberToObject(mObject, "SUB", SAFE_ZONE_SUB_TYPE);
    cJSON *mpl = cJSON_AddObjectToObject(mObject, "PL");
    cJSON_AddNumberToObject(mpl, "CRON", time);
    cJSON_AddNumberToObject(mpl, "ALERT_TYPE", type);
    snprintf(buf, 64, "%s", ssid);
    cJSON_AddStringToObject(mpl, "ALERT_DESC", buf);
    if (reason != NULL && strlen(reason) <= 40) {
        cJSON_AddStringToObject(mpl, "REASON", reason);
    }
    int movement = imu_get_trigger_count();
    cJSON_AddNumberToObject(mpl, "MOVEMENT", movement);

    cJSON_PrintPreallocated(topLevel, json_message_buffer, D1_JSON_MESSAGE_BUFFER_SIZE, false);
    cJSON_Delete(topLevel);

    return json_message_buffer;
}

///////////////////////////////////////////////////
// json_pair_msg()
///////////////////////////////////////////////////
char *json_pair_msg(char *MID, char *nonce, char *iccd, char *imei, char *ble_mac, char *model)
{
    int midlen = strlen(MID);
    if (midlen == 0 || midlen > MAX_MID_SIZE) {
        return NULL;
    }
    int msg_size = strlen(nonce);
    if (msg_size > D1_JSON_MESSAGE_BUFFER_SIZE) {
        return NULL;
    }
    cJSON_Init();

    cJSON *topLevel = cJSON_CreateObject();
    cJSON_AddNumberToObject(topLevel, "P", REALTIME_PROTOCOL_VERSION);
    cJSON_AddStringToObject(topLevel, "MID", MID);
    cJSON_AddStringToObject(topLevel, "MK", CONFIG_IOT_MQTT_REGION_ID);
    cJSON_AddNumberToObject(topLevel, "B", CONFIG_IOT_MQTT_BRAND_ID);
    cJSON_AddNumberToObject(topLevel, "T", MQTT_MESSAGE_TYPE_NEAR_REAL_TIME);
    cJSON *mObject = cJSON_AddObjectToObject(topLevel, "M");
    cJSON_AddNumberToObject(mObject, "SUB", PAIRING_SUB_TYPE);
    cJSON *plObject = cJSON_AddObjectToObject(mObject, "PL");
    cJSON_AddStringToObject(plObject, "PT", nonce);
    cJSON_AddNumberToObject(plObject, "CRON", get_unix_time());
    cJSON_AddStringToObject(plObject, "IMEI", imei);
    cJSON_AddStringToObject(plObject, "ICCID", iccd);
    char model_str[5];
    strncpy(model_str, model, 4);
    model_str[4] = '\0';
    cJSON_AddStringToObject(plObject, "ModelNumber", model_str);
    cJSON_AddStringToObject(plObject, "MacAddress", ble_mac);

    cJSON_PrintPreallocated(topLevel, json_message_buffer, D1_JSON_MESSAGE_BUFFER_SIZE, false);
    cJSON_Delete(topLevel);

    return json_message_buffer;
}

#ifndef NAN
#define NAN 0.0 / 0.0
#endif

///////////////////////////////////////////////////
// json_fota_validation_msg()
///////////////////////////////////////////////////
char *json_fota_feedback_msg(char *MID, int errorCode, char *deployType, char *requestID, int64_t timeStamp, char *state)
{
    int midlen = strlen(MID);
    if (midlen == 0 || midlen > MAX_MID_SIZE) {
        return NULL;
    }
    int depT = strlen(deployType);
    if (depT == 0 || depT > DEPLOY_TYPE_SIZE) {
        return NULL;
    }
    int reqID = strlen(requestID);
    if (reqID == 0 || reqID > REQUEST_ID_SIZE) {
        return NULL;
    }
    int stateTest = strlen(state);
    if (stateTest == 0 || stateTest > REQUEST_ID_SIZE) {
        return NULL;
    }
    if (timeStamp < 1000000000) {    // device didnt exist at Sept 9,2001, 1:46:40
        return NULL;
    }
    cJSON_Init();

    cJSON *ret      = NULL;
    cJSON *topLevel = cJSON_CreateObject();
    cJSON_AddNumberToObject(topLevel, "P", FOTA_LIFECYCLE_PROTOCOL_VERSION);
    cJSON_AddStringToObject(topLevel, "MID", MID);
    cJSON_AddStringToObject(topLevel, "MK", CONFIG_IOT_MQTT_REGION_ID);
    cJSON_AddNumberToObject(topLevel, "B", CONFIG_IOT_MQTT_BRAND_ID);
    cJSON_AddNumberToObject(topLevel, "T", MQTT_MESSAGE_TYPE_FOTA_LIFE);
    cJSON *mObject = cJSON_AddObjectToObject(topLevel, "M");
    cJSON_AddStringToObject(mObject, "RID", requestID);
    cJSON *alArray = cJSON_AddArrayToObject(mObject, "AL");
    cJSON *resp    = cJSON_CreateObject();

    ret = cJSON_AddStringToObject(resp, "DT", deployType);
    if (ret == NULL) {
        printf("error with DT\n");
        goto cleanup;
    }
    ret = cJSON_AddNumberToObject(resp, "EC", errorCode);
    if (ret == NULL) {
        printf("error with EC\n");
        goto cleanup;
    }

    ret = cJSON_AddNumberToObject(resp, "TS", timeStamp);
    if (ret == NULL) {
        printf("error with T\n");
        goto cleanup;
    }

    ret = cJSON_AddTrueToObject(resp, "SR");
    if (ret == NULL) {
        printf("error with SR\n");
        goto cleanup;
    }
    if (state) {
        ret = cJSON_AddStringToObject(resp, "ST", state);
        if (ret == NULL) {
            printf("error with ST\n");
            goto cleanup;
        }
    }

    cJSON_AddItemToArray(alArray, resp);

    cJSON_PrintPreallocated(topLevel, json_message_buffer, D1_JSON_MESSAGE_BUFFER_SIZE, false);

cleanup:
    cJSON_Delete(topLevel);
    return json_message_buffer;
}

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
    int            chunk_num)
{
    int finalReturn = 0;
    int midlen      = strlen(MID);
    if (midlen == 0 || midlen > MAX_MID_SIZE) {
        return -EINVAL;
    }
    if (request_id == NULL) {
        return -EINVAL;
    }
    cJSON_Init();

    cJSON *topLevel = cJSON_CreateObject();
    cJSON_AddNumberToObject(topLevel, "P", REALTIME_PROTOCOL_VERSION);
    cJSON_AddStringToObject(topLevel, "MID", MID);
    cJSON_AddStringToObject(topLevel, "MK", CONFIG_IOT_MQTT_REGION_ID);
    cJSON_AddNumberToObject(topLevel, "B", CONFIG_IOT_MQTT_BRAND_ID);
    cJSON_AddNumberToObject(topLevel, "T", MQTT_MESSAGE_TYPE_NEAR_REAL_TIME);
    cJSON *mObject = cJSON_AddObjectToObject(topLevel, "M");
    cJSON_AddNumberToObject(mObject, "SUB", WHERES_MY_DOG_SUB_TYPE);
    cJSON *plObject = cJSON_AddObjectToObject(mObject, "PL");

    cJSON_AddNumberToObject(plObject, "SENT_AT", 1919191919);

    if (cellid != NULL) {
        cJSON_AddNumberToObject(plObject, "CELLID", *cellid);
    }
    if (tracking_area != NULL) {
        cJSON_AddNumberToObject(plObject, "TRACKING_AREA", *tracking_area);
    }
    if ((dog_ssid != NULL) && (strlen(dog_ssid) > 0)) {
        cJSON_AddStringToObject(plObject, "DOG_SSID", dog_ssid);
        if (dog_in_safe_zone != NULL) {
            cJSON_AddBoolToObject(plObject, "DOG_IN_SAFE_ZONE", *dog_in_safe_zone);
        }
    }
    cJSON_AddNumberToObject(plObject, "CRON", get_unix_time());
    //cJSON_AddStringToObject(plObject, "REQUEST_ID", request_id);
    cJSON_AddNumberToObject(plObject, "FMD_STATUS", fmd_status());

    cJSON_PrintPreallocated(topLevel, json_message_buffer, D1_JSON_MESSAGE_BUFFER_SIZE, false);
    int     A_LITTE_PADDING_TO_BE_SAFE = 10;
    int16_t remaining_json_size = (CONFIG_MAX_MQTT_MSG_SIZE - A_LITTE_PADDING_TO_BE_SAFE) - strlen(json_message_buffer);

    // PUT THE FOLLOWING CALLS IN PRIORITY ORDER, SO THE MOST IMPORTANT DATA IS SENT IN THE FIRST MESSAGE
    // TODO: handle the <0 return from each of these functions

    // SSIDS
    if (wifi_ssids_fifo && !k_fifo_is_empty(wifi_ssids_fifo)) {
        cJSON_AddNumberToObject(plObject, "SSID_CRON", g_last_ssid_scan_time);
        remaining_json_size -= 25;    // size of ["SSID_CRON": 9999999999,],Good till 2286
    }
    if (add_ssids_to_json(plObject, wifi_ssids_fifo, &remaining_json_size) > 0) {
        LOG_WRN("SSID data did not fit in the message");
        finalReturn = 1;    // indicate we have more to process
    }

    if (send_gps) {
        if (gps_get_json_list(plObject, &remaining_json_size) > 0) {
            LOG_WRN("GPS data did not fit in the message");
            finalReturn = 1;    // indicate we have more to process
        }
    }

    cJSON_PrintPreallocated(topLevel, json_message_buffer, D1_JSON_MESSAGE_BUFFER_SIZE, false);
    cJSON_Delete(topLevel);

    *returnJSON = json_message_buffer;

    return finalReturn;
}

///////////////////////////////////////////////////
// comm_dev_str()
// Get the name of the comm device type
// device: comm device type
// returns: name of the comm device type
const char *comm_dev_str(comm_device_type_t device)
{
    switch (device) {
    case COMM_DEVICE_NONE:
        return "NONE";
    case COMM_DEVICE_NRF9160:
        return "NRF9160";
    case COMM_DEVICE_DA16200:
        return "DA16200";
    case COMM_DEVICE_NRF5340:
        return "NRF5340";
    case COMM_DEVICE_ALL:
        return "ALL";
    case COMM_DEVICE_ANY:
        return "ANY";
    default:
        return "UNKNOWN";
    }
}

char *json_srf_nonce(char *MID, int nonce)
{
    int midlen = strlen(MID);
    if (midlen == 0 || midlen > MAX_MID_SIZE) {
        return NULL;
    }

    cJSON_Init();

    cJSON *topLevel = cJSON_CreateObject();
    cJSON_AddNumberToObject(topLevel, "P", REALTIME_PROTOCOL_VERSION);
    cJSON_AddStringToObject(topLevel, "MID", MID);
    cJSON_AddStringToObject(topLevel, "MK", CONFIG_IOT_MQTT_REGION_ID);
    cJSON_AddNumberToObject(topLevel, "B", CONFIG_IOT_MQTT_BRAND_ID);
    cJSON_AddNumberToObject(topLevel, "T", MQTT_MESSAGE_TYPE_SRF_NONCE);
    cJSON *mObject = cJSON_AddObjectToObject(topLevel, "M");
    cJSON_AddNumberToObject(mObject, "N", nonce);

    cJSON_PrintPreallocated(topLevel, json_message_buffer, D1_JSON_MESSAGE_BUFFER_SIZE, false);
    cJSON_Delete(topLevel);

    return json_message_buffer;
}

///////////////////////////////////////////////////
// json_parse_srf()
// build struct from srf json
int json_parse_srf(char *str, srf_data_t *data)
{
    cJSON *json = cJSON_Parse(str);
    if (json == NULL) {
        return -EINVAL;
    }
    cJSON *obj;
    cJSON *params;
    char  *string;
    double val;

    if ((obj = cJSON_GetObjectItem(json, "rid")) != NULL) {
        string = cJSON_GetStringValue(obj);
        if (string == NULL) {
            cJSON_Delete(json);
            LOG_ERR("rid is null");
            return -ENOMSG;
        }
        strncpy(data->rid, string, 48);
        data->rid[47] = '\0';
    }

    if ((obj = cJSON_GetObjectItem(json, "sub")) != NULL) {
        string = cJSON_GetStringValue(obj);
        if (string == NULL) {
            cJSON_Delete(json);
            LOG_ERR("sub is null");
            return -ENOMSG;
        }
        strncpy(data->sub, string, 64);
        data->sub[63] = '\0';
        // // loop over srf_command_strings and find the index
        // for (int i = 0; i < SRF_COMMAND_COUNT; i++) {
        // 	if (strcmp(srf_command_strings[i], data->sub) == 0) {
        // 		data->command = i;
        // 		break;
        // 	}
        // }
    }

    if ((obj = cJSON_GetObjectItem(json, "exp")) != NULL) {
        val = cJSON_GetNumberValue(obj);
        if (val == NAN) {
            cJSON_Delete(json);
            LOG_ERR("exp is null");
            return -ENOMSG;
        }
        data->exp = (uint64_t)val;
    }

    if ((obj = cJSON_GetObjectItem(json, "iss")) != NULL) {
        string = cJSON_GetStringValue(obj);
        if (string == NULL) {
            cJSON_Delete(json);
            LOG_ERR("iss is null");
            return -ENOMSG;
        }
        memset(data->iss, 0, 64);
        strncpy(data->iss, string, 64);
        data->iss[63] = '\0';
    }

    if ((obj = cJSON_GetObjectItem(json, "n")) != NULL) {
        val = cJSON_GetNumberValue(obj);
        if (val == NAN) {
            cJSON_Delete(json);
            LOG_ERR("n is null");
            return -ENOMSG;
        }
        data->nonce = (uint64_t)val;
    }

    if ((obj = cJSON_GetObjectItem(json, "aud")) != NULL) {
        string = cJSON_GetStringValue(obj);
        if (string == NULL) {
            cJSON_Delete(json);
            LOG_ERR("aud is null");
            return -ENOMSG;
        }
        memset(data->aud, 0, 64);
        strncpy(data->aud, string, 64);
        data->aud[63] = '\0';
    }

    if ((obj = cJSON_GetObjectItem(json, "iat")) != NULL) {
        val = cJSON_GetNumberValue(obj);
        if (val == NAN) {
            cJSON_Delete(json);
            LOG_ERR("iat is null");
            return -ENOMSG;
        }
        data->iat = (uint64_t)val;
    }

    if ((obj = cJSON_GetObjectItem(json, "fpar")) != NULL) {
        if ((params = cJSON_GetObjectItem(obj, "params")) != NULL) {
            char cmd_str[64];
            memset(cmd_str, 0, 64);
            if ((obj = cJSON_GetObjectItem(params, "cmd")) != NULL) {
                string = cJSON_GetStringValue(obj);
                if (string == NULL) {
                    cJSON_Delete(json);
                    LOG_ERR("fpar/params/cmd is null");
                    return -ENOMSG;
                }
                strncpy(cmd_str, string, 64);

                // loop over srf_command_strings and find the index
                for (int i = 0; i < SRF_COMMAND_COUNT; i++) {
                    if (strcmp(srf_command_strings[i], cmd_str) == 0) {
                        data->command = i;
                        break;
                    }
                }

                if ((obj = cJSON_GetObjectItem(params, "p1")) != NULL) {
                    string = cJSON_GetStringValue(obj);
                    if (string == NULL) {
                        LOG_WRN("fpar/params/p1 is null");
                        data->param1[0] = '\0';
                    } else {
                        memset(data->param1, 0, 64);
                        strncpy(data->param1, string, strlen(string));
                        data->param1[63] = '\0';
                        LOG_WRN("param1: %s", data->param1);
                    }
                }

                if ((obj = cJSON_GetObjectItem(params, "p2")) != NULL) {
                    string = cJSON_GetStringValue(obj);
                    if (string == NULL) {
                        LOG_WRN("fpar/params/p2 is null");
                        data->param2[0] = '\0';
                    } else {
                        memset(data->param2, 0, 64);
                        strncpy(data->param2, string, strlen(string));
                        data->param2[63] = '\0';
                        LOG_WRN("param2: %s", data->param2);
                    }
                }

                if ((obj = cJSON_GetObjectItem(params, "p3")) != NULL) {
                    string = cJSON_GetStringValue(obj);
                    if (string == NULL) {
                        LOG_WRN("fpar/params/p3 is null");
                        data->param3[0] = '\0';
                    } else {
                        memset(data->param3, 0, 64);
                        strncpy(data->param3, string, strlen(string));
                        data->param3[63] = '\0';
                        LOG_WRN("param3: %s", data->param3);
                    }
                }

                if ((obj = cJSON_GetObjectItem(params, "p4")) != NULL) {
                    string = cJSON_GetStringValue(obj);
                    if (string == NULL) {
                        LOG_WRN("fpar/params/p4 is null");
                        data->param4[0] = '\0';
                    } else {
                        memset(data->param4, 0, 64);
                        strncpy(data->param4, string, strlen(string));
                        data->param4[63] = '\0';
                        LOG_WRN("param4: %s", data->param4);
                    }
                }
            }
        } else {
            LOG_ERR("fpar is empty");
        }
    }

    cJSON_Delete(json);
    return 0;
}

char *json_srf_response_message(char *rid, char *st, char *fres, uint64_t currTime)
{

    int ridlen = strlen(rid);
    if (ridlen == 0 || ridlen > 48) {
        return NULL;
    }
    int stlen = strlen(st);
    if (stlen == 0 || stlen > 64) {
        return NULL;
    }
    int freslen = strlen(fres);
    if (freslen == 0 || freslen > 64) {
        return NULL;
    }

    cJSON_Init();

    char *machine_id = uicr_serial_number_get();
    char  issuer[64];
    snprintf(issuer, 64, "%d_%s", CONFIG_IOT_MQTT_BRAND_ID, machine_id);

    int    oLen = 0;
    int    len  = 0;
    char   headerBuffer[256];
    cJSON *headerTop = cJSON_CreateObject();
    cJSON_AddStringToObject(headerTop, "alg", "none");
    cJSON_AddStringToObject(headerTop, "typ", "JWT");
    char headerEncBuffer[256];
    cJSON_PrintPreallocated(headerTop, headerBuffer, 256, false);
    base64_encode(headerEncBuffer, 256, &oLen, headerBuffer, strlen(headerBuffer));

    // this base64 lib pads with "=" so we need to remove them, as that is invalid
    // search back from the end and replace '=' with '\0', until you find a non '='
    len = strlen(headerEncBuffer);
    for (int i = len - 1; i >= 0; i--) {
        if (headerEncBuffer[i] == '=') {
            headerEncBuffer[i] = '\0';
        } else {
            break;
        }
    }

    char   respBuffer[256];
    cJSON *encodeTop = cJSON_CreateObject();
    cJSON_AddStringToObject(encodeTop, "rid", rid);
    cJSON_AddNumberToObject(encodeTop, "exp", currTime + (15 * 60));    // now + 5 mins
    cJSON_AddStringToObject(encodeTop, "iss", issuer);
    cJSON_AddStringToObject(encodeTop, "st", st);
    cJSON_AddNumberToObject(encodeTop, "iat", currTime);
    cJSON *fresObj = cJSON_AddObjectToObject(encodeTop, "fres");
    cJSON_AddStringToObject(fresObj, "errstr", fres);
    cJSON_PrintPreallocated(encodeTop, respBuffer, 256, false);

    char encBuffer[256];
    base64_encode(encBuffer, 256, &oLen, respBuffer, strlen(respBuffer));

    // this base64 lib pads with "=" so we need to remove them, as that is invalid
    // search back from the end and replace '=' with '\0', until you find a non '='
    len = strlen(encBuffer);
    for (int i = len - 1; i >= 0; i--) {
        if (encBuffer[i] == '=') {
            encBuffer[i] = '\0';
        } else {
            break;
        }
    }

    char finalEncBuffer[513];
    snprintf(finalEncBuffer, 513, "%s.%s.", headerEncBuffer, encBuffer);

    cJSON *topLevel = cJSON_CreateObject();
    cJSON_AddNumberToObject(topLevel, "P", REALTIME_PROTOCOL_VERSION);
    cJSON_AddStringToObject(topLevel, "MID", machine_id);
    cJSON_AddStringToObject(topLevel, "MK", CONFIG_IOT_MQTT_REGION_ID);
    cJSON_AddNumberToObject(topLevel, "B", CONFIG_IOT_MQTT_BRAND_ID);
    cJSON_AddNumberToObject(topLevel, "T", MQTT_MESSAGE_TYPE_SRF_FUNC);
    cJSON *mObject = cJSON_AddObjectToObject(topLevel, "M");
    cJSON_AddStringToObject(mObject, "SM", finalEncBuffer);

    cJSON_PrintPreallocated(topLevel, json_message_buffer, D1_JSON_MESSAGE_BUFFER_SIZE, false);
    cJSON_Delete(encodeTop);
    cJSON_Delete(topLevel);
    cJSON_Delete(headerTop);

    return json_message_buffer;
}

///////////////////////////////////////////////////
// ml_get_json_list()
//
//
int ml_get_json_list(cJSON *jsonObj, int16_t *remaining_space)
{
    int min_space_for_ml = 200;    // ST, AV, MV, RV, ET and at least 1 record - hand wavy math

    // see if there is ML data in the file system
    fqueue_t fq;

    fqueue_init(&fq, "ml", FQ_READ, false);

    uint8_t          cbor_buffer[sizeof(struct Inference) * 2];    // allow some overhead
    struct Inference ml_info;
    size_t           size = sizeof(cbor_buffer);
    if (fqueue_peek(&fq, cbor_buffer, &size, K_NO_WAIT)) {
        // the queue is empty ... send nothing
        // LOG_WRN("ML queue is empty");
        return 0;
    }
    // We came here, ML quue is NOT empty
    if (*remaining_space < min_space_for_ml) {
        LOG_WRN("There is ML, but no space for it, retry next time");
        return 1;
    }

    LOG_WRN("Create JSON ML");
    cJSON   *ml_obj = cJSON_AddObjectToObject(jsonObj, "ML");
    uint64_t start_time;
    uint64_t end_time = 0;
    char     tempSpace[256];

    cbor_decode_Inference(cbor_buffer, size, &ml_info, &size);
    start_time = ml_info._Inference_start;

    cJSON_AddNumberToObject(ml_obj, "ST", start_time / 1000);
    cJSON_AddStringToObject(ml_obj, "MV", ml_version());
    cJSON_AddStringToObject(ml_obj, "AV", ml_version());
    cJSON_AddStringToObject(ml_obj, "RV", ml_version());

    cJSON *ml_array = cJSON_AddArrayToObject(ml_obj, "AT");
    if (!ml_array) {
        LOG_ERR("Failed to create ml array");
        return -1;
    }

    // update for the space the header used
    cJSON_PrintPreallocated(ml_obj, tempSpace, 256, false);
    *remaining_space -= strlen(tempSpace);

    int n_ml_recs = 1;
    int act_recs  = 0;
    int ret_value = 0;
    while (1) {
        // get the ML data from the file queue object
        cJSON *ml_inf_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(ml_inf_obj, "CA", ml_info._Inference_activity);
        cJSON_AddNumberToObject(ml_inf_obj, "PR", roundf_val(ml_info._Inference_reps, 3));
        cJSON_AddNumberToObject(ml_inf_obj, "PP", roundf_val(ml_info._Inference_probability, 3));
        cJSON_AddNumberToObject(ml_inf_obj, "ST", ml_info._Inference_start / 1000);
        cJSON_AddNumberToObject(ml_inf_obj, "TS", (ml_info._Inference_end - ml_info._Inference_start) / 1000);
        cJSON_AddNumberToObject(ml_inf_obj, "AM", roundf_val(ml_info._Inference_am, 3));
        cJSON_AddNumberToObject(ml_inf_obj, "AS", roundf_val(ml_info._Inference_as, 3));
        cJSON_AddNumberToObject(ml_inf_obj, "GM", roundf_val(ml_info._Inference_gm, 3));
        cJSON_AddNumberToObject(ml_inf_obj, "GS", roundf_val(ml_info._Inference_gs, 3));

        // Acertain if it will fit
        cJSON_PrintPreallocated(ml_inf_obj, tempSpace, 256, false);
        if (cJSON_GetArraySize(ml_inf_obj) < ML_MINIMUM_ELEMENTS) {
            LOG_ERR("MHR - error condition, unknown reason..abort");
            LOG_ERR("size is %d", cJSON_GetArraySize(ml_inf_obj));
            cJSON_Delete(ml_inf_obj);
            ret_value = 1;
            goto graceful_exit;
        }

        LOG_WRN(
            "New ML entry size %d strlen: %d remaing_space: %d",
            cJSON_GetArraySize(ml_inf_obj),
            strlen(tempSpace),
            *remaining_space);
#define MIN_SPACE_FOR_ET_FIELD (20)    // size of "ET": 1726841031 + SOME BUFFER
        if (*remaining_space < strlen(tempSpace) + MIN_SPACE_FOR_ET_FIELD) {
            cJSON_Delete(ml_inf_obj);
            ret_value = 1;    // we're out of space and need to continue on the next loop
            goto graceful_exit;
        }
        *remaining_space -= strlen(tempSpace);
        // It will fit, go ahead and add it
        fqueue_get(&fq, cbor_buffer, &size, K_NO_WAIT);    /// POP the data
        cJSON_AddItemToArray(ml_array, ml_inf_obj);
        ++act_recs;
        // the records should be in order, so the last record is the overall end time
        end_time    = ml_info._Inference_end;
        size_t size = sizeof(cbor_buffer);
        if (fqueue_peek(&fq, cbor_buffer, &size, K_NO_WAIT)) {
            LOG_WRN("Reached end of queue");
            break;
        }
        cbor_decode_Inference(cbor_buffer, size, &ml_info, &size);
        n_ml_recs++;
    }
graceful_exit:
    if (act_recs > 0) {    // We have added at least 1 ml array
        cJSON_AddNumberToObject(ml_obj, "ET", end_time / 1000);
        *remaining_space -= MIN_SPACE_FOR_ET_FIELD;
    }
    LOG_WRN("Added %d/%d records, covering a %lldms time", act_recs, n_ml_recs, end_time - start_time);
    return ret_value;
}
