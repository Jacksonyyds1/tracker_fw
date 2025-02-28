/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include <net/mqtt_helper.h>

#include "zbus_msgs.h"
#include "transport.h"


ZBUS_CHAN_DEFINE(MQTT_DEV_TO_CLOUD_MESSAGE,
		 mqtt_payload_t,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(mqtt_client),
		 ZBUS_MSG_INIT(0)
);

ZBUS_CHAN_DEFINE(NETWORK_CHAN,
		 enum network_status,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(mqtt_client, network_client, status_client, gps_listener),
		 ZBUS_MSG_INIT(0)
);

ZBUS_CHAN_DEFINE(POWER_STATE_CHANNEL,
		 int,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(shutdown),
		 ZBUS_MSG_INIT(0)
);

ZBUS_CHAN_DEFINE(STATUS_UPDATE,
		 uint8_t,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(spis_sub),
		 ZBUS_MSG_INIT(0)
);

ZBUS_CHAN_DEFINE(MQTT_CLOUD_TO_DEV_MESSAGE,   /* Name */
        inc_mqtt_event_t,                    /* Message type */
        NULL,                                /* Validator */
        NULL,                                /* User Data */
        ZBUS_OBSERVERS(spis_sub),			  /* orvers */
        ZBUS_MSG_INIT(.topic_length = 0, .msg_length = 0)                     /* Initial value {0} */
);

ZBUS_CHAN_DEFINE(SPI_MSG_RESPONSE,
		 spi_msg_response_t,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(spis_sub),
		 ZBUS_MSG_INIT(.msgHandle = 255, .msgStatus = 0, .length = 0, .data = NULL)
);


ZBUS_CHAN_DEFINE(GPS_STATE_CHANNEL,
		 gps_settings_t,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(gps_listener),
		 ZBUS_MSG_INIT(.gps_enable = 0, .gps_fakedata_enable = 0, .gps_sample_period = 0)
);

ZBUS_CHAN_DEFINE(GPS_DATA_CHANNEL,
		 gps_info_t,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(spis_sub),
		 ZBUS_MSG_INIT(.latitude = 0, .longitude = 0, .altitude = 0, .speed = 0, .heading = 0, .timestamp = 0, .numSatellites = 0, .secSinceLock = 0, .timeToLock = 0, .accuracy = 0, .speed_accuracy = 0)
);

ZBUS_CHAN_DEFINE(DEBUG_INFO_CHANNEL,
		 debug_info_t,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(spis_sub),
		 ZBUS_MSG_INIT(.debug_string_length = 0, .debug_level = 0, .debug_string_buffer = {0})
);

ZBUS_CHAN_DEFINE(CELL_INFO_CHANNEL,
		 cell_info_t,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(spis_sub),
		 ZBUS_MSG_INIT(.ip = {0}, .tracking_area = 0, .cellID = {0}, .rssi = 115, .lte_nbiot_mode = 2, .lte_band = 255)
);

ZBUS_CHAN_DEFINE(CELL_NEIGHBOR_INFO_CHANNEL,
		 bool,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(spis_sub),
		 ZBUS_MSG_INIT(true)
);

ZBUS_CHAN_DEFINE(DOWNLOAD_UPDATE_CHANNEL,
		 download_update_t,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(spis_sub),
		 ZBUS_MSG_INIT( .download_status = DOWNLOAD_INPROGRESS, 
		 				.download_progress_bytes = 0, 
						.download_progress_percent = 0,
						.download_total_size = 0,
						.current_dl_amount = 0,
						.download_data_ptr = NULL
						)
);

ZBUS_CHAN_DEFINE(DOWNLOAD_REQUEST_CHANNEL,
		 download_request_t,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(network_client),
		 ZBUS_MSG_INIT(.download_handle = 0, .download_url = {0})
);

ZBUS_CHAN_DEFINE(FW_UPLOAD_CHANNEL,
		 firmware_upload_data_t,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(fota_client),
		 ZBUS_MSG_INIT(.chunk_num = 0, .chunk_total = 0, .data_len = 0, .handle = 255, .data = NULL, .crc = 0, .return_code = 0)
);

ZBUS_CHAN_DEFINE(FW_UPLOAD_RESP_CHANNEL,
		 firmware_upload_data_t,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(spis_sub),
		 ZBUS_MSG_INIT(.chunk_num = 0, .chunk_total = 0, .data_len = 0, .handle = 255, .data = NULL, .crc = 0, .return_code = 0)
);

ZBUS_CHAN_DEFINE(FOTA_DL_CHANNEL,
		 download_request_t,
		 NULL,
		 NULL,
		 ZBUS_OBSERVERS(fota_client),
		 ZBUS_MSG_INIT(.download_handle = 0, .download_url = {0})
);


int send_zbus_debug_info(uint8_t debug_level, uint8_t err_code, char *debug_string) {
#if defined(CONFIG_PURINA_D1_LTE_ENABLE_ZBUS_LOG_MESSAGES)
		// if ((debug_level & 0x7FFF) > CONFIG_PURINA_D1_LTE_ZBUS_LOG_MESSAGE_LEVEL) {
		// 	LOG_DBG("debug level %d is > the configured log level %d", debug_level, CONFIG_PURINA_D1_LTE_ZBUS_LOG_MESSAGE_LEVEL);
		// 	return -EINVAL;
		// }
		//LOG_DBG("sending debug info = %d", debug_level);
		debug_info_t debug_event;
		snprintf(debug_event.debug_string_buffer, 128, "%s", debug_string);
		debug_event.debug_string_length = strlen(debug_event.debug_string_buffer);
		debug_event.debug_level = debug_level;
		debug_event.error_code = err_code;

		int ret = zbus_chan_pub(&DEBUG_INFO_CHANNEL, &debug_event, K_SECONDS(1));
		return ret;
#endif
}