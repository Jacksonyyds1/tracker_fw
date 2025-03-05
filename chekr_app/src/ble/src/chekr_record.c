/*
 * Copyright (c) 2023 Culvert Engineering
 *
 * SPDX-License-Identifier: Unlicensed
 *
 * Raw data recording support
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <stdio.h>

#include "ble.h"
#include "chekr.h"
#include "imu.h"
#include "ml.h"
#include "pmic_stub.h"
#include "storage.h"
#include "utils.h"

typedef enum {
	RECORD_STOP = 0,
	RECORD_START,
} RecordTypeE;

LOG_MODULE_REGISTER(chekr_record, LOG_LEVEL_DBG);

// blip LED when we record this many samples
// at 120Hz sampling it's abouve every ~420ms
#define BLIP_SAMPLE_INTERVAL (50)

static K_SEM_DEFINE(record_wait, 0, 1);

#define RECORDING_MAX_TIME_S (10 * 60)
static void recording_timer_handler(struct k_timer *timer_id);
K_TIMER_DEFINE(recording_timer, recording_timer_handler, NULL);
static void recording_timer_work_handler(struct k_work *work);
K_WORK_DEFINE(recording_timer_work, recording_timer_work_handler);
static int record_to_file(char *filename, RecordTypeE type);

static file_handle_t m_handle;

// we use this struct to queue samples for recording
struct sample_queue_t {
	struct k_work work;
	imu_sample_t sample;
};

void recording_timer_work_handler(struct k_work *work)
{
	LOG_WRN("active recording has reached %d s, stopping!", RECORDING_MAX_TIME_S);
	// filename is only used for logging when stopping the recording, the handle is
	// used internally for closing it via the storage module.
	record_to_file("active", RECORD_STOP);
}

// called when a recording has reached RECORDING_MAX_TIME_S
static void recording_timer_handler(struct k_timer *timer_id)
{
	k_work_submit(&recording_timer_work);
}

static void record_sample_work_handler(struct k_work *work)
{
	imu_sample_t sample;

	// copy queued data, and free it asap
	struct sample_queue_t *sample_q = CONTAINER_OF(work, struct sample_queue_t, work);
	memcpy((uint8_t *)&sample, (uint8_t *)&sample_q->sample, sizeof(imu_sample_t));
	k_free(sample_q);

	// now process it for recording
	chekr_record_samples(sample);
}

static int record_samples_cb(imu_sample_t data)
{
	// put imu data on a work queue and process outside of trigger callback
	struct sample_queue_t *sample_q = k_malloc(sizeof(struct sample_queue_t));
	if (!sample_q) {
		LOG_ERR("can't malloc sample_q!");
		return -1;
	}
	memcpy((uint8_t *)&sample_q->sample, (uint8_t *)&data, sizeof(imu_sample_t));
	k_work_init(&sample_q->work, record_sample_work_handler);
	k_work_submit(&sample_q->work);
	return 0;
}

// used from the shell for testing
int chekr_set_filehandle(file_handle_t handle)
{
	m_handle = handle;
	return 0;
}

int chekr_record_samples(imu_sample_t data)
{
	static raw_imu_record_t record;
	static uint32_t record_count;
	uint8_t index = data.sample_count % RAW_FRAMES_PER_RECORD;

	if (data.sample_count == 0) {
		// reset record_count on first sample of recording sessions
		record_count = 0;
	}

	if (index == 0) {
		// save timestamp of first sample in record
		record.timestamp = sys_cpu_to_be64(data.timestamp);
	}

	// feed sample to ML module
	ml_feed_sample(data);

	record.raw_data[index].ax = data.ax;
	record.raw_data[index].ay = data.ay;
	record.raw_data[index].az = data.az;
	record.raw_data[index].gx = data.gx;
	record.raw_data[index].gy = data.gy;
	record.raw_data[index].gz = data.gz;

	// write to flash when we fill in last sample in record
	if ((index + 1) % (RAW_FRAMES_PER_RECORD) == 0) {
		record.record_num = sys_cpu_to_be32(record_count);
		record_count++;
		int ret = storage_write_raw_imu_record(m_handle, record);
		if (ret) {
			LOG_ERR("failed to write imu sample: ret=%d", ret);
			return -1;
		}
	}

	// indication we're recording
	if (data.sample_count % BLIP_SAMPLE_INTERVAL == 0) {
		pmic_blip_blue_led();
	}

	return 0;
}

static bool recording_in_progress(void)
{
	return m_handle != NULL;
}

// start recording to filename
static int record_to_file(char *filename, RecordTypeE type)
{
	int ret;
	file_handle_t handle;

	if (type == RECORD_START) {
		handle = storage_open_file(filename);

		if (handle == NULL) {
			LOG_ERR("failed to open file %s", filename);
			return -1;
		}

		// set handle if we opened file successfully
		m_handle = handle;

		ret = ml_start(true, m_handle);
		if (ret) {
			LOG_ERR("failed to start ML");
			storage_close_file(m_handle);
			return -1;
		}

		ret = imu_enable(RECORD_SAMPLE_RATE, record_samples_cb);
		if (ret) {
			LOG_ERR("failed to enable IMU");
			storage_close_file(m_handle);
			return -1;
		}

		k_timer_start(&recording_timer, K_SECONDS(RECORDING_MAX_TIME_S), K_NO_WAIT);

		LOG_INF("started IMU and writing IMU samples to file: %s...", filename);
	} else // stop
	{
		LOG_INF("stopping IMU...");

		k_timer_stop(&recording_timer);

		imu_enable(IMU_ODR_0_HZ, NULL); // stop IMU

		LOG_INF("stopped IMU");

		ret = ml_start(false, m_handle);
		if (ret) {
			LOG_ERR("failed to stop ML");
			storage_close_file(m_handle);
			return -1;
		}

		ret = storage_close_file(m_handle);
		if (ret) {
			LOG_ERR("failed to close file %s", filename);
			return -1;
		}
		LOG_INF("stopped IMU and closed file: %s...", filename);

		pmic_blue_led_off();

		m_handle = NULL;
	}

	return 0;
}

/***********************************************/
// Checkr API jump table functions
/***********************************************/
int start_stop_rec_session(char *data, int len)
{
	typedef struct __attribute__((__packed__)) {
		frame_format_header_t header;
		uint8_t start_stop;
		uint8_t uid[8];
	} start_stop_rec_session_req_t;

	start_stop_rec_session_req_t *req = (start_stop_rec_session_req_t *)data;

	// turn uid into a usable filename
	char uid_str[8 * 2 + 1] = {0}; // 2 characters per byte + 1 for null terminator

	for (int i = 0; i < 8; i++) {
		sprintf(&uid_str[i * 2], "%02X", req->uid[i]);
	}

	if (req->start_stop && recording_in_progress()) {
		LOG_WRN("stopping current recording, and starting a new one!");
		// filename is only used for logging when stopping the recording, the handle is
		// used internally for closing it via the storage module.
		record_to_file("active", RECORD_STOP);
	}

	int ret = record_to_file(uid_str, req->start_stop);

	// send response
	typedef struct __attribute__((__packed__)) {
		frame_format_header_t header;
		uint8_t ack;
		uint8_t reserved;
		uint16_t crc;
	} start_stop_rec_session_resp_t;

	start_stop_rec_session_resp_t resp = {
		.header.start_byte = SB_DEVICE_TO_MOBILE_APP,
		.header.frame_len = sizeof(start_stop_rec_session_resp_t),
		.header.frame_type = FT_REPORT,
		.header.cmd = START_STOP_RECORDING_SESSION,
	};

	if (ret == 0) {
		resp.ack = ERR_NONE;
	} else {
		resp.ack = ERR_SLAVE_DEVICE_FAILURE;
	}

	resp.crc = utils_crc16_modbus((const uint8_t *)&resp, sizeof(resp) - sizeof(uint16_t));
	write_to_central((uint8_t *)&resp, sizeof(resp));
	return 0;
}

int read_rec_session_details_raw_imu(char *data, int len)
{
	error_code_t ret = ERR_NONE;

	typedef struct __attribute__((__packed__)) {
		frame_format_header_t header;
		uint8_t uid[8];
		uint8_t reserved;
	} read_rec_session_details_req_t;

	read_rec_session_details_req_t *req = (read_rec_session_details_req_t *)data;

	// turn uid into a usable filename
	char uid_str[8 * 2 + 1] = {0}; // 2 characters per byte + 1 for null terminator

	for (int i = 0; i < 8; i++) {
		sprintf(&uid_str[i * 2], "%02X", req->uid[i]);
	}

	if (recording_in_progress()) {
		// stop a recording if we try to retrieve session details while recording in
		// progress
		LOG_WRN("stopping current recording!");
		record_to_file("active", RECORD_STOP);
	}

	int16_t record_count = storage_get_raw_imu_record_count(uid_str);
	if (record_count < 0) {
		ret = ERR_NEGATIVE_ACK;
		record_count = 0;
	}

	// send response
	typedef struct __attribute__((__packed__)) {
		frame_format_header_t header;
		uint8_t ack;
		uint8_t uid[8];
		uint16_t num_records;
		uint16_t crc;
	} read_rec_session_details_resp_t;

	read_rec_session_details_resp_t resp = {
		.header.start_byte = SB_DEVICE_TO_MOBILE_APP,
		.header.frame_len = sizeof(read_rec_session_details_resp_t),
		.header.frame_type = FT_REPORT,
		.header.cmd = READ_REC_SESSION_DETAILS_RAW_IMU,
	};

	memcpy(resp.uid, req->uid, sizeof(req->uid));
	resp.num_records = sys_cpu_to_be16(record_count);

	resp.ack = ret;

	resp.crc = utils_crc16_modbus((const uint8_t *)&resp, sizeof(resp) - sizeof(uint16_t));
	write_to_central((uint8_t *)&resp, sizeof(resp));
	return 0;
}

int read_rec_session_data_raw_imu(char *data, int len)
{
	typedef struct __attribute__((__packed__)) {
		frame_format_header_t header;
		uint8_t uid[8];
		uint8_t reserved[3];
		uint16_t record_num;
	} read_rec_session_data_req_t;

	read_rec_session_data_req_t *req = (read_rec_session_data_req_t *)data;

	uint16_t record_num = sys_be16_to_cpu(req->record_num);

	// turn uid into a usable filename
	char uid_str[8 * 2 + 1] = {0}; // 2 characters per byte + 1 for null terminator

	for (int i = 0; i < 8; i++) {
		sprintf(&uid_str[i * 2], "%02X", req->uid[i]);
	}

	LOG_DBG("reading record %d from file: %s", record_num, (char *)uid_str);
	raw_imu_record_t record = {0};
	int ret = storage_read_raw_imu_record((char *)uid_str, record_num, &record);
	if (ret < 0) {
		LOG_ERR("failed to read record %d from file: %s", record_num, (char *)uid_str);
		return -1;
	}

	// send response
	typedef struct __attribute__((__packed__)) {
		// NOTE: unusual header, only a SOH byte
		uint8_t soh;
		raw_imu_record_t record;
		uint16_t crc;
	} read_rec_session_data_resp_t;

	read_rec_session_data_resp_t resp = {
		.soh = 2,
	};
	memcpy(&resp.record, &record, sizeof(record));

	resp.crc = utils_crc16_modbus((const uint8_t *)&resp, sizeof(resp) - sizeof(uint16_t));
	write_to_central((uint8_t *)&resp, sizeof(resp));
	return 0;
}

int read_rec_session_details_activity(char *data, int len)
{
	error_code_t ret = ERR_NONE;

	typedef struct __attribute__((__packed__)) {
		frame_format_header_t header;
		uint8_t uid[8];
		uint8_t reserved;
	} read_rec_session_details_req_t;

	read_rec_session_details_req_t *req = (read_rec_session_details_req_t *)data;

	// turn uid into a usable filename
	char uid_str[8 * 2 + 1] = {0}; // 2 characters per byte + 1 for null terminator

	for (int i = 0; i < 8; i++) {
		sprintf(&uid_str[i * 2], "%02X", req->uid[i]);
	}

	if (recording_in_progress()) {
		// stop a recording if we try to retrieve session details while recording in
		// progress
		LOG_WRN("stopping current recording!");
		record_to_file("active", RECORD_STOP);
	}

	int16_t record_count = storage_get_activity_record_count(uid_str);
	if (record_count < 0) {
		ret = ERR_NEGATIVE_ACK;
		record_count = 0;
	}

	// send response
	typedef struct __attribute__((__packed__)) {
		frame_format_header_t header;
		uint8_t ack;
		uint8_t uid[8];
		uint16_t num_records;
		uint16_t crc;
	} read_rec_session_details_resp_t;

	read_rec_session_details_resp_t resp = {
		.header.start_byte = SB_DEVICE_TO_MOBILE_APP,
		.header.frame_len = sizeof(read_rec_session_details_resp_t),
		.header.frame_type = FT_REPORT,
		.header.cmd = READ_REC_SESSION_DETAILS_ACTIVITY,
	};

	memcpy(resp.uid, req->uid, sizeof(req->uid));
	resp.num_records = sys_cpu_to_be16(record_count);

	resp.ack = ret;

	resp.crc = utils_crc16_modbus((const uint8_t *)&resp, sizeof(resp) - sizeof(uint16_t));
	write_to_central((uint8_t *)&resp, sizeof(resp));
	return 0;
}

int read_rec_session_data_activity(char *data, int len)
{
	typedef struct __attribute__((__packed__)) {
		frame_format_header_t header;
		uint8_t uid[8];
		uint8_t reserved[3];
		uint16_t record_num;
	} read_rec_session_data_req_t;

	read_rec_session_data_req_t *req = (read_rec_session_data_req_t *)data;

	uint16_t record_num = sys_be16_to_cpu(req->record_num);

	// turn uid into a usable filename
	char uid_str[8 * 2 + 1] = {0}; // 2 characters per byte + 1 for null terminator

	for (int i = 0; i < 8; i++) {
		sprintf(&uid_str[i * 2], "%02X", req->uid[i]);
	}

	LOG_DBG("reading record %d from file: %s", record_num, (char *)uid_str);
	activity_record_t record = {0};
	int ret = storage_read_activity_record((char *)uid_str, record_num, &record);
	if (ret < 0) {
		LOG_ERR("failed to read record %d from file: %s", record_num, (char *)uid_str);
		return -1;
	}

	// send response
	typedef struct __attribute__((__packed__)) {
		// NOTE: unusual header, only a SOH byte
		uint8_t soh;
		activity_record_t record;
		uint16_t crc;
	} read_rec_session_data_resp_t;

	read_rec_session_data_resp_t resp = {
		.soh = 2,
	};
	memcpy(&resp.record, &record, sizeof(record));

	resp.crc = utils_crc16_modbus((const uint8_t *)&resp, sizeof(resp) - sizeof(uint16_t));
	write_to_central((uint8_t *)&resp, sizeof(resp));
	return 0;
}

static int record(const struct shell *sh, size_t argc, char **argv)
{
	char *filename = argv[1];
	char *op = argv[2];

	if (argc < 3) {
		shell_fprintf(sh, SHELL_NORMAL, "usage: rec_start <filename> <start|stop>\r\n");
		return -1;
	}

	RecordTypeE type;

	if (strcmp("start", op) == 0) {
		type = RECORD_START;
	}

	else if (strcmp("stop", op) == 0) {
		type = RECORD_STOP;
	} else {
		shell_fprintf(sh, SHELL_NORMAL, "usage: rec_start <filename> <start|stop>\r\n");
		return -1;
	}

	record_to_file(filename, type);
	return 0;
}

SHELL_CMD_REGISTER(record, NULL,
		   "start raw recording session, usage: record <filename> <start|stop>", record);
