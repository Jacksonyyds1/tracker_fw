/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
/*
 *
 * SPDX-License-Identifier: LicenseRef-Proprietary
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/types.h>
#include <stdio.h>
#include "d1_zbus.h"
#include "pmic_leds.h"

#include "ble.h"
#include "tracker_service.h"
#include "uicr.h"

LOG_MODULE_REGISTER(ble, CONFIG_BLE_LOG_LEVEL);

static struct bt_conn *current_conn;
static struct bt_conn *auth_conn;

static uint16_t company_id = 0x2502;    // TODO: this is Nestlé Nespresso

// Note: 'Petivity Dog Tracker AABBCC' is 27 characters.
// A BLE advertisement is 31 bytes; the flags require 3 bytes, leaving 28.
// The type and length for the name require 2 bytes, leaving 26 bytes
// for the actual name string.  So we shortened the name to: 'Petivity Tracker AABBCC'
//  includes null for sprintf
static char local_name_str[27];

#define BLE_ADV_TIME 600
static bool defer_ble_stop = false;
void        ble_adv_timeout_worker(struct k_work *work)
{
    LOG_DBG("Turning off BLE");
    // if in a connection, dont disconnect
    if (current_conn == NULL) {
        bt_le_adv_stop();
        bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_POWER_OFF);
        LOG_ERR("Disconnecting at %ds timeout", BLE_ADV_TIME);
        if (uicr_shipping_flag_get()) {
            led_api_set_state(LED_BLE_ADV_DONE);
        }
        defer_ble_stop = false;
    } else {
        LOG_ERR("Not disconnecting since we are in a connection");
        defer_ble_stop = true;
    }
}
K_WORK_DEFINE(ble_adv_work, ble_adv_timeout_worker);
void ble_adv_timer_handler(struct k_timer *dummy)
{
    k_work_submit(&ble_adv_work);
}
K_TIMER_DEFINE(ble_adv_timer, ble_adv_timer_handler, NULL);

struct bt_conn *ble_get_conn(void)
{
    return current_conn;
}

static void publish_bt_conn_state(bool connected)
{
#if !defined(CONFIG_AVOID_ZBUS)
    bool state = connected;
    int  err   = zbus_chan_pub(&BT_CONN_STATE_UPDATE, &state, K_SECONDS(1));
    if (err) {
        LOG_ERR("zbus_chan_pub, error: %d", err);
        return;
    }
#endif
}

static void connected(struct bt_conn *conn, uint8_t err)
{
    char addr[BT_ADDR_LE_STR_LEN];

    if (err) {
        LOG_ERR("Connection failed (err %u)", err);
        publish_bt_conn_state(false);
        return;
    }

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("Connected %s", addr);
    if (uicr_shipping_flag_get()) {
        led_api_set_state(LED_BLE_ADV_DONE);
    }

    if (current_conn == NULL) {
        current_conn = bt_conn_ref(conn);
        publish_bt_conn_state(true);
    } else {
        LOG_ERR("current_conn exists!");
        publish_bt_conn_state(false);
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    char addr[BT_ADDR_LE_STR_LEN];

    if (auth_conn) {
        bt_conn_unref(auth_conn);
        auth_conn = NULL;
    }

    if (current_conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    if (defer_ble_stop) {
        LOG_ERR("Deferred BLE stop on disconnect");
        bt_le_adv_stop();
        bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_POWER_OFF);    //Should I have this?
        if (uicr_shipping_flag_get()) {
            led_api_set_state(LED_BLE_ADV_DONE);
        }
    }

    LOG_DBG("WHAT SHOULD WE DO FOR THE LED HERE????????");
    //     if(uicr_shipping_flag_get()){
    //         led_api_set_state(LED_BLE_ADV);
    // }

    publish_bt_conn_state(false);
}

void mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
    LOG_INF("Updated MTU: TX: %d RX: %d bytes", tx, rx);
}

static struct bt_gatt_cb gatt_callbacks = { .att_mtu_updated = mtu_updated };

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (!err) {
        LOG_INF("Security changed: %s level %u\n", addr, level);

        if (bt_conn_get_security(conn) >= BT_SECURITY_L2) {
            LOG_INF("security >= BT_SECURITY_L2");
        }
    } else {
        LOG_ERR("Security failed: %s level %u err %d\n", addr, level, err);
    }
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected        = connected,
    .disconnected     = disconnected,
    .security_changed = security_changed,
};

static void auth_cancel(struct bt_conn *conn)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    printk("Pairing cancelled: %s\n", addr);

    bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

static void pairing_complete(struct bt_conn *conn, bool bonded)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    printk("Pairing completed: %s, bonded: %d\n", addr, bonded);
}

static void pairing_failed(struct bt_conn *conn, enum bt_security_err reason)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    printk("Pairing failed conn: %s, reason %d\n", addr, reason);

    bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}

static struct bt_conn_auth_cb conn_auth_callbacks = {
    .cancel = auth_cancel,
};

static struct bt_conn_auth_info_cb conn_auth_info_callbacks = { .pairing_complete = pairing_complete,
                                                                .pairing_failed   = pairing_failed };

int ble_advertise_start(int timeout_sec)
{
    int err = 0;

    // advertisement
    const struct bt_data advertisement_data[] = {
        BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
        BT_DATA(BT_DATA_NAME_COMPLETE, local_name_str, strlen(local_name_str)),
    };

    // company ID followed by 12-character serial number (without null termination)
    uint8_t mfg_data[sizeof(company_id) + UICR_STR_MAX_LEN - 1];
    memcpy(mfg_data, &company_id, sizeof(company_id));

    char *serial_num = uicr_serial_number_get();
    memcpy(&mfg_data[sizeof(company_id)], serial_num, UICR_STR_MAX_LEN - 1);

    // scan response
    const struct bt_data scan_response_data[] = { BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_data, sizeof(mfg_data)) };

    err = bt_le_adv_start(
        BT_LE_ADV_CONN,
        advertisement_data,
        ARRAY_SIZE(advertisement_data),
        scan_response_data,
        ARRAY_SIZE(scan_response_data));
    LOG_ERR("Turning on BLE for %d seconds", timeout_sec);
    if (timeout_sec != 0) {
        k_timer_start(&ble_adv_timer, K_SECONDS(timeout_sec), K_SECONDS(0));
    }
    if (uicr_shipping_flag_get()) {
        led_api_set_state(LED_BLE_ADV);
    }
    if (err) {
        LOG_ERR("Advertising failed to start (err %d)", err);
        return -1;
    }
    return 0;
}
int ble_stop(void)
{
    if (current_conn == NULL) {
        LOG_ERR("Stopping BLE Advertising, not in a connection");
        bt_le_adv_stop();
        bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_POWER_OFF);    //Should I have this?
        if (uicr_shipping_flag_get()) {
            led_api_set_state(LED_BLE_ADV_DONE);
        }
    } else {
        LOG_ERR("Not stopping adv since we are in a connection??????");
        defer_ble_stop = true;
    }
    return 0;
}

int ble_init()
{
    int err = 0;

    // bt_conn_cb_register(&conn_callbacks);

    err = bt_enable(NULL);
    if (err) {
        LOG_ERR("ble_init error: %d", err);
    }
    settings_load();
    char *serial_num = uicr_serial_number_get();

    if (serial_num[UICR_STR_MAX_LEN - 7] != 0xFF) {
        sprintf(
            local_name_str,
            "Petivity Tracker %c%c%c%c%c%c",
            serial_num[UICR_STR_MAX_LEN - 7],
            serial_num[UICR_STR_MAX_LEN - 6],
            serial_num[UICR_STR_MAX_LEN - 5],
            serial_num[UICR_STR_MAX_LEN - 4],
            serial_num[UICR_STR_MAX_LEN - 3],
            serial_num[UICR_STR_MAX_LEN - 2]);

    } else {
        bt_addr_le_t addr  = { 0 };
        size_t       count = 1;
        bt_id_get(&addr, &count);
        LOG_DBG("serial number not set, using BLE MAC address");
        sprintf(local_name_str, "Petivity Tracker %02X%02X%02X", addr.a.val[2], addr.a.val[1], addr.a.val[0]);
    }
    LOG_DBG("BLE Device Name: %s", local_name_str);

    // set GAP Device Name
    bt_set_name(local_name_str);

    bt_gatt_cb_register(&gatt_callbacks);

    err = bt_conn_auth_cb_register(&conn_auth_callbacks);
    if (err) {
        LOG_ERR("Failed to register authorization callbacks\n");
        return 0;
    }

    err = bt_conn_auth_info_cb_register(&conn_auth_info_callbacks);
    if (err) {
        LOG_ERR("Failed to register authorization info callbacks.\n");
        return 0;
    }
    tracker_service_init();
    LOG_INF("Bluetooth initialized");
    if (uicr_shipping_flag_get()) {
        LOG_ERR("Advertise for %ds, shipped unit", BLE_ADV_TIME);
        return ble_advertise_start(BLE_ADV_TIME);
    } else {
        LOG_ERR("MFG UNIT, NO BLE");
        return 0;
    }
}

int ble_disconnect(void)
{
    LOG_WRN("forcing BLE disconnect");
    bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    return 0;
}

int ble_shutdown(void)
{
    bt_le_adv_stop();
    bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_POWER_OFF);
    return 0;
}

char *ble_get_local_name(void)
{
    return local_name_str;
}

#include <zephyr/shell/shell.h>

static int ble_adv_shell(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        LOG_ERR("This needs a timeout( in seconds)");
        return -1;
    }
    int timeout = atoi(argv[1]);
    ble_advertise_start(timeout);
    return 0;
}

SHELL_CMD_REGISTER(ble_adv, NULL, "Advertise for N seconds", ble_adv_shell);
