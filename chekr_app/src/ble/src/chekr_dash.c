/*
 * Copyright (c) 2023 Culvert Engineering
 *
 * SPDX-License-Identifier: Unlicensed
 *
 * Periodic Dashboard related code
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>
#include "ble.h"
#include "chekr.h"
#include "pmic_stub.h"
#include "utils.h"
#include "app_version.h"

LOG_MODULE_REGISTER(chekr_dash, LOG_LEVEL_DBG);

#define MAX_INTERVAL_PERIOD_S (240)

typedef struct __attribute__((__packed__)) {
	frame_format_header_t header;
	uint8_t ack;
	uint8_t device_name[8];
	uint8_t device_type;
	uint8_t app_fw_major;
	uint8_t app_fw_minor;
	uint8_t app_fw_patch;
	uint8_t battery_level;
	uint16_t battery_voltage;
	int16_t input_current;
	uint16_t temperature;
	uint16_t crc;
} read_dashboard_info_resp_t;

static void dashboard_timer_handler(struct k_timer *timer_id);
K_TIMER_DEFINE(dashboard_timer, dashboard_timer_handler, NULL);
static K_SEM_DEFINE(dashboard_wait, 0, 1);
static uint32_t dashboard_interval_s;

/* dashboard thread */
#define DASHBOARD_THREAD_STACK_SIZE (1024 * 4)
K_THREAD_STACK_DEFINE(dashboard_thread_stack_area, DASHBOARD_THREAD_STACK_SIZE);
static struct k_thread dashboard_thread_data;
static k_tid_t dashboard_thread_tid;
static void dashboard_thread(void *p1, void *p2, void *p3);

int dashboard_init(void)
{
	LOG_DBG("starting dashboard thread....");
	dashboard_thread_tid = k_thread_create(&dashboard_thread_data, dashboard_thread_stack_area,
					       K_THREAD_STACK_SIZEOF(dashboard_thread_stack_area),
					       dashboard_thread, NULL, NULL, NULL,
					       K_LOWEST_APPLICATION_THREAD_PRIO - 2, 0, K_NO_WAIT);

	k_thread_name_set(&dashboard_thread_data, "dashboard");

	return 0;
}

double celsius_to_fahrenheit(double celsius)
{
	return (celsius * 9.0 / 5.0) + 32.0;
}

int dashboard_response(void)
{

	// send dashboard info
	read_dashboard_info_resp_t resp = {
		.header.start_byte = SB_DEVICE_TO_MOBILE_APP,
		.header.frame_len = 0x1a,
		.header.frame_type = FT_REPORT,
		.header.cmd = READ_DASHBOARD_INFO,
		.ack = ERR_NONE,

		.app_fw_major = APP_VERSION_MAJOR,
		.app_fw_minor = APP_VERSION_MINOR,
		.app_fw_patch = APP_VERSION_PATCH,
	};

	pmic_get_info_t info = {0};
	pmic_get_info(&info);
	resp.battery_level = info.battery_pct;

	uint16_t battery_mv = (uint16_t)(info.battery_v * 1000);
	resp.battery_voltage = sys_cpu_to_be16(battery_mv);

	uint16_t current_ma = abs(info.charging_a * 1000);
	resp.input_current = sys_cpu_to_be16(current_ma);

	float temperature_f = celsius_to_fahrenheit(info.temperature);
	uint16_t temperature_f_scaled = (uint16_t)(temperature_f * 100);
	resp.temperature = sys_cpu_to_be16(temperature_f_scaled);

	memcpy(resp.device_name, ble_get_local_name() + 6, sizeof(resp.device_name));
	resp.device_type = NESTLE_COMMERCIAL_PET_COLLAR;
	resp.crc = utils_crc16_modbus((const uint8_t *)&resp, sizeof(resp) - sizeof(uint16_t));
	write_to_central((uint8_t *)&resp, sizeof(resp));
	LOG_HEXDUMP_DBG((uint8_t *)&resp, sizeof(resp), "dashboard data");
	LOG_DBG("wrote dashboard info, %d bytes", sizeof(resp));
	return 0;
}

static void dashboard_thread(void *p1, void *p2, void *p3)
{
	for (;;) {
		k_sem_take(&dashboard_wait, K_FOREVER);

		LOG_DBG("dashboard_thread running");

		dashboard_response();

		// re-fire at interval if it's set
		if (dashboard_interval_s) {
			k_timer_start(&dashboard_timer, K_MSEC(dashboard_interval_s * 1000),
				      K_NO_WAIT);
		}
	}
}

// give sem when timer fires
static void dashboard_timer_handler(struct k_timer *timer_id)
{
	k_sem_give(&dashboard_wait);
}

// 'interval' is ignored for everything but 'start'
int dashboard_ctrl(dashboard_ctrl_t ctrl, uint8_t interval)
{
	if (interval > MAX_INTERVAL_PERIOD_S) {
		LOG_ERR("invalid interval: %d", interval);
		return -1;
	}

	switch (ctrl) {
	case DASH_ONCE:
		dashboard_interval_s = 0;
		k_sem_give(&dashboard_wait);
		break;
	case DASH_START:
		dashboard_interval_s = interval;
		k_sem_give(&dashboard_wait);
		break;
	case DASH_STOP:
		dashboard_interval_s = 0;
		k_timer_stop(&dashboard_timer);
		break;
	default:
		LOG_ERR("invalid ctrl: %d", ctrl);
		return -1;
		break;
	}

	LOG_DBG("sent dashboard ctrl: %d", ctrl);
	return 0;
}
