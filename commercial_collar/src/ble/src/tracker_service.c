/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
/*
 *
 * SPDX-License-Identifier: LicenseRef-Proprietary
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/random/rand32.h>
#include <zephyr/settings/settings.h>

#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <zcbor_common.h>

#include "app_version.h"
#include "ble.h"
#include "commMgr.h"
#include "d1_json.h"
#include "modem.h"
#include "net_mgr.h"
#include "pmic.h"
#include "self_check.h"
#include "tracker_service.h"
#include "uicr.h"
#include "utils.h"
#include "wifi.h"
#include "wifi_at.h"
#include "fota.h"
#include "d1_json.h"
#include "radioMgr.h"
#include "pmic_leds.h"
#define ONBOARDED_SETTINGS_PATH "tracker_service/onboarded"

LOG_MODULE_REGISTER(tracker_service, CONFIG_TRACKER_SERVICE_LOG_LEVEL);

// type for our flags->enum lookup table
typedef struct
{
    uint8_t flag_str[64];
    uint8_t security_protocol;
    uint8_t encryption;
    uint8_t key_index;    // TODO: not sure what to do here, it's used for WEP
} ApScanFlagsToEnumT;

// TODO: add more commonly found network flags and their protocol/encryption type here
// If it turns out that there are too many permutations of these flags we'll have
// to resort to parsing instead of a lookup table
ApScanFlagsToEnumT m_ap_flags_lookup[] = {
    { "[WPA-PSK-CCMP+TKIP][WPA2-PSK-CCMP+TKIP][WPS][ESS]", 4, 2, 0 },    // found in UM-WI-003
    { "[WPA2-PSK-CCMP][ESS]", 3, 1, 0 },                                 // a Google WiFi AP, verified
    { "[WPA2-PSK-CCMP][WPS][ESS]", 3, 1, 0 },                            // a home ATT WiFi AP
    { "[WPA2-SAE-CCMP][ESS]", 6, 1, 0 },        // a Pixel Hotspot set as "WPA3-Personal", verified
    { "[ESS]", 0, 0, 0 },                       // a Pixel Hotspot set as "None", verified
    { "[WPA2-PSK+SAE-CCMP][ESS]", 7, 1, 0 },    // a Pixel set as "WPA2/WPA3-Personal", verified
    { "[OPEN]", 0, 0, 0 },                      // an open network?
    { "", 0, 0, 0 },                            // an open network?
};

static bool g_started_connecting = false;

static wifi_arr_t m_sorted_wifi_scan_list;
static int        m_wifi_retrieved_count;    // how many AP's from our scan list have been retrieved

// define UUIDs
static struct bt_uuid_128 tracker_service_uuid = BT_UUID_INIT_128(BT_UUID_TRACKER_SERVICE_VAL);

// single notification characteristic for service
static struct bt_uuid_128 service_notify_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x9d1589a6, 0xcea6, 0x4df1, 0x96d9, 0x1697cd4dc1e8));

static struct bt_uuid_128 device_info_rx_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x9d1589a6, 0xcea6, 0x4df1, 0x96d9, 0x1697cd4dc200));

static struct bt_uuid_128 set_device_info_tx_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x9d1589a6, 0xcea6, 0x4df1, 0x96d9, 0x1697cd4dc201));

static struct bt_uuid_128 set_ping_tx_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x9d1589a6, 0xcea6, 0x4df1, 0x96d9, 0x1697cd4dc202));

static struct bt_uuid_128 battery_info_rx_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x9d1589a6, 0xcea6, 0x4df1, 0x96d9, 0x1697cd4dc300));

static struct bt_uuid_128 modem_info_rx_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x9d1589a6, 0xcea6, 0x4df1, 0x96d9, 0x1697cd4dc400));

static struct bt_uuid_128 wifi_info_rx_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x9d1589a6, 0xcea6, 0x4df1, 0x96d9, 0x1697cd4dc500));

static struct bt_uuid_128 wifi_creds_tx_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x9d1589a6, 0xcea6, 0x4df1, 0x96d9, 0x1697cd4dc501));

static struct bt_uuid_128 wifi_creds_tx_clear_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x9d1589a6, 0xcea6, 0x4df1, 0x96d9, 0x1697cd4dc502));

static struct bt_uuid_128 wifi_scan_rx_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x9d1589a6, 0xcea6, 0x4df1, 0x96d9, 0x1697cd4dc600));

static struct bt_uuid_128 trigger_scan_tx_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x9d1589a6, 0xcea6, 0x4df1, 0x96d9, 0x1697cd4dc601));

static struct bt_uuid_128 dog_collar_info_rx_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x9d1589a6, 0xcea6, 0x4df1, 0x96d9, 0x1697cd4dc700));

static struct bt_uuid_128 dog_collar_info_tx_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x9d1589a6, 0xcea6, 0x4df1, 0x96d9, 0x1697cd4dc701));

static struct bt_uuid_128 led_ctrl_rx_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x9d1589a6, 0xcea6, 0x4df1, 0x96d9, 0x1697cd4dc800));

static struct bt_uuid_128 pairing_nonce_rx_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x9d1589a6, 0xcea6, 0x4df1, 0x96d9, 0x1697cd4dc900));

static int     charging_notify(char *status);
static int     wifi_scan_notify(int count);
static int     wifi_connect_fail_notify(char *status);
static int     wifi_connect_success_notify(char *ip_addr);
static ssize_t set_dog_collar_info(
    struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf, uint16_t len, uint16_t offset, uint8_t flags);
static int ping_notify(void);

static void notify_timer_handler(struct k_timer *dummy);
K_TIMER_DEFINE(notify_timer, notify_timer_handler, NULL);

static void notify_work_handler(struct k_work *work);
typedef struct notify_err_message
{
    struct k_work notify_work;
    char          notify_message[32];
} notify_message_t;
notify_message_t my_notify_message;

static void notify_work_handler(struct k_work *work)
{
    notify_message_t *notify_msg = CONTAINER_OF(work, notify_message_t, notify_work);
    LOG_DBG("EXTRACTED MESSAGE WAS %s", notify_msg->notify_message);
    if (da_state.ap_connected == 0) {
        wifi_connect_fail_notify(notify_msg->notify_message);
    } else if (da_state.ap_connected == 1) {
        wifi_connect_success_notify(notify_msg->notify_message);
    }
}

static void notify_timer_handler(struct k_timer *timer_obj_ptr)
{
    if (timer_obj_ptr->user_data != NULL) {
        LOG_DBG("MY MESSAGE IS %s", (char *)timer_obj_ptr->user_data);
        strncpy(
            my_notify_message.notify_message, (char *)timer_obj_ptr->user_data, sizeof(my_notify_message.notify_message));
    }
    k_work_submit(&my_notify_message.notify_work);
}

static ssize_t set_device_info(
    struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    LOG_HEXDUMP_DBG(buf, len, "DEVICE_INFO");
    bool                success;
    struct zcbor_string decoded_key;
    uint64_t            ts;

    /* Create zcbor state variable for decoding. */
    ZCBOR_STATE_D(decoding_state, 3, buf, len, 2);

    // Start decoding the map containing one key-value pair
    success = zcbor_map_start_decode(decoding_state);
    if (!success) {
        goto error;
    }

    success = zcbor_tstr_decode(decoding_state, &decoded_key);
    if (!success) {
        goto error;
    }

    LOG_DBG("Decoded key: '%.*s'", (int)decoded_key.len, decoded_key.value);

    if (strncmp(decoded_key.value, "UTC_TIME", strlen("UTC_TIME")) == 0) {
        // set time
        success = zcbor_uint64_decode(decoding_state, &ts);
        if (!success) {
            goto error;
        }
        LOG_DBG("Decoded time: %llu", ts);
        utils_set_currentmillis(ts);
    } else if (strncmp(decoded_key.value, "CLEAR_BONDING", strlen("CLEAR_BONDING")) == 0) {
        bool clear_bonding;
        success = zcbor_bool_decode(decoding_state, &clear_bonding);
        if (!success) {
            goto error;
        }
        if (clear_bonding) {
            LOG_INF("clearing BLE bonding info!");
            int err = bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
            if (err) {
                LOG_ERR("Unpairing failed (err %d)", err);
            }
        } else {
            LOG_ERR("unknown CLEAR_BONDING value!");
        }
    } else if (strncmp(decoded_key.value, "SET_ONBOARDED", strlen("SET_ONBOARDED")) == 0) {
        bool onboarded;
        success = zcbor_bool_decode(decoding_state, &onboarded);
        if (!success) {
            goto error;
        }
        LOG_INF("setting Onboarded flag to %d", onboarded);
        settings_save_one(ONBOARDED_SETTINGS_PATH, &onboarded, sizeof(onboarded));
    } else if (strncmp(decoded_key.value, "INITIATE_FOTA", strlen("INITIATE_FOTA")) == 0) {
        struct zcbor_string fota_type;
        success = zcbor_tstr_decode(decoding_state, &fota_type);
        if (!success) {
            goto error;
        }
        LOG_INF("Initiating FOTA for: %.*s", (int)fota_type.len, fota_type.value);
        if (!strncmp(fota_type.value, "ALL", fota_type.len)) {
            fota_update_all_devices();
        } else if (!strncmp(fota_type.value, "WIFI", fota_type.len)) {
            check_for_updates(true, COMM_DEVICE_DA16200);
        } else if (!strncmp(fota_type.value, "MODEM", fota_type.len)) {
            check_for_updates(true, COMM_DEVICE_NRF9160);
        } else if (!strncmp(fota_type.value, "APP_CPU", fota_type.len)) {
            check_for_updates(true, COMM_DEVICE_NRF5340);
        }
    } else if (strncmp(decoded_key.value, "SET_SAFEZONE", strlen("SET_SAFEZONE")) == 0) {
        struct zcbor_string ap_name;
        success = zcbor_tstr_decode(decoding_state, &ap_name);
        if (!success) {
            goto error;
        }
        LOG_INF("Setting safezone for: %.*s", (int)ap_name.len, ap_name.value);

        // TODO: set safezone here for ap_name.value (NOTE: not NULL terminated!)

    } else if (strncmp(decoded_key.value, "DELETE_SAFEZONE", strlen("DELETE_SAFEZONE")) == 0) {
        struct zcbor_string ap_name;
        success = zcbor_tstr_decode(decoding_state, &ap_name);
        if (!success) {
            goto error;
        }
        LOG_INF("Deleting safezone for: %.*s", (int)ap_name.len, ap_name.value);

        // TODO: delete safezone here for ap_name.value (NOTE: not NULL terminated!)
    }

    // End decoding the map
    success = zcbor_map_end_decode(decoding_state);
    if (!success) {
        goto error;
    }

    return len;

error:
    LOG_ERR("CBOR decoding failed: %d\r\n", zcbor_peek_error(decoding_state));
    return -1;
}

static char m_ap_ssid[33]      = { 0 };
static char m_ap_password[64]  = { 0 };
static char m_ap_sec_flags[64] = { 0 };

// helper to set the AP name and password
static void parse_wifi_creds(const char *key, const char *value)
{
    if (strncmp(key, "SSID", strlen("SSID")) == 0) {
        strncpy(m_ap_ssid, value, sizeof(m_ap_ssid));
        LOG_DBG("set ap ssid: %s", m_ap_ssid);
    }

    else if (strncmp(key, "PASSWD", strlen("PASSWD")) == 0) {
        strncpy(m_ap_password, value, sizeof(m_ap_password));
        LOG_DBG("set ap password: %s", m_ap_password);
    }

    else if (strncmp(key, "SEC_FLAGS", strlen("SEC_FLAGS")) == 0) {
        strncpy(m_ap_sec_flags, value, sizeof(m_ap_sec_flags));
        LOG_DBG("set ap security flags: %s", m_ap_sec_flags);
    }
}

// map the flags string to the connect enums
int map_flags_to_enum(char *flags, uint8_t *sec_protocol, uint8_t *enc_type)
{
    // defaults
    uint8_t protocol   = 3;    // WPA2
    uint8_t encryption = 1;    // AES

    int entries = sizeof(m_ap_flags_lookup) / sizeof(ApScanFlagsToEnumT);
    int i;
    for (i = 0; i < entries; i++) {
        if (strlen(m_ap_flags_lookup[i].flag_str) == strlen(flags)
            && !strncmp(m_ap_flags_lookup[i].flag_str, flags, strlen(m_ap_flags_lookup[i].flag_str))) {
            protocol   = m_ap_flags_lookup[i].security_protocol;
            encryption = m_ap_flags_lookup[i].encryption;
            LOG_DBG("%s: found protocol=%d, encryption=%d", flags, protocol, encryption);
            break;    // stop iterating over flag table
        }
    }
    if (i == entries) {
        LOG_WRN("%s: flag string not recognized!  using defaults for security flags", flags);
        // TODO: we should log this and/or send a notification to the
        // mobile app
    }
    *sec_protocol = protocol;
    *enc_type     = encryption;
    return 0;
}

// this helper takes a SSID, and looks for it in our list of saved SSID's.
// it then converts the AP's saved 'flags' string into enums we can use when
// calling the rm_connect_to_AP() API.
//
// If the SSID is not found, we return -1 and alert the app.
// if the 'flags' string isn't something we recognize (certainly could happen
// if this is a network type we haven't accounted for), then we assume WPA2 / AES
int map_ap_flags_to_enum(char *ssid, uint8_t *sec_protocol, uint8_t *enc_type)
{
    uint8_t protocol   = 0;
    uint8_t encryption = 0;

    int i;
    LOG_DBG("searching %d AP's", m_sorted_wifi_scan_list.count);
    for (i = 0; i < m_sorted_wifi_scan_list.count; i++) {
        wifi_obj_t *wifi_ap = &m_sorted_wifi_scan_list.wifi[i];
        LOG_DBG("comparing %s to %s", ssid, wifi_ap->ssid);

        // we found the AP
        if (strlen(ssid) == strlen(wifi_ap->ssid) && !strncmp(ssid, wifi_ap->ssid, strlen(wifi_ap->ssid))) {
            char *flags = wifi_ap->flags;
            map_flags_to_enum(flags, &protocol, &encryption);
            break;
        }
    }

    if (i == m_sorted_wifi_scan_list.count) {
        LOG_WRN("%s: unknown!", ssid);
        // we don't set args if unknown because we don't even attempt to connect
        return -1;
    }

    // set to values map_flags_to_enum() provided
    *sec_protocol = protocol;
    *enc_type     = encryption;
    return 0;
}

static ssize_t set_wifi_creds(
    struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    LOG_PANIC();    // flush
    LOG_HEXDUMP_DBG(buf, len, "WIFI_CONNECT");
    LOG_PANIC();    // flush
    bool success;

    // clear old SSID and password
    memset(&m_ap_ssid, 0, sizeof(m_ap_ssid));
    memset(&m_ap_password, 0, sizeof(m_ap_password));
    memset(&m_ap_sec_flags, 0, sizeof(m_ap_sec_flags));

    // Create zcbor state variable for decoding.
    ZCBOR_STATE_D(decoding_state, 3, buf, len, 6);

    // Start decoding the map containing two key-value pairs
    success = zcbor_map_start_decode(decoding_state);
    if (!success) {
        goto error;
    }

    // can contain up to 3 key/value pairs
    for (int i = 0; i < 3; i++) {
        struct zcbor_string decoded_key;
        struct zcbor_string decoded_value;
        char                null_terminated_value[64] = { 0 };

        success = zcbor_tstr_decode(decoding_state, &decoded_key);
        if (!success) {
            LOG_DBG("set_wifi_creds: no more elements");
            break;
        }
        LOG_DBG("Decoded key: '%.*s'", (int)decoded_key.len, decoded_key.value);

        // the value
        success = zcbor_tstr_decode(decoding_state, &decoded_value);
        if (!success) {
            goto error;
        }
        LOG_DBG("Decoded value: '%.*s'", (int)decoded_value.len, decoded_value.value);

        // note: decoded strings are not null terminated (!)
        strncpy(null_terminated_value, decoded_value.value, (int)decoded_value.len);

        // set either SSID or password
        parse_wifi_creds(decoded_key.value, null_terminated_value);
    }

    // End decoding the map
    success = zcbor_map_end_decode(decoding_state);
    if (!success) {
        goto error;
    }

    if (m_ap_ssid[0] != 0) {
        if (g_started_connecting) {
            LOG_ERR("We are already connecting to an AP");
            wifi_connect_fail_notify("CONN ALREADY IN PROGRESS");
            return -1;
        }
        // try to connect
        uint8_t sec_protocol = 0;
        uint8_t enc_type     = 0;
        int     ret;

        // if the wifi flags weren't passed in, we'll need to scan to obtain them,
        // then lookup the flags from our scan list (no need to sort here)
        if (m_ap_sec_flags[0] == 0) {
            LOG_DBG("flags not provided, scanning for %s", m_ap_ssid);
            // Scan for a new list if older then 10 seconds (Mike to confirm)

            ret = wifi_refresh_ssid_list(true, 10, K_MSEC(1500));
            if (ret != 0) {
                LOG_ERR("'%s'(%d) when scanning for AP's", wstrerr(-ret), ret);
                goto error;
            }
            wifi_arr_t *ssid_list = wifi_get_last_ssid_list();
            LOG_DBG("found %u AP's", ssid_list->count);
            if (ssid_list->count > 0 && ssid_list->count < 0xffff) {
                memcpy(&m_sorted_wifi_scan_list, ssid_list, sizeof(wifi_arr_t));
            } else {
                LOG_ERR("no AP's found!");
                wifi_connect_fail_notify("NO APs NOT FOUND IN SCAN");
                goto error;
            }

            ret = map_ap_flags_to_enum(m_ap_ssid, &sec_protocol, &enc_type);
            if (ret == -1) {
                wifi_connect_fail_notify("AP NOT FOUND IN SCAN");
                goto error;
            }
        } else {
            LOG_DBG("looking up flags: %s", m_ap_sec_flags);
            // if the flags were passed in, we can lookup the flags directly
            map_flags_to_enum(m_ap_sec_flags, &sec_protocol, &enc_type);
        }
        if (strlen(m_ap_password) < 8) {
            LOG_ERR("PASSWORD IS TOO SHORT(for sec 3, should be > 8");
            notify_timer.user_data = "PASSWORD TOO SHORT";
            k_timer_start(&notify_timer, K_SECONDS(2), K_SECONDS(0));
            return len;
        }
        ret = rm_connect_to_AP(m_ap_ssid, m_ap_password, sec_protocol, 0, enc_type);
        if (ret != 0) {
            switch (ret) {
            case -EAGAIN:
                LOG_ERR("We are already connecting to an AP");
                notify_timer.user_data = "CONN ALREADY IN PROGRESS";
                k_timer_start(&notify_timer, K_SECONDS(2), K_SECONDS(0));
                return len;
            case -EEXIST:
                LOG_ERR("AP creds exist");
                notify_timer.user_data = "AP creds exist";
                k_timer_start(&notify_timer, K_SECONDS(2), K_SECONDS(0));
                return len;
            default:
                LOG_ERR("Unknown connection failure %d", ret);
                notify_timer.user_data = "Unknown connection failure";
                k_timer_start(&notify_timer, K_SECONDS(2), K_SECONDS(0));
                return len;
            }
        }
        led_api_set_state(LED_WIFI_TRYING);

        g_started_connecting = true;

    } else {
        LOG_ERR("missing SSID or password");
        return -1;
    }

    return len;

error:
    LOG_ERR("CBOR decoding failed: %d\r\n", zcbor_peek_error(decoding_state));
    return -1;
}

// for a ping, we want to see this map: {"PING" : "REQUEST"}
// we'll send this response in a notification: {"PING" : "RESPONSE"}
static ssize_t
set_ping(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    LOG_HEXDUMP_DBG(buf, len, "PING");
    bool                success;
    struct zcbor_string decoded_key;

    /* Create zcbor state variable for decoding. */
    ZCBOR_STATE_D(decoding_state, 3, buf, len, 2);

    // Start decoding the map containing one key-value pair
    success = zcbor_map_start_decode(decoding_state);
    if (!success) {
        goto error;
    }

    success = zcbor_tstr_decode(decoding_state, &decoded_key);
    if (!success) {
        goto error;
    }

    LOG_DBG("Decoded key: '%.*s'", (int)decoded_key.len, decoded_key.value);

    if (strncmp(decoded_key.value, "PING", strlen("PING")) == 0) {
        struct zcbor_string ping_val;
        success = zcbor_tstr_decode(decoding_state, &ping_val);
        if (!success) {
            goto error;
        }
        if (!strncmp(ping_val.value, "REQUEST", ping_val.len)) {
            // send ping response as a notification
            ping_notify();
        } else {
            LOG_ERR("Invalid ping value: %s", ping_val.value);
            goto error;
        }
    } else {
        LOG_ERR("Invalid ping key: %s", decoded_key.value);
        goto error;
    }

    // End decoding the map
    success = zcbor_map_end_decode(decoding_state);
    if (!success) {
        goto error;
    }

    return len;

error:
    LOG_ERR("PING CBOR decoding failed: %d\r\n", zcbor_peek_error(decoding_state));
    return -1;
}

volatile bool notify_enable;

// Comparison function for qsort, to sort largest RSSI first.
// each argument points to a wifi_obj_t
static int compare_rssi(const void *a, const void *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }

    wifi_obj_t *obj_a = (wifi_obj_t *)a;
    wifi_obj_t *obj_b = (wifi_obj_t *)b;

    float rssi_a = obj_a->rssi;
    float rssi_b = obj_b->rssi;

    if (rssi_a > rssi_b) {
        return -1;
    } else if (rssi_a < rssi_b) {
        return 1;
    }
    return 0;
}

static ssize_t trigger_scan(
    struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    LOG_HEXDUMP_DBG(buf, len, "SCAN_TRIG");
    bool                success;
    struct zcbor_string decoded_key;

    /* Create zcbor state variable for decoding. */
    ZCBOR_STATE_D(decoding_state, 3, buf, len, 2);

    // Start decoding the map containing one key-value pair
    success = zcbor_map_start_decode(decoding_state);
    if (!success) {
        goto error;
    }

    success = zcbor_tstr_decode(decoding_state, &decoded_key);
    if (!success) {
        goto error;
    }

    // should be 'SCAN_TRIG'
    LOG_DBG("Decoded key: '%.*s'", (int)decoded_key.len, decoded_key.value);

    // value should be True
    bool scan_start;
    success = zcbor_bool_decode(decoding_state, &scan_start);
    if (!success) {
        goto error;
    }

    LOG_DBG("Decoded value: %d", scan_start);

    // End decoding the map
    success = zcbor_map_end_decode(decoding_state);
    if (!success) {
        goto error;
    }

    if ((strncmp(decoded_key.value, "SCAN_TRIG", strlen("SCAN_TRIG")) == 0) && scan_start) {
        LOG_DBG("starting WiFi scan");
        // Scan for a new list if older then 10 seconds (Mike to confirm)

        int ret = wifi_refresh_ssid_list(true, 10, K_MSEC(1500));
        if (ret != 0) {
            LOG_ERR("'%s'(%d) when scanning for AP's", wstrerr(-ret), ret);
            goto error;
        }
        wifi_arr_t *ssid_list = wifi_get_last_ssid_list();
        LOG_DBG("found %d AP's", ssid_list->count);
        if (ssid_list->count > 0) {
            memcpy(&m_sorted_wifi_scan_list, ssid_list, sizeof(wifi_arr_t));
            m_wifi_retrieved_count = 0;

            qsort(m_sorted_wifi_scan_list.wifi, m_sorted_wifi_scan_list.count, sizeof(wifi_obj_t), compare_rssi);
            LOG_DBG("sorted %d AP's", m_sorted_wifi_scan_list.count);
        }

#define DEBUG_SCAN_LIST (1)
#ifdef DEBUG_SCAN_LIST
        for (int i = 0; i < m_sorted_wifi_scan_list.count; i++) {
            wifi_obj_t *wifi_ap = &m_sorted_wifi_scan_list.wifi[i];
            LOG_DBG("%s: %s", wifi_ap->ssid, wifi_ap->flags);
        }
#endif

        // send notification when this returns with count
        wifi_scan_notify(m_sorted_wifi_scan_list.count);

    } else {
        LOG_WRN("SCAN_TRIG called with invalid params!");
        return -1;
    }

    return len;

error:
    LOG_ERR("CBOR decoding failed: %d\r\n", zcbor_peek_error(decoding_state));
    return -1;
}

static void ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    notify_enable = (value == BT_GATT_CCC_NOTIFY);
    LOG_DBG("Notification %s", notify_enable ? "enabled" : "disabled");
}

#define MAX_RESPONSE_LEN (4096)
typedef struct
{
    uint8_t  response[MAX_RESPONSE_LEN];
    uint16_t response_len;
} read_response_t;

static read_response_t read_response;

// called in response to reading from the 'DeviceInfo' characteristic
static int device_info_resp(void)
{
    uint8_t cbor_payload[256];
    bool    success;

    // Initialize ZCBOR encoding state
    ZCBOR_STATE_E(encoding_state, 1, cbor_payload, sizeof(cbor_payload), 0);

    // Start the map containing two key-value pairs
    success = zcbor_map_start_encode(encoding_state, 8);
    if (!success) {
        goto error;
    }

    // Add the version key
    success = zcbor_tstr_put_lit(encoding_state, "VERSION");
    if (!success) {
        goto error;
    }

    // Add the version value
    char version_str[16];
    sprintf(version_str, "%d.%d.%d", APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_VERSION_PATCH);
    success = zcbor_tstr_put_term(encoding_state, version_str);
    if (!success) {
        goto error;
    }

    // Add the status key
    success = zcbor_tstr_put_lit(encoding_state, "STATUS");
    if (!success) {
        goto error;
    }

    // Add the self-check tatus value
    char              status_str[8] = "PASS";
    self_check_enum_t status_code   = self_check();
    if (status_code != SELF_CHECK_PASS) {
        memset(status_str, 0, sizeof(status_str));
        strcpy(status_str, "FAIL");
        // TODO: we could append status code or add another map containing it
    }

    success = zcbor_tstr_put_term(encoding_state, status_str);
    if (!success) {
        goto error;
    }

    // Add the serial number key
    success = zcbor_tstr_put_lit(encoding_state, "SERIAL_NUM");
    if (!success) {
        goto error;
    }

    // Add the serial number value
    success = zcbor_tstr_put_term(encoding_state, uicr_serial_number_get());
    if (!success) {
        goto error;
    }

    // Add the current time key
    success = zcbor_tstr_put_lit(encoding_state, "UTC_TIME");
    if (!success) {
        goto error;
    }

    // Add the current time value
    uint64_t time = utils_get_currentmillis();
    success       = zcbor_uint64_put(encoding_state, time);
    if (!success) {
        goto error;
    }

    // Finalize the map
    success = zcbor_map_end_encode(encoding_state, 6);
    if (!success) {
        goto error;
    }

    size_t cbor_len = encoding_state->payload - cbor_payload;
    memcpy(read_response.response, cbor_payload, cbor_len);
    read_response.response_len = cbor_len;
    return 0;

error:
    LOG_ERR("CBOR encoding failed: %d\r\n", zcbor_peek_error(encoding_state));
    return -1;
}

// called in response to reading from the 'BatteryInfo' characteristic
static int battery_info_resp(void)
{
    uint8_t cbor_payload[256];
    bool    success;

    // Initialize ZCBOR encoding state
    ZCBOR_STATE_E(encoding_state, 1, cbor_payload, sizeof(cbor_payload), 0);

    // Start the map containing two key-value pairs
    success = zcbor_map_start_encode(encoding_state, 4);
    if (!success) {
        goto error;
    }

    fuel_gauge_info_t fuel;

    if (fuel_gauge_get_latest(&fuel) != 0) {
        LOG_ERR("Failed to get fuel gauge info");
        fuel.soc = 0;
    }
    LOG_DBG("fuel guage soc: %.2f", fuel.soc);

    // Add the SOC key
    success = zcbor_tstr_put_lit(encoding_state, "SOC");
    if (!success) {
        goto error;
    }

    // Add the SOC value
    int soc = (int)fuel.soc;
    if (soc > 100) {
        soc = 100;
    }
    success = zcbor_uint32_put(encoding_state, soc);
    if (!success) {
        goto error;
    }

    // Add the charging status key
    success = zcbor_tstr_put_lit(encoding_state, "CHARGING_STATUS");
    if (!success) {
        goto error;
    }

    // Add the charging status value
    bool  charging     = get_charging_active();
    char *charging_str = "CHARGING";
    if (!charging) {
        if (soc >= 100) {
            charging_str = "CHARGED";
        } else {
            charging_str = "DISCHARGING";
        }
    }

    success = zcbor_tstr_put_term(encoding_state, charging_str);
    if (!success) {
        goto error;
    }

    // Finalize the map
    success = zcbor_map_end_encode(encoding_state, 4);
    if (!success) {
        goto error;
    }

    size_t cbor_len = encoding_state->payload - cbor_payload;
    memcpy(read_response.response, cbor_payload, cbor_len);
    read_response.response_len = cbor_len;

    return 0;

error:
    LOG_ERR("CBOR encoding failed: %d\r\n", zcbor_peek_error(encoding_state));
    return -1;
}

// called in response to reading from the 'Modem Info' characteristic
static int modem_info_resp(void)
{
    LOG_DBG("getting modem info...");
    uint8_t cbor_payload[256];
    bool    success;
    // Initialize ZCBOR encoding state
    ZCBOR_STATE_E(encoding_state, 1, cbor_payload, sizeof(cbor_payload), 0);

    // Start the map containing 3 key-value pairs
    success = zcbor_map_start_encode(encoding_state, 8);
    if (!success) {
        goto error;
    }

    char iccid_str[50] = "UNKNOWN";
    char imei_str[50]  = "UNKNOWN";

    int err = modem_get_IMEI(imei_str, sizeof(imei_str));
    if (err) {
        LOG_WRN("Couldn't get IMEI");
    } else {
        iccid_str[19] = '\0';
    }
    err = modem_get_ICCID(iccid_str, sizeof(iccid_str));
    if (err) {
        LOG_WRN("Couldn't get IMEI");
    } else {
        imei_str[15] = '\0';
    }

    // Add the ICCID key
    success = zcbor_tstr_put_lit(encoding_state, "ICCID");
    if (!success) {
        goto error;
    }

    // Add the ICCID value
    success = zcbor_tstr_put_term(encoding_state, iccid_str);
    if (!success) {
        goto error;
    }

    // Add the IMEI key
    success = zcbor_tstr_put_lit(encoding_state, "IMEI");
    if (!success) {
        goto error;
    }

    // Add the IMEI value
    success = zcbor_tstr_put_term(encoding_state, imei_str);
    if (!success) {
        goto error;
    }

    // Add the  version key
    success = zcbor_tstr_put_lit(encoding_state, "VERSION");
    if (!success) {
        goto error;
    }

    char               version_str[16] = "UNKNOWN";
    version_response_t version;
    if (modem_get_version(&version) == 0) {
        memset(version_str, 0, sizeof(version_str));
        sprintf(version_str, "%d.%d.%d", version.major, version.minor, version.patch);
    }

    // Add the version value
    success = zcbor_tstr_put_term(encoding_state, version_str);
    if (!success) {
        goto error;
    }

    // Add the  status key
    success = zcbor_tstr_put_lit(encoding_state, "LTE_STATUS");
    if (!success) {
        goto error;
    }

    // Add the status value
    char           status_str[16] = "UNKNOWN";
    modem_status_t status;
    if (modem_get_status(&status) == 0) {
        memset(status_str, 0, sizeof(status_str));
        bool connected = ((status.status_flags >> STATUS_LTE_CONNECTED) & 1);
        if (connected) {
            strcpy(status_str, "CONNECTED");
        } else {
            strcpy(status_str, "DISCONNECTED");
        }
    }

    success = zcbor_tstr_put_term(encoding_state, status_str);
    if (!success) {
        goto error;
    }
    // Finalize the map
    success = zcbor_map_end_encode(encoding_state, 8);
    if (!success) {
        goto error;
    }

    size_t cbor_len = encoding_state->payload - cbor_payload;
    memcpy(read_response.response, cbor_payload, cbor_len);
    read_response.response_len = cbor_len;

    return 0;

error:
    LOG_ERR("CBOR encoding failed: %d\r\n", zcbor_peek_error(encoding_state));
    return -1;
}

// called in response to reading from the 'WiFi Info' characteristic
static int wifi_info_resp(void)
{
    LOG_DBG("getting wifi info...");
    uint8_t cbor_payload[256];
    bool    success;
    // Initialize ZCBOR encoding state
    ZCBOR_STATE_E(encoding_state, 1, cbor_payload, sizeof(cbor_payload), 0);

    // Start the map
    success = zcbor_map_start_encode(encoding_state, 8);
    if (!success) {
        goto error;
    }
    // Add the MACADDR key
    success = zcbor_tstr_put_lit(encoding_state, "MACADDR");
    if (!success) {
        goto error;
    }

    // Add the MACADDR value
    char *macaddr = uicr_wifi_mac_address_get();
    if (macaddr[0] == 0xff) {
        // it's not set!
        macaddr = "NOT SET";
    }
    success = zcbor_tstr_put_term(encoding_state, macaddr);
    if (!success) {
        goto error;
    }

    // Add the version key
    success = zcbor_tstr_put_lit(encoding_state, "VERSION");
    if (!success) {
        goto error;
    }

    // Add the WiFi FW version value
    char version[60] = { 0 };
    wifi_get_da_fw_ver(version, 60, K_MSEC(1000));
    success = zcbor_tstr_put_term(encoding_state, version);
    if (!success) {
        goto error;
    }

    // Add the WiFi connection status key
    success = zcbor_tstr_put_lit(encoding_state, "STATUS");
    if (!success) {
        goto error;
    }
    char connection_status_ssid[256];
    // Add the WiFi connection status value
    char *connection_status_str = "UNKNOWN";
    if (da_state.ap_connected == 0) {
        connection_status_str = "DISCONNECTED";
    } else if (da_state.ap_connected == 1) {
        snprintf(connection_status_ssid, sizeof(connection_status_ssid), "CONNECTED TO %s", da_state.ap_name);
        connection_status_str = connection_status_ssid;
    }

    success = zcbor_tstr_put_term(encoding_state, connection_status_str);
    if (!success) {
        goto error;
    }

    // Finalize the map
    success = zcbor_map_end_encode(encoding_state, 8);
    if (!success) {
        goto error;
    }

    size_t cbor_len = encoding_state->payload - cbor_payload;
    memcpy(read_response.response, cbor_payload, cbor_len);
    read_response.response_len = cbor_len;

    return 0;

error:
    LOG_ERR("CBOR encoding failed: %d\r\n", zcbor_peek_error(encoding_state));
    return -1;
}

// called in response to reading from the 'Pairing Nonce' characteristic
static int pairing_nonce_resp(void)
{
    LOG_DBG("getting pairing nonce...");
    uint8_t cbor_payload[256];
    bool    success;
    // Initialize ZCBOR encoding state
    ZCBOR_STATE_E(encoding_state, 1, cbor_payload, sizeof(cbor_payload), 0);

    // Start the map
    success = zcbor_map_start_encode(encoding_state, 2);
    if (!success) {
        goto error;
    }
    // Add the NONCE key
    success = zcbor_tstr_put_lit(encoding_state, "NONCE");
    if (!success) {
        goto error;
    }

    char nonce[16] = { 0 };

    // NOTE: not cryptographically secure
    sys_rand_get(nonce, sizeof(nonce));

    // FIXME: get 'factory reset count' and overwrite first 4 bytes of nonce with it
    // in the meantime just hardcode it to 1
    uint8_t factory_reset_count = 1;
    nonce[0]                    = 0;
    nonce[1]                    = 0;
    nonce[2]                    = 0;
    nonce[3]                    = factory_reset_count;

    // Add the NONCE value
    success = zcbor_bstr_put_arr(encoding_state, nonce);
    if (!success) {
        goto error;
    }

    // Finalize the map
    success = zcbor_map_end_encode(encoding_state, 8);
    if (!success) {
        goto error;
    }

    // convert nonce to a null-terminated string and send to backend
    char nonce_str[sizeof(nonce) * 2 + 1] = { 0 };
    for (int i = 0; i < sizeof(nonce); i++) {
        sprintf(&nonce_str[i * 2], "%02x", (unsigned char)nonce[i]);
    }

    commMgr_queue_pairing_nonce(nonce_str);

    size_t cbor_len = encoding_state->payload - cbor_payload;
    memcpy(read_response.response, cbor_payload, cbor_len);
    read_response.response_len = cbor_len;

    return 0;

error:
    LOG_ERR("CBOR encoding failed: %d\r\n", zcbor_peek_error(encoding_state));
    return -1;
}

// called in response to reading from the 'WiFi Scan' characteristic
static int read_next_wifi_ap_info(uint16_t len, uint16_t offset)
{
    // point to next AP to retrieve
    wifi_obj_t *wifi_ap = &m_sorted_wifi_scan_list.wifi[m_wifi_retrieved_count];
    uint8_t     ssid_cbor_payload[256];
    bool        success;

    // Initialize ZCBOR encoding state
    ZCBOR_STATE_E(encoding_state, 4, ssid_cbor_payload, sizeof(ssid_cbor_payload), 0);

    // Start the outer map containing one key-value pair
    // Key is WIFI_AP_N, value is a map containing AP info
    success = zcbor_map_start_encode(encoding_state, 1);
    if (!success) {
        goto error;
    }

    if (m_wifi_retrieved_count == m_sorted_wifi_scan_list.count) {
        // all wifi ap's have been retrieved - return an empty object

        LOG_DBG("no more wifi AP's to retrieve!");
        success = zcbor_map_end_encode(encoding_state, 10);
        if (!success) {
            goto error;
        }

        size_t cbor_len = encoding_state->payload - ssid_cbor_payload;
        memcpy(read_response.response, ssid_cbor_payload, cbor_len);
        read_response.response_len = cbor_len;
        return 0;
    }

    char ap_key[16] = { 0 };
    sprintf(ap_key, "WIFI_AP_%d", m_wifi_retrieved_count);

    // Add the key "WIFI_AP_N"
    success = zcbor_tstr_put_term(encoding_state, ap_key);
    if (!success) {
        goto error;
    }

    // start of value (a map) for this AP
    success = zcbor_map_start_encode(encoding_state, 6);
    if (!success) {
        goto error;
    }

    // Add the key-value pair "NAME"
    success = zcbor_tstr_put_lit(encoding_state, "NAME");
    if (!success) {
        goto error;
    }

    success = zcbor_tstr_put_term(encoding_state, wifi_ap->ssid);
    if (!success) {
        goto error;
    }

    // Add the key-value pair "RSSI"
    success = zcbor_tstr_put_lit(encoding_state, "RSSI");
    if (!success) {
        goto error;
    }

    int32_t rssi = (int)wifi_ap->rssi;
    success      = zcbor_int32_put(encoding_state, rssi);
    if (!success) {
        goto error;
    }

    // Add the key-value pair "SEC_FLAGS"
    success = zcbor_tstr_put_lit(encoding_state, "SEC_FLAGS");
    if (!success) {
        goto error;
    }

    // Add the flags string (informational purposes only)
    success = zcbor_tstr_put_term(encoding_state, wifi_ap->flags);
    if (!success) {
        goto error;
    }

    // Finalize inner map for this AP
    success = zcbor_map_end_encode(encoding_state, 10);
    if (!success) {
        goto error;
    }

    // Finalize outer map
    success = zcbor_map_end_encode(encoding_state, 1);
    if (!success) {
        goto error;
    }

    size_t cbor_len = encoding_state->payload - ssid_cbor_payload;
    memcpy(read_response.response, ssid_cbor_payload, cbor_len);
    read_response.response_len = cbor_len;

    if ((offset + len) >= cbor_len) {
        LOG_DBG("fully retrieved AP %d", m_wifi_retrieved_count);
        m_wifi_retrieved_count++;    // mark this one as retrieved
    }

    return 0;

error:
    LOG_ERR("CBOR encoding failed: %d\r\n", zcbor_peek_error(encoding_state));
    return -1;
}

// called when central wants to read from the read characteristic
static ssize_t
read_from_central(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len, uint16_t offset)
{
    char str[BT_UUID_STR_LEN];
    bt_uuid_to_str(attr->uuid, str, sizeof(str));

    if (!bt_uuid_cmp(attr->uuid, &device_info_rx_uuid.uuid)) {
        device_info_resp();
    } else if (!bt_uuid_cmp(attr->uuid, &battery_info_rx_uuid.uuid)) {
        battery_info_resp();
    } else if (!bt_uuid_cmp(attr->uuid, &modem_info_rx_uuid.uuid)) {
        modem_info_resp();
    } else if (!bt_uuid_cmp(attr->uuid, &wifi_info_rx_uuid.uuid)) {
        wifi_info_resp();
    } else if (!bt_uuid_cmp(attr->uuid, &wifi_scan_rx_uuid.uuid)) {
        read_next_wifi_ap_info(len, offset);
    } else if (!bt_uuid_cmp(attr->uuid, &pairing_nonce_rx_uuid.uuid)) {
        pairing_nonce_resp();
    }

    LOG_DBG("reading %d bytes from uuid: %s", read_response.response_len, str);
    LOG_DBG("len: %d, offset: %d", len, offset);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, read_response.response, read_response.response_len);

    memset(read_response.response, 0, MAX_RESPONSE_LEN);
    read_response.response_len = 0;
}

// define service
BT_GATT_SERVICE_DEFINE(
    tracker_service,
    BT_GATT_PRIMARY_SERVICE(&tracker_service_uuid),

    // notify
    BT_GATT_CHARACTERISTIC(&service_notify_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ, NULL, NULL, NULL),

    // notify ccc
    BT_GATT_CCC(ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

    // rx - read Device Info from host
    BT_GATT_CHARACTERISTIC(&device_info_rx_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_from_central, NULL, NULL),

    // tx - set time from host
    BT_GATT_CHARACTERISTIC(
        &set_device_info_tx_uuid.uuid, BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE, NULL, set_device_info, NULL),

    // tx - set ping from host (via encrypted characteristic)
    BT_GATT_CHARACTERISTIC(&set_ping_tx_uuid.uuid, BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE_ENCRYPT, NULL, set_ping, NULL),

    // rx - read Battery Info from host
    BT_GATT_CHARACTERISTIC(&battery_info_rx_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_from_central, NULL, NULL),

    // rx - read Modem Info from host
    BT_GATT_CHARACTERISTIC(&modem_info_rx_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_from_central, NULL, NULL),

    // rx - read WiFi Info from host
    BT_GATT_CHARACTERISTIC(&wifi_info_rx_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_from_central, NULL, NULL),

    // tx - set WiFi credentials from host
    BT_GATT_CHARACTERISTIC(
        &wifi_creds_tx_uuid.uuid, BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE_ENCRYPT, NULL, set_wifi_creds, NULL),

    // tx - set WiFi credentials from host (in the clear)
    BT_GATT_CHARACTERISTIC(
        &wifi_creds_tx_clear_uuid.uuid, BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE, NULL, set_wifi_creds, NULL),

    // rx - read WiFi scan list from host
    BT_GATT_CHARACTERISTIC(&wifi_scan_rx_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_from_central, NULL, NULL),

    // tx - trigger wifi scan from host
    BT_GATT_CHARACTERISTIC(&trigger_scan_tx_uuid.uuid, BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE, NULL, trigger_scan, NULL),

    // rx - read Dog Collar Info from host
    BT_GATT_CHARACTERISTIC(
        &dog_collar_info_rx_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_from_central, NULL, NULL),

    // tx - set Dog Collar Info from host
    BT_GATT_CHARACTERISTIC(
        &dog_collar_info_tx_uuid.uuid, BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE, NULL, set_dog_collar_info, NULL),

    // rx - read LED state from host
    BT_GATT_CHARACTERISTIC(&led_ctrl_rx_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_from_central, NULL, NULL),

    // rx - read pairing Nonce
    BT_GATT_CHARACTERISTIC(&pairing_nonce_rx_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_from_central, NULL, NULL),

);

static int charging_notify(char *status)
{
    if (notify_enable) {

        uint8_t cbor_payload[256];
        bool    success;

        // Initialize ZCBOR encoding state
        ZCBOR_STATE_E(encoding_state, 1, cbor_payload, sizeof(cbor_payload), 0);

        // one key-value pair
        success = zcbor_map_start_encode(encoding_state, 2);
        if (!success) {
            goto error;
        }

        // Add the charging status key
        success = zcbor_tstr_put_lit(encoding_state, "CHARGING_STATUS");
        if (!success) {
            goto error;
        }

        // Add the charging status value
        success = zcbor_tstr_put_term(encoding_state, status);
        if (!success) {
            goto error;
        }

        // Finalize the map
        success = zcbor_map_end_encode(encoding_state, 2);
        if (!success) {
            goto error;
        }

        size_t cbor_len = encoding_state->payload - cbor_payload;

        struct bt_conn *conn = ble_get_conn();
        bt_gatt_notify(conn, &tracker_service.attrs[1], cbor_payload, cbor_len);
        return 0;

    error:
        LOG_ERR("CBOR encoding failed: %d\r\n", zcbor_peek_error(encoding_state));
        return -1;
    }

    return 0;
}

static int wifi_connect_success_notify(char *ip_addr)
{
    if (notify_enable) {

        uint8_t cbor_payload[256];
        bool    success;

        // Initialize ZCBOR encoding state
        ZCBOR_STATE_E(encoding_state, 1, cbor_payload, sizeof(cbor_payload), 0);

        // three key-value pairs
        success = zcbor_map_start_encode(encoding_state, 6);
        if (!success) {
            goto error;
        }

        // Add the wifi status key
        success = zcbor_tstr_put_lit(encoding_state, "WIFI_STATUS");
        if (!success) {
            goto error;
        }

        // Add the wifi status value
        success = zcbor_tstr_put_lit(encoding_state, "CONNECT_SUCCESS");
        if (!success) {
            goto error;
        }

        // Add the ip addr key
        success = zcbor_tstr_put_lit(encoding_state, "IP_ADDR");
        if (!success) {
            goto error;
        }

        // Add the ip addr value
        success = zcbor_tstr_put_term(encoding_state, ip_addr);
        if (!success) {
            goto error;
        }

        // Finalize the map
        success = zcbor_map_end_encode(encoding_state, 4);
        if (!success) {
            goto error;
        }

        size_t cbor_len = encoding_state->payload - cbor_payload;

        struct bt_conn *conn = ble_get_conn();
        bt_gatt_notify(conn, &tracker_service.attrs[1], cbor_payload, cbor_len);
        return 0;

    error:
        LOG_ERR("CBOR encoding failed: %d\r\n", zcbor_peek_error(encoding_state));
        return -1;
    }

    return 0;
}

int fota_notify(char *fota_state)
{
    if (notify_enable) {

        uint8_t cbor_payload[256];
        bool    success;

        // Initialize ZCBOR encoding state
        ZCBOR_STATE_E(encoding_state, 1, cbor_payload, sizeof(cbor_payload), 0);

        // two key-value pairs
        success = zcbor_map_start_encode(encoding_state, 2);
        if (!success) {
            goto error;
        }

        // Add the wifi status key
        success = zcbor_tstr_put_lit(encoding_state, "FOTA_STATUS");
        if (!success) {
            goto error;
        }

        // Add the wifi status value
        success = zcbor_tstr_put_term(encoding_state, fota_state);
        if (!success) {
            goto error;
        }

        // Finalize the map
        success = zcbor_map_end_encode(encoding_state, 2);
        if (!success) {
            goto error;
        }

        size_t cbor_len = encoding_state->payload - cbor_payload;

        struct bt_conn *conn = ble_get_conn();
        bt_gatt_notify(conn, &tracker_service.attrs[1], cbor_payload, cbor_len);
        return 0;

    error:
        LOG_ERR("CBOR encoding failed: %d\r\n", zcbor_peek_error(encoding_state));
        return -1;
    }

    return 0;
}

static int wifi_connect_fail_notify(char *status)
{
    if (notify_enable) {

        uint8_t cbor_payload[256];
        bool    success;

        // Initialize ZCBOR encoding state
        ZCBOR_STATE_E(encoding_state, 1, cbor_payload, sizeof(cbor_payload), 0);

        // two key-value pairs
        success = zcbor_map_start_encode(encoding_state, 4);
        if (!success) {
            goto error;
        }

        // Add the wifi status key
        success = zcbor_tstr_put_lit(encoding_state, "WIFI_STATUS");
        if (!success) {
            goto error;
        }

        // Add failure status
        success = zcbor_tstr_put_lit(encoding_state, "CONNECT_FAILURE");
        if (!success) {
            goto error;
        }

        // Add the error key
        success = zcbor_tstr_put_lit(encoding_state, "ERROR");
        if (!success) {
            goto error;
        }

        // Add the error value
        success = zcbor_tstr_put_term(encoding_state, status);
        if (!success) {
            goto error;
        }

        // Finalize the map
        success = zcbor_map_end_encode(encoding_state, 4);
        if (!success) {
            goto error;
        }

        size_t cbor_len = encoding_state->payload - cbor_payload;

        struct bt_conn *conn = ble_get_conn();
        bt_gatt_notify(conn, &tracker_service.attrs[1], cbor_payload, cbor_len);
        return 0;

    error:
        LOG_ERR("CBOR encoding failed: %d\r\n", zcbor_peek_error(encoding_state));
        return -1;
    }

    return 0;
}
static int wifi_scan_notify(int count)
{
    if (notify_enable) {

        uint8_t cbor_payload[256];
        bool    success;

        // Initialize ZCBOR encoding state
        ZCBOR_STATE_E(encoding_state, 1, cbor_payload, sizeof(cbor_payload), 0);

        // one key-value pair
        success = zcbor_map_start_encode(encoding_state, 2);
        if (!success) {
            goto error;
        }

        // Add the scan count key
        success = zcbor_tstr_put_lit(encoding_state, "SCAN_COUNT");
        if (!success) {
            goto error;
        }

        // Add the scan count value
        success = zcbor_uint32_put(encoding_state, count);
        if (!success) {
            goto error;
        }

        // Finalize the map
        success = zcbor_map_end_encode(encoding_state, 2);
        if (!success) {
            goto error;
        }

        size_t cbor_len = encoding_state->payload - cbor_payload;

        struct bt_conn *conn = ble_get_conn();
        bt_gatt_notify(conn, &tracker_service.attrs[1], cbor_payload, cbor_len);
        return 0;

    error:
        LOG_ERR("CBOR encoding failed: %d\r\n", zcbor_peek_error(encoding_state));
        return -1;
    }

    return 0;
}

// send a ping response notification
static int ping_notify(void)
{
    if (notify_enable) {

        uint8_t cbor_payload[256];
        bool    success;

        // Initialize ZCBOR encoding state
        ZCBOR_STATE_E(encoding_state, 1, cbor_payload, sizeof(cbor_payload), 0);

        // one key-value pair
        success = zcbor_map_start_encode(encoding_state, 2);
        if (!success) {
            goto error;
        }

        // Add the ping response key
        success = zcbor_tstr_put_lit(encoding_state, "PING");
        if (!success) {
            goto error;
        }

        // Add the ping response value
        success = zcbor_tstr_put_lit(encoding_state, "RESPONSE");
        if (!success) {
            goto error;
        }

        // Finalize the map
        success = zcbor_map_end_encode(encoding_state, 2);
        if (!success) {
            goto error;
        }

        size_t cbor_len = encoding_state->payload - cbor_payload;

        struct bt_conn *conn = ble_get_conn();
        bt_gatt_notify(conn, &tracker_service.attrs[1], cbor_payload, cbor_len);
        return 0;

    error:
        LOG_ERR("CBOR encoding failed: %d\r\n", zcbor_peek_error(encoding_state));
        return -1;
    }

    return 0;
}

static int charging_cb(pmic_state_t new_state)
{
    fuel_gauge_info_t fuel;

    if (fuel_gauge_get_latest(&fuel) != 0) {
        LOG_ERR("Failed to get fuel gauge info");
        fuel.soc = 0;
    }

    if (new_state == PMIC_CHARGING_STARTED) {
        charging_notify("CHARGING");
    } else {
        if (fuel.soc >= 100) {
            charging_notify("CHARGED");
        } else {
            charging_notify("DISCHARGING");
        }
    }
    return 0;
}

K_THREAD_STACK_DEFINE(modem_info_stack, 2048);

static void strip_crlf(char *str)
{
    int i, j;
    int len = strlen(str);
    for (i = 0, j = 0; i < len; i++, j++) {
        // Copy over characters unless they are CR or LF
        if (str[i] == '\r' || str[i] == '\n') {
            j--;    // Decrement j to overwrite CR or LF
        } else {
            str[j] = str[i];
        }
    }
    str[j] = '\0';    // Null-terminate the modified string
}

// This will be called whenever we receive a message from the DA16200
void wifi_cb(wifi_msg_t *msg, void *user_data)
{
    if (msg->incoming == false) {
        // outgoing message
        return;
    }
    if (strcmp(msg->data, "OK") == 0 || strcmp(msg->data, "+INIT") == 0) {
        // ignore 'OK', 'INIT', 'WFSCAN' messages
        return;
    }

    if (strstr(msg->data, "\r\n+WFJAP") && g_started_connecting) {
// these are relatively smaller messages
#define MAX_SFJAP_LEN (128)
        char str[MAX_SFJAP_LEN] = { 0 };
        memcpy(str, msg->data, MAX_SFJAP_LEN - 1);

        // strip CRLF to make it easier to format
        strip_crlf(str);
        LOG_DBG("%s", str);

        // EAS removed code checking for outgoing cmds, see above
        char at_cmd[10];
        char ssid[32];
        char ip_addr[16];

        // successful connect looks like: +WFJAP:1,'MY_APS_SSID',192.168.0.3
        int parsed = sscanf(str, "+%[^:]:%*d,'%[^']',%[^ ]", at_cmd, ssid, ip_addr);
        if (parsed == 3) {
            LOG_DBG("AT Command: %s", at_cmd);
            LOG_DBG("SSID: %s", ssid);
            LOG_DBG("IP Address: %s", ip_addr);
            wifi_connect_success_notify(ip_addr);
            led_api_set_state(LED_API_IDLE);
        } else {
            // not a successful connection, looks like +WFJAP:0, look for failure code
            char status[32];
            int  code;
            int  parsed = sscanf(str, "+%[^:]:%d,%s", at_cmd, &code, status);
            if (parsed == 3) {
                LOG_DBG("fail status: %s", status);
                // led_api_set_state(LED_API_ERROR);
                wifi_connect_fail_notify(status);
            } else {
                LOG_ERR("can't parse response");
            }
        }
        g_started_connecting = false;
    }

    return;
}

int tracker_service_init(void)
{
    pmic_state_set_callback(charging_cb);
    wifi_add_tx_rx_cb(wifi_cb, NULL);
    k_work_init(&my_notify_message.notify_work, notify_work_handler);

#ifdef ENABLE_MODEM_INTERFACE
    static struct k_work_q modem_info_work_q;
    k_work_queue_start(
        &modem_info_work_q,
        modem_info_stack,
        K_THREAD_STACK_SIZEOF(modem_info_stack),
        CONFIG_SYSTEM_WORKQUEUE_PRIORITY,
        NULL);

    k_work_submit_to_queue(&modem_info_work_q, &modem_info_work);
#endif

    LOG_INF("Onboarded: %d", tracker_service_is_onboarded());

    return 0;
}

// helper to set the dog info
static void parse_dog_info(const char *key, int value)
{
    if (strncmp(key, "DOG_SIZE", strlen("DOG_SIZE")) == 0) {
        LOG_DBG("dog size: %d", value);
        // TODO: save this in settings
    }

    else if (strncmp(key, "COLLAR_POS", strlen("COLLAR_POS")) == 0) {
        LOG_DBG("collar position: %d", value);
        // TODO: save this in settings
    }
}

static ssize_t set_dog_collar_info(
    struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    LOG_HEXDUMP_DBG(buf, len, "DOG_INFO");
    bool success;

    /* Create zcbor state variable for decoding. */
    ZCBOR_STATE_D(decoding_state, 3, buf, len, 4);

    // Start decoding the map containing two key-value pairs
    success = zcbor_map_start_decode(decoding_state);
    if (!success) {
        goto error;
    }

    // should contain 2 key/value pairs
    for (int i = 0; i < 2; i++) {
        struct zcbor_string decoded_key;

        success = zcbor_tstr_decode(decoding_state, &decoded_key);
        if (!success) {
            goto error;
        }
        LOG_DBG("Decoded key: '%.*s'", (int)decoded_key.len, decoded_key.value);

        // the value
        uint32_t value;
        success = zcbor_uint32_decode(decoding_state, &value);
        if (!success) {
            goto error;
        }

        // set either DOG_SIZE or COLLAR_POS
        parse_dog_info(decoded_key.value, value);
    }

    // End decoding the map
    success = zcbor_map_end_decode(decoding_state);
    if (!success) {
        goto error;
    }

    return len;

error:
    LOG_ERR("CBOR decoding failed: %d\r\n", zcbor_peek_error(decoding_state));
    return -1;
}

bool tracker_service_is_onboarded(void)
{
    bool onboarded = false;
    int  rc        = utils_load_setting(ONBOARDED_SETTINGS_PATH, &onboarded, sizeof(onboarded));
    if (rc == -ENOENT) {
        LOG_ERR("can't load onboarded flag!");
    }
    return onboarded;
}
