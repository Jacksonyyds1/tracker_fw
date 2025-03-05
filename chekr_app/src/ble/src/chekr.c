/*
 * Copyright (c) 2023 Culvert Engineering
 *
 * SPDX-License-Identifier: Unlicensed
 */
#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/kernel.h>
#include <zephyr/posix/time.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#include <stdio.h>

#include <zephyr/logging/log.h>
#include "ble.h"
#include "chekr.h"
#include "ml.h"
#include "utils.h"
#include "app_version.h"

LOG_MODULE_REGISTER(chekr, LOG_LEVEL_DBG);

#define MAX_COMMAND_LEN (128)
#define MAX_FRAME_LEN   (255)

// FIXME: find correct values
#define DEVICE_MODEL 'a'
#define MFG_CODE     '1'
#define FACTORY_CODE '2'
#define PCBA_MAJOR   (0)
#define PCBA_MINOR   (0)
#define PCBA_PATCH   (1)

typedef struct __attribute__((__packed__)) {
	frame_format_header_t header;
	uint8_t reservered;
	uint16_t crc;
} app_to_device_req_t;

typedef int (*command_funcp_t)(char *data, int len);

typedef struct {
	commands_list_t command;
	command_funcp_t func;
	uint8_t frame_len;

} cmd_jump_entry_t;

typedef struct {
	uint8_t response[MAX_FRAME_LEN];
	uint8_t response_len;
} read_response_t;

static read_response_t read_response;

// command declarations
static int get_device_mac_addr(char *data, int len);
static int get_device_system_info(char *data, int len);

// TODO: remove unused attribute after included in jump table
static int get_user_info(char *data, int len) __attribute__((unused));
static int set_dog_collar_position(char *data, int len);
static int set_dog_size(char *data, int len);
static int get_epoch_rtc(char *data, int len);
static int set_epoch_rtc(char *data, int len);
static int start_stop_raw_data_harvesting(char *data, int len) __attribute__((unused));
static int start_stop_activity_data_harvesting(char *data, int len) __attribute__((unused));
static int start_stop_periodic_dashboard_status_info(char *data, int len);
static int ble_status_show(char *data, int len);
static int ble_connected_show(char *data, int len);
static int factory_reset(char *data, int len) __attribute__((unused));
static int reboot(char *data, int len);
static int system_alarm_status_notif(char *data, int len) __attribute__((unused));
static int system_general_notif(char *data, int len) __attribute__((unused));
static int read_raw_rec_session_data(char *data, int len) __attribute__((unused));
static int read_activity_rec_session_data(char *data, int len) __attribute__((unused));
static int read_device_log_info(char *data, int len) __attribute__((unused));
static int read_dashboard_info(char *data, int len);

// command jump table
static cmd_jump_entry_t command_jump_table[] = {

	// these commands are called after connection
	{SET_EPOCH_RTC, set_epoch_rtc, SET_EPOCH_RTC_FRAME_LEN},
	{GET_EPOCH_RTC, get_epoch_rtc, GET_EPOCH_RTC_FRAME_LEN},
	{SET_DOG_COLLAR_POSITION, set_dog_collar_position, SET_DOG_COLLAR_POS_FRAME_LEN},
	{SET_DOG_SIZE, set_dog_size, SET_DOG_SIZE_FRAME_LEN},
	{READ_DASHBOARD_INFO, read_dashboard_info, READ_DASHBOARD_INFO_FRAME_LEN},

	// these commands are used for a typical raw data recording session
	{START_STOP_RECORDING_SESSION, start_stop_rec_session, START_STOP_REC_SESSION_FRAME_LEN},
	{READ_REC_SESSION_DETAILS_RAW_IMU, read_rec_session_details_raw_imu,
	 READ_REC_SESSION_DETAILS_FRAME_LEN},
	{READ_REC_SESSION_DATA_RAW_IMU, read_rec_session_data_raw_imu,
	 READ_REC_SESSION_DATA_FRAME_LEN},

	{READ_REC_SESSION_DETAILS_ACTIVITY, read_rec_session_details_activity,
	 READ_REC_SESSION_DETAILS_FRAME_LEN},
	{READ_REC_SESSION_DATA_ACTIVITY, read_rec_session_data_activity,
	 READ_REC_SESSION_DATA_FRAME_LEN},

	// unsure if and when these are used
	{GET_DEVICE_MAC_ADDR, get_device_mac_addr, GET_DEVICE_MAC_ADDR_FRAME_LEN},
	{GET_DEVICE_SYSTEM_INFO, get_device_system_info, GET_DEVICE_SYSTEM_INFO_FRAME_LEN},
	{FACTORY_RESET, factory_reset, FACTORY_RESET_FRAME_LEN},
	{REBOOT, reboot, REBOOT_FRAME_LEN},
	{BLE_STATUS_SHOW, ble_status_show, BLE_STATUS_SHOW_FRAME_LEN},
	{BLE_CONNECTED_SHOW, ble_connected_show, BLE_CONNECTED_SHOW_FRAME_LEN},
	{START_STOP_PERIODIC_DASHBOARD_STATUS_INFO, start_stop_periodic_dashboard_status_info,
	 START_STOP_PERIODIC_DASH_FRAME_LEN},

	// fill in more commands as necessary here
};

static void command_parse_work_handler(struct k_work *work);

// define UUIDs
static struct bt_uuid_128 chekr_service_uuid = BT_UUID_INIT_128(BT_UUID_CHEKR_SERVICE_VAL);

static struct bt_uuid_128 chekr_tx_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x6765a69d, 0xcd79, 0x4df6, 0xaad5, 0x043df9425556));

static struct bt_uuid_128 chekr_rx_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0xb6ab2ce3, 0xa5aa, 0x436a, 0x817a, 0xcc13a45aab76));

static struct bt_uuid_128 chekr_notify_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x207bdc30, 0xc3cc, 0x4a14, 0x8b66, 0x56ba8a826640));

// FIXME: workaround to use device with Bleak on macOS
static struct bt_uuid_128 chekr_tx_no_resp_uuid =
	BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x6765a69d, 0xcd79, 0x4df6, 0xaad5, 0x043df9425557));

// we use this struct to hold commands
struct CommandBufferT {
	struct k_work work;
	uint8_t len;
	uint8_t data[MAX_COMMAND_LEN];
};

// called when central writes to the write characteristic
// we queue it as work, in case there are back to back commands
static ssize_t write_to_device(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			       const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	LOG_DBG("rx %d bytes", len);

	// malloc a command buffer and submit to work queue.  we free it asap when we process it.
	struct CommandBufferT *cmd = k_malloc(sizeof(struct CommandBufferT));
	if (!cmd) {
		LOG_ERR("can't malloc command buffer!");
		return -1;
	}

	cmd->len = len;
	memcpy(cmd->data, buf, len);
	k_work_init(&cmd->work, command_parse_work_handler);
	k_work_submit(&cmd->work);
	return 0;
}

volatile bool notify_enable;

static void mpu_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	notify_enable = (value == BT_GATT_CCC_NOTIFY);
	LOG_DBG("Notification %s", notify_enable ? "enabled" : "disabled");
}

// called when central wants to read from the read characteristic
static ssize_t read_from_central(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
				 uint16_t len, uint16_t offset)
{
	// LOG_HEXDUMP_DBG(read_response.response, read_response.response_len, "read");
	LOG_DBG("reading %d bytes", read_response.response_len);

	return bt_gatt_attr_read(conn, attr, buf, len, offset, read_response.response,
				 read_response.response_len);
}

// define service
BT_GATT_SERVICE_DEFINE(
	chekr_service, BT_GATT_PRIMARY_SERVICE(&chekr_service_uuid),

	// notify
	BT_GATT_CHARACTERISTIC(&chekr_notify_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ,
			       NULL, NULL, NULL),

	// notify ccc
	BT_GATT_CCC(mpu_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	// tx - write with response from host
	BT_GATT_CHARACTERISTIC(&chekr_tx_uuid.uuid, BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE, NULL,
			       write_to_device, NULL),

	// tx - write without response from host (FIXME: workaround for Bleak on macOS)
	BT_GATT_CHARACTERISTIC(&chekr_tx_no_resp_uuid.uuid, BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE, NULL, write_to_device, NULL),

	// rx - read from host
	BT_GATT_CHARACTERISTIC(&chekr_rx_uuid.uuid, BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
			       read_from_central, NULL, NULL),

);

// send notification
void write_to_central(uint8_t *data, int len)
{
	if (1) // TODO: get bt_conn
	{
		memcpy(read_response.response, data, len);
		read_response.response_len = len;
	} else {
		LOG_INF("BLE not connected");
	}
}

int chekr_service_init(void)
{
	// create ChekrAppLink service
	dashboard_init();
	return 0;
}

// parse and handle commands put on work queue when central writes to device
// it's a work handler so shouldn't be re-entrant as it's handled in a queue
static void command_parse_work_handler(struct k_work *work)
{
	// copy queued command data and length, and free it asap
	struct CommandBufferT *cmd = CONTAINER_OF(work, struct CommandBufferT, work);
	uint8_t data[MAX_COMMAND_LEN] = {0};
	memcpy(data, cmd->data, cmd->len);
	uint8_t len = cmd->len;
	k_free(cmd);
	LOG_HEXDUMP_DBG(data, len, "command");

	frame_format_header_t *header = (frame_format_header_t *)data;

	// some basic checks
	if (header->start_byte != SB_MOBILE_APP_TO_DEVICE) {
		LOG_ERR("invalid start byte: %02x", header->start_byte);
		// FIXME: report Invalid Start byte
		return;
	}

	uint8_t frame_len = header->frame_len;

	if (frame_len != len) {
		LOG_ERR("frame length mismatch: received %d, expected %d", len, frame_len);
		// FIXME: report Length is not matching
		return;
	}

	frame_type_t frame_type = header->frame_type;
	if (frame_type != FT_REQUEST) {
		LOG_ERR("invalid frame type: %d", header->frame_type);
		// FIXME: report Unrecognized Frame Type
		return;
	}

	// TODO: verify crc endianess, verify it includes entire frame

	uint16_t req_crc = *(uint16_t *)(cmd->data + (len - 2)); //这行代码有错误，CRC一直没变
	uint16_t calc_crc = utils_crc16_modbus(data, len - sizeof(uint16_t));
	if (calc_crc != req_crc) {
		LOG_ERR("invalid crc: request=%04x, calc=%04x", req_crc, calc_crc);
		// FIXME: report Invalid Checksum
		return;
	}

	LOG_INF("processing command: %d", header->cmd);

	// jump to command if defined
	for (int i = 0; i < sizeof(command_jump_table) / sizeof(cmd_jump_entry_t); i++) {
		commands_list_t command = command_jump_table[i].command;
		command_funcp_t func = command_jump_table[i].func;
		uint8_t expected_frame_len = command_jump_table[i].frame_len;

		if (header->cmd == command && func) {

			if (expected_frame_len != len) {
				LOG_ERR("jump table frame length mismatch: received %d, expected "
					"%d",
					len, expected_frame_len);
				// FIXME: report Length is not matching
				return;
			}

			LOG_DBG("calling func for command: %d", command);
			func(data, len);
			return;
		}
	}
	LOG_WRN("command not defined: %d", header->cmd);
	// FIXME: report Unrecognized Command
}

// commands
// TODO: split these out into separate files based on functionality
static int get_device_mac_addr(char *data, int len)
{
	size_t count = 1;
	bt_addr_le_t addr = {0};
	typedef struct __attribute__((__packed__)) {
		frame_format_header_t header;
		uint8_t ack;
		uint8_t mac_addr[6];
		uint16_t crc;
	} get_mac_addr_resp_t;

	get_mac_addr_resp_t resp = {
		.header.start_byte = SB_DEVICE_TO_MOBILE_APP,
		.header.frame_len = sizeof(get_mac_addr_resp_t),
		.header.frame_type = FT_REPORT,
		.header.cmd = GET_DEVICE_MAC_ADDR,
		resp.ack = ERR_NONE,
	};

	bt_id_get(&addr, &count);
	resp.mac_addr[0] = addr.a.val[0];
	resp.mac_addr[1] = addr.a.val[1];
	resp.mac_addr[2] = addr.a.val[2];
	resp.mac_addr[3] = addr.a.val[3];
	resp.mac_addr[4] = addr.a.val[4];
	resp.mac_addr[5] = addr.a.val[5];

	resp.crc = utils_crc16_modbus((const uint8_t *)&resp, sizeof(resp) - sizeof(uint16_t));
	write_to_central((uint8_t *)&resp, sizeof(resp));

	return 0;
}

static int get_device_system_info(char *data, int len)
{
	typedef struct __attribute__((__packed__)) {
		frame_format_header_t header;
		uint8_t ack;
		uint8_t boot_major;
		uint8_t boot_minor;
		uint8_t boot_patch;
		uint8_t app_fw_major;
		uint8_t app_fw_minor;
		uint8_t app_fw_patch;
		uint8_t pcba_major;
		uint8_t pcba_minor;
		uint8_t pcba_patch;
		uint8_t device_name[6];
		uint8_t device_model;
		uint8_t mfg_code;
		uint8_t factory_code;
		uint8_t mfg_date[4];
		uint8_t serial_num[8];
		uint8_t reserved[11];
		uint16_t crc;
	} get_device_info_resp_t;

	get_device_info_resp_t resp = {
		.header.start_byte = SB_DEVICE_TO_MOBILE_APP,
		.header.frame_len = sizeof(get_device_info_resp_t),
		.header.frame_type = FT_REPORT,
		.header.cmd = GET_DEVICE_SYSTEM_INFO,
		.ack = ERR_NONE,

		.app_fw_major = APP_VERSION_MAJOR,
		.app_fw_minor = APP_VERSION_MINOR,
		.app_fw_patch = APP_VERSION_PATCH,

		.pcba_major = PCBA_MAJOR,
		.pcba_minor = PCBA_MINOR,
		.pcba_patch = PCBA_PATCH,

		.device_name = {'t', 'r', 'a', 'k', 'r', '1'},
		.device_model = DEVICE_MODEL,
		.mfg_code = MFG_CODE,
		.factory_code = FACTORY_CODE,
		.mfg_date = {0x65, 0x2f, 0x1b, 0xc9} // todays date - Big Endian?
	};
	// skip 'NDC' in name
	memcpy(resp.serial_num, ble_get_local_name() + 3, sizeof(resp.serial_num));

	resp.crc = utils_crc16_modbus((const uint8_t *)&resp, sizeof(resp) - sizeof(uint16_t));
	write_to_central((uint8_t *)&resp, sizeof(resp));

	return 0;
}

static int get_user_info(char *data, int len)
{
	return 0;
}

static int set_dog_collar_position(char *data, int len)
{
	typedef struct __attribute__((__packed__)) {
		frame_format_header_t header;
		uint8_t position;
		uint16_t crc;
	} set_dog_collar_pos_t;

	set_dog_collar_pos_t *pos = (set_dog_collar_pos_t *)data;
	LOG_DBG("set position to %d", pos->position);

	// TODO: what do we do with position?

	// send response
	typedef struct __attribute__((__packed__)) {
		frame_format_header_t header;
		uint8_t ack;
		uint8_t reserved;
		uint16_t crc;
	} set_dog_collar_pos_resp_t;

	set_dog_collar_pos_resp_t resp = {
		.header.start_byte = SB_DEVICE_TO_MOBILE_APP,
		.header.frame_len = sizeof(set_dog_collar_pos_resp_t),
		.header.frame_type = FT_REPORT,
		.header.cmd = SET_DOG_COLLAR_POSITION,
		.ack = ERR_NONE,
	};

	resp.crc = utils_crc16_modbus((const uint8_t *)&resp, sizeof(resp) - sizeof(uint16_t));
	write_to_central((uint8_t *)&resp, sizeof(resp));
	return 0;
}

static int set_dog_size(char *data, int len)
{
	typedef struct __attribute__((__packed__)) {
		frame_format_header_t header;
		uint8_t size;
		uint16_t crc;
	} set_dog_size_t;

	set_dog_size_t *p = (set_dog_size_t *)data;
	pet_size_t size = p->size;
	ml_set_dog_size(size);

	LOG_DBG("set size to %d", size);

	// send response
	typedef struct __attribute__((__packed__)) {
		frame_format_header_t header;
		uint8_t ack;
		uint16_t crc;
	} set_dog_size_resp_t;

	set_dog_size_resp_t resp = {
		.header.start_byte = SB_DEVICE_TO_MOBILE_APP,
		.header.frame_len = sizeof(set_dog_size_resp_t),
		.header.frame_type = FT_REPORT,
		.header.cmd = SET_DOG_SIZE,
		.ack = ERR_NONE,
	};

	resp.crc = utils_crc16_modbus((const uint8_t *)&resp, sizeof(resp) - sizeof(uint16_t));
	write_to_central((uint8_t *)&resp, sizeof(resp));
	return 0;
}

static int get_epoch_rtc(char *data, int len)
{
	typedef struct __attribute__((__packed__)) {
		frame_format_header_t header;
		uint8_t ack;
		uint8_t time[8];
		uint16_t crc;
	} get_epoch_time_resp_t;

	get_epoch_time_resp_t resp = {
		.header.start_byte = SB_DEVICE_TO_MOBILE_APP,
		.header.frame_len = sizeof(get_epoch_time_resp_t),
		.header.frame_type = FT_REPORT,
		.header.cmd = GET_EPOCH_RTC,
		.ack = ERR_NONE,
	};

	uint64_t currentmillis = sys_cpu_to_be64(utils_get_currentmillis());
	memcpy(resp.time, &currentmillis, sizeof(currentmillis));

	resp.crc = utils_crc16_modbus((const uint8_t *)&resp, sizeof(resp) - sizeof(uint16_t));
	write_to_central((uint8_t *)&resp, sizeof(resp));

	return 0;
}

static int set_epoch_rtc(char *data, int len)
{
	typedef struct __attribute__((__packed__)) {
		frame_format_header_t header;
		uint64_t time;
		uint16_t crc;
	} set_epoch_time_t;

	set_epoch_time_t *t = (set_epoch_time_t *)data;
	uint64_t currentmillis = sys_be64_to_cpu(t->time);

	struct timespec ts;
	ts.tv_sec = currentmillis / 1000;
	ts.tv_nsec = (currentmillis % 1000) * 1000000;
	int ret = clock_settime(CLOCK_REALTIME, &ts);
	if (ret != 0) {
		LOG_ERR("Failed to set system time: ret=%d", ret);
		// FIXME: goto out with .ack ERR set
		return -1;
	} else {
		LOG_DBG("set clock to %llu", currentmillis);
	}

	// send response
	typedef struct __attribute__((__packed__)) {
		frame_format_header_t header;
		uint8_t ack;
		uint8_t reserved;
		uint16_t crc;
	} set_epoch_time_resp_t;

	set_epoch_time_resp_t resp = {
		.header.start_byte = SB_DEVICE_TO_MOBILE_APP,
		.header.frame_len = sizeof(set_epoch_time_resp_t),
		.header.frame_type = FT_REPORT,
		.header.cmd = SET_EPOCH_RTC,
		.ack = ERR_NONE,
	};

	resp.crc = utils_crc16_modbus((const uint8_t *)&resp, sizeof(resp) - sizeof(uint16_t));
	write_to_central((uint8_t *)&resp, sizeof(resp));
	return 0;
}

static int start_stop_raw_data_harvesting(char *data, int len)
{
	return 0;
}

static int start_stop_activity_data_harvesting(char *data, int len)
{
	return 0;
}

static int start_stop_periodic_dashboard_status_info(char *data, int len)
{
	typedef struct __attribute__((__packed__)) {
		frame_format_header_t header;
		dashboard_ctrl_t start_stop_once;
		uint8_t interval;
		uint16_t crc;
	} ble_periodic_dashboard_set_t;

	ble_periodic_dashboard_set_t *dash = (ble_periodic_dashboard_set_t *)data;
	dashboard_ctrl(dash->start_stop_once, dash->interval);

	return 0;
}

// ble_status_show and ble_connected_show look idential from the spec
static int ble_status_show(char *data, int len)
{
	typedef struct __attribute__((__packed__)) {
		frame_format_header_t header;
		uint8_t ack;
		uint8_t reserved;
		uint16_t crc;
	} ble_status_show_resp_t;

	ble_status_show_resp_t resp = {
		.header.start_byte = SB_DEVICE_TO_MOBILE_APP,
		.header.frame_len = sizeof(ble_status_show_resp_t),
		.header.frame_type = FT_REPORT,
		.header.cmd = BLE_STATUS_SHOW,
		.ack = ERR_NONE,
	};

	resp.crc = utils_crc16_modbus((const uint8_t *)&resp, sizeof(resp) - sizeof(uint16_t));
	write_to_central((uint8_t *)&resp, sizeof(resp));
	return 0;
}

static int ble_connected_show(char *data, int len)
{
	typedef struct __attribute__((__packed__)) {
		frame_format_header_t header;
		uint8_t ack;
		uint8_t reserved;
		uint16_t crc;
	} ble_status_show_resp_t;

	ble_status_show_resp_t resp = {
		.header.start_byte = SB_DEVICE_TO_MOBILE_APP,
		.header.frame_len = sizeof(ble_status_show_resp_t),
		.header.frame_type = FT_REPORT,
		.header.cmd = BLE_CONNECTED_SHOW,
		.ack = ERR_NONE,
	};

	resp.crc = utils_crc16_modbus((const uint8_t *)&resp, sizeof(resp) - sizeof(uint16_t));
	write_to_central((uint8_t *)&resp, sizeof(resp));
	return 0;
}

static int factory_reset(char *data, int len)
{
	typedef struct __attribute__((__packed__)) {
		frame_format_header_t header;
		uint8_t ack;
		uint8_t reserved;
		uint16_t crc;
	} ble_factory_reset_resp_t;

	ble_factory_reset_resp_t resp = {
		.header.start_byte = SB_DEVICE_TO_MOBILE_APP,
		.header.frame_len = sizeof(ble_factory_reset_resp_t),
		.header.frame_type = FT_REPORT,
		.header.cmd = FACTORY_RESET,
		.ack = ERR_NONE,
	};

	resp.crc = utils_crc16_modbus((const uint8_t *)&resp, sizeof(resp) - sizeof(uint16_t));
	write_to_central((uint8_t *)&resp, sizeof(resp));

	// TODO: what do we do for factory reset?
	return 0;
}

static int reboot(char *data, int len)
{
	typedef struct __attribute__((__packed__)) {
		frame_format_header_t header;
		uint8_t ack;
		uint8_t reserved;
		uint16_t crc;
	} ble_reboot_resp_t;

	ble_reboot_resp_t resp = {
		.header.start_byte = SB_DEVICE_TO_MOBILE_APP,
		.header.frame_len = sizeof(ble_reboot_resp_t),
		.header.frame_type = FT_REPORT,
		.header.cmd = REBOOT,
		.ack = ERR_NONE,
	};

	resp.crc = utils_crc16_modbus((const uint8_t *)&resp, sizeof(resp) - sizeof(uint16_t));
	write_to_central((uint8_t *)&resp, sizeof(resp));

	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}

static int system_alarm_status_notif(char *data, int len)
{
	return 0;
}

static int system_general_notif(char *data, int len)
{
	return 0;
}

static int read_raw_rec_session_data(char *data, int len)
{
	return 0;
}

static int read_activity_rec_session_data(char *data, int len)
{
	return 0;
}

static int read_device_log_info(char *data, int len)
{
	return 0;
}

static int read_dashboard_info(char *data, int len)
{
	dashboard_response();
	return 0;
}
