 /*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#pragma once

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/zbus/zbus.h>

#define DEBUG_INFO_MAX_STRING_LENGTH 128

/** @brief Macro used to send a message on the FATAL_ERROR_CHANNEL.
 *	   The message will be handled in the error module.
 */
#define SEND_FATAL_ERROR()									\
	int instruc1 = SYS_REBOOT;									\
	LOG_ERR("Fatal Error, rebooting.");								\
	k_sleep(K_SECONDS(2));									\
	if (zbus_chan_pub(&POWER_STATE_CHANNEL, &instruc1, K_SECONDS(10))) {			\
		LOG_ERR("Sending a message on the fatal error channel failed, rebooting");	\
	}

#define SEND_SYS_REBOOT()									\
	int instruc2 = SYS_REBOOT;									\
	if (zbus_chan_pub(&POWER_STATE_CHANNEL, &instruc2, K_SECONDS(10))) {			\
		LOG_ERR("error sending reboot event");	\
	}

#define SEND_SYS_SHUTDOWN()									\
	int instruc3 = SYS_SHUTDOWN;									\
	if (zbus_chan_pub(&POWER_STATE_CHANNEL, &instruc3, K_SECONDS(10))) {			\
		LOG_ERR("error sending shutdown event");	\
	}

int send_zbus_debug_info(uint8_t debug_level, uint8_t err_code, char *debug_string);

#define LOG_5340_DBG(_ERR_CODE, _MSG_STR_) send_zbus_debug_info(4, _ERR_CODE, _MSG_STR_)
#define LOG_5340_INF(_ERR_CODE, _MSG_STR_) send_zbus_debug_info(3, _ERR_CODE, _MSG_STR_)
#define LOG_5340_WRN(_ERR_CODE, _MSG_STR_) send_zbus_debug_info(2, _ERR_CODE, _MSG_STR_)
#define LOG_5340_ERR(_ERR_CODE, _MSG_STR_) send_zbus_debug_info(1, _ERR_CODE, _MSG_STR_)

#define LOG_CLOUD_DBG(_ERR_CODE, _MSG_STR_) send_zbus_debug_info(132, _ERR_CODE, _MSG_STR_)
#define LOG_CLOUD_INF(_ERR_CODE, _MSG_STR_) send_zbus_debug_info(131, _ERR_CODE, _MSG_STR_)
#define LOG_CLOUD_WRN(_ERR_CODE, _MSG_STR_) send_zbus_debug_info(130, _ERR_CODE, _MSG_STR_)
#define LOG_CLOUD_ERR(_ERR_CODE, _MSG_STR_) send_zbus_debug_info(129, _ERR_CODE, _MSG_STR_)

typedef struct payload {
    char *topic;
	uint16_t topic_length;
	char *string;
	uint16_t string_length;
	uint8_t qos;
	uint8_t msgHandle;
	uint16_t pad;
} mqtt_payload_t;

typedef struct spi_msg_response {
	uint8_t *data;
	uint16_t length;
	int8_t msgStatus;
	uint8_t msgHandle;
} spi_msg_response_t;

typedef struct debug_info {
	uint16_t debug_string_length;
	uint8_t debug_level;
	uint8_t error_code;
	char debug_string_buffer[DEBUG_INFO_MAX_STRING_LENGTH + 1];
} debug_info_t;

enum network_status {
	NETWORK_DISCONNECTED = 0,
	NETWORK_INITIALIZING = 1,
	NETWORK_CONNECTED = 2,
	NETWORK_AIRPLANE_MODE_ON = 3,
	NETWORK_AIRPLANE_MODE_OFF = 4,
	NETWORK_CELL_CHANGED = 5,
	NETWORK_CELL_NEIGHBORS_CHANGED = 6
};

enum sys_shutdown_state {
	SYS_SHUTDOWN = 0,
	SYS_REBOOT = 1,
};

typedef struct gps_settings {
	uint8_t gps_enable;
	uint8_t gps_fakedata_enable;
	uint8_t gps_sample_period;
	uint8_t pad1;
} gps_settings_t;

typedef struct download_update {
	uint8_t download_status;
	uint8_t download_progress_percent;
	uint16_t current_dl_amount;
	uint8_t* download_data_ptr;
	uint8_t download_handle;
	uint8_t pad1;
	uint8_t pad2;
	uint64_t download_progress_bytes;
	uint64_t download_total_size;
	uint32_t crc;
} download_update_t;

typedef struct download_request {
	uint8_t download_handle;
	char download_url[CONFIG_PURINA_D1_LTE_DOWNLOAD_URL_SIZE_MAX];
} download_request_t;

typedef struct firmware_upload_data {
	uint16_t chunk_num;
	uint16_t chunk_total;
	uint16_t data_len;
	uint16_t handle;
	uint8_t* data;
	uint8_t return_code;
	uint16_t pad1;
	uint32_t crc;
} firmware_upload_data_t;

ZBUS_CHAN_DECLARE(MQTT_CLOUD_TO_DEV_MESSAGE, 
				MQTT_DEV_TO_CLOUD_MESSAGE, 
				NETWORK_CHAN, 
				POWER_STATE_CHANNEL, 
				STATUS_UPDATE, 
				SPI_MSG_RESPONSE, 
				GPS_STATE_CHANNEL, 
				DEBUG_INFO_CHANNEL, 
				GPS_DATA_CHANNEL, 
				DOWNLOAD_UPDATE_CHANNEL,
				DOWNLOAD_REQUEST_CHANNEL,
				CELL_INFO_CHANNEL,
				CELL_NEIGHBOR_INFO_CHANNEL,
				FW_UPLOAD_CHANNEL,
				FOTA_DL_CHANNEL,
				FW_UPLOAD_RESP_CHANNEL
				);

