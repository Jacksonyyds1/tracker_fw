/*
 * Copyright (c) 2023 Culvert Engineering
 *
 * SPDX-License-Identifier: Unlicensed
 */

#include "ble.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <zephyr/types.h>

#include "chekr.h"

LOG_MODULE_REGISTER(ble, LOG_LEVEL_DBG);

static struct bt_conn *current_conn;
static struct bt_conn *auth_conn;

static uint16_t company_id = 0x2502; // TODO: this is Nestlé Nespresso
#define BLE_ADDR_LEN (sizeof(bt_addr_t))

static char local_name_str[16];

/*hardcoded to 8 */
#define TX_POWER (8)

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (err) {
		LOG_ERR("Connection failed (err %u)", err);
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Connected %s", addr);

	if (current_conn == NULL) {
		current_conn = bt_conn_ref(conn);
	} else {
		LOG_ERR("current_conn exists!");
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
	LOG_INF("Disconnected: %s (reason %u)", addr, reason);
}

void mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	LOG_INF("Updated MTU: TX: %d RX: %d bytes", tx, rx);
}

static struct bt_gatt_cb gatt_callbacks = {.att_mtu_updated = mtu_updated};

static struct bt_conn_cb conn_callbacks = {
	.connected = connected,
	.disconnected = disconnected,
};

int ble_init(void)
{
	int err = 0;
	bt_addr_le_t addr = {0};
	size_t count = 1;

	bt_conn_cb_register(&conn_callbacks);

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("ble_init error: %d", err);
	}

	LOG_INF("Bluetooth initialized");
	bt_id_get(&addr, &count);
	sprintf(local_name_str, "NDC%02X%02X%02X%02X%02X%02X", addr.a.val[5], addr.a.val[4],
		addr.a.val[3], addr.a.val[2], addr.a.val[1], addr.a.val[0]);
	LOG_DBG("BLE Device Name: %s", local_name_str);

	// set GAP Device Name
	bt_set_name(local_name_str);

	bt_gatt_cb_register(&gatt_callbacks);

	err = chekr_service_init();
	if (err) {
		LOG_ERR("Failed to initialize checkr service (err: %d)", err);
		return -1;
	}

	/* set MfgData containing address in advertisement data */
	uint8_t mfg_data[sizeof(company_id) + BLE_ADDR_LEN];
	memcpy(mfg_data, &company_id, sizeof(company_id));
	memcpy(&mfg_data[sizeof(company_id)], addr.a.val, sizeof(addr.a.val));

	const struct bt_data advertisement_data[] = {
		BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
		BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_data, sizeof(mfg_data)),
		BT_DATA(BT_DATA_NAME_COMPLETE, local_name_str, strlen(local_name_str)),
	};

	const struct bt_data scan_response_data[] = {
		BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_CHEKR_SERVICE_VAL)};

	err = bt_le_adv_start(BT_LE_ADV_CONN, advertisement_data, ARRAY_SIZE(advertisement_data),
			      scan_response_data, ARRAY_SIZE(scan_response_data));

	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		return -1;
	}

	return 0;
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