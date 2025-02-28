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
void ble_adv_timeout_worker(struct k_work *work)
{
	LOG_DBG("Turning off BLE");
	bt_le_adv_stop();
	bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_POWER_OFF);
}
K_WORK_DEFINE(ble_adv_work, ble_adv_timeout_worker);
void ble_adv_timer_handler(struct k_timer *dummy)
{
	k_work_submit(&ble_adv_work);
}
K_TIMER_DEFINE(ble_adv_timer, ble_adv_timer_handler, NULL);

int ble_advertise(int timeout_sec)
{
	bt_addr_le_t addr = {0};
	size_t count = 1;
	int err = 0;

	bt_id_get(&addr, &count);
	sprintf(local_name_str, "CUL_MFG_%02X%02X%02X", addr.a.val[2], addr.a.val[1],
		addr.a.val[0]);
	LOG_DBG("BLE Device Name: %s", local_name_str);

	// set GAP Device Name
	bt_set_name(local_name_str);

	bt_gatt_cb_register(&gatt_callbacks);

	/* set MfgData containing address in advertisement data */
	uint8_t mfg_data[sizeof(company_id) + BLE_ADDR_LEN];
	memcpy(mfg_data, &company_id, sizeof(company_id));
	memcpy(&mfg_data[sizeof(company_id)], addr.a.val, sizeof(addr.a.val));

	const struct bt_data advertisement_data[] = {
		BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
		BT_DATA_BYTES(BT_DATA_TX_POWER, TX_POWER),
		BT_DATA(BT_DATA_MANUFACTURER_DATA, mfg_data, sizeof(mfg_data)),
		BT_DATA(BT_DATA_NAME_COMPLETE, local_name_str, strlen(local_name_str)),
	};

	const struct bt_data scan_response_data[] = {
		BT_DATA(BT_DATA_NAME_COMPLETE, local_name_str, strlen(local_name_str))};

	err = bt_le_adv_start(BT_LE_ADV_CONN, advertisement_data, ARRAY_SIZE(advertisement_data),
			      scan_response_data, ARRAY_SIZE(scan_response_data));
	LOG_DBG("Turning on BLE for %d seconds", timeout_sec);
	if (timeout_sec != 0) {
		k_timer_start(&ble_adv_timer, K_SECONDS(timeout_sec), K_SECONDS(0));
	}

	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		return -1;
	}
	return 0;
}
int ble_init()
{
	int err = 0;

	bt_conn_cb_register(&conn_callbacks);

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("ble_init error: %d", err);
	}

	LOG_INF("Bluetooth initialized");
	return ble_advertise(10);
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
	ble_advertise(timeout);
	return 0;
}

SHELL_CMD_REGISTER(ble_adv, NULL, "Advertise for N seconds", ble_adv_shell);