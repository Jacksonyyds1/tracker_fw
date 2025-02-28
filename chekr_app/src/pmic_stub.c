/*
 * Copyright (c) 2023 Culvert Engineering Inc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/sensor/npm1300_charger.h>
#include <zephyr/dt-bindings/regulator/npm1300.h>
#include <zephyr/shell/shell.h>

#include "pmic_stub.h"

LOG_MODULE_REGISTER(pmic_stub, LOG_LEVEL_DBG);

#define BLIP_ON_MS (50)
static void blip_timer_handler(struct k_timer *timer_id);
static void blip_work_handler(struct k_work *work);
K_TIMER_DEFINE(blip_timer, blip_timer_handler, NULL);
K_WORK_DEFINE(blip_work, blip_work_handler);

typedef struct {
	uint16_t percent;
	uint16_t mV;
} battery_voltage_pct_t;

// TODO: get real values
#define BATTERY_EMPTY_MILLIVOLTS (3000)
#define BATTERY_FULL_MILLIVOLTS  (4350)
const battery_voltage_pct_t m_battery_table[] = {
	{0, BATTERY_EMPTY_MILLIVOLTS},
	{10, 3150},
	{20, 3300},
	{30, 3450},
	{40, 3600},
	{50, 3750},
	{60, 3900},
	{70, 4000},
	{80, 4100},
	{90, 4200},
	{100, BATTERY_FULL_MILLIVOLTS},
};

static int blue_led = 2; // led number 2
static const struct device *charger = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_charger));
static const struct device *rgbleds = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_leds));

void read_charger_sensors_stub(const struct shell *sh, size_t argc, char **argv)
{
	struct sensor_value volt;
	struct sensor_value current;
	struct sensor_value temp;
	struct sensor_value error;
	struct sensor_value status;

	sensor_sample_fetch(charger);
	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &volt);
	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_AVG_CURRENT, &current);
	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_TEMP, &temp);
	sensor_channel_get(charger, SENSOR_CHAN_NPM1300_CHARGER_STATUS, &status);
	sensor_channel_get(charger, SENSOR_CHAN_NPM1300_CHARGER_ERROR, &error);

	shell_fprintf(sh, SHELL_NORMAL, "V: %d.%03d ", volt.val1, volt.val2 / 1000);
	shell_fprintf(sh, SHELL_NORMAL, "I: %s%d.%04d ",
		      ((current.val1 < 0) || (current.val2 < 0)) ? "-" : "", abs(current.val1),
		      abs(current.val2) / 100);
	shell_fprintf(sh, SHELL_NORMAL, "T: %d.%02d\n", temp.val1, temp.val2 / 10000);
	shell_fprintf(sh, SHELL_NORMAL, "Charger Status: %d, Error: %d\n", status.val1, error.val1);
}

int pmic_toggle_blue_led(void)
{
	static bool on;
	if (!device_is_ready(rgbleds)) {
		LOG_ERR("PMIC LED device not ready.\n");
		return -1;
	}

	on = !on;
	on ? led_on(rgbleds, blue_led) : led_off(rgbleds, blue_led);

	return 0;
}

void blip_work_handler(struct k_work *work)
{
	led_off(rgbleds, blue_led);
}

static void blip_timer_handler(struct k_timer *timer_id)
{
	k_work_submit(&blip_work);
}

// blip it on for BLIP_ON_MS
int pmic_blip_blue_led(void)
{
	if (!device_is_ready(rgbleds)) {
		LOG_ERR("PMIC LED device not ready");
		return -1;
	}

	led_on(rgbleds, blue_led);
	k_timer_start(&blip_timer, K_MSEC(BLIP_ON_MS), K_NO_WAIT);
	return 0;
}

int pmic_blue_led_off(void)
{
	led_off(rgbleds, blue_led);
	return 0;
}

static uint8_t get_battery_pct(uint16_t battery_mv)
{
	for (int i = 0; i < (int)(sizeof(m_battery_table) / sizeof(battery_voltage_pct_t)); i++) {
		battery_voltage_pct_t upper_range = m_battery_table[i];
		if (battery_mv <= upper_range.mV) {
			return upper_range.percent;
		}
	}
	/*voltage is larger than max we defined, it must be 100% */
	return 100;
}

int pmic_get_info(pmic_get_info_t *info)
{
	struct sensor_value volt;
	struct sensor_value current;
	struct sensor_value temp;
	struct sensor_value charge;

	sensor_sample_fetch(charger);
	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &volt);
	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_AVG_CURRENT, &current);
	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_TEMP, &temp);
	sensor_channel_get(charger, SENSOR_CHAN_GAUGE_STATE_OF_CHARGE, &charge);

	info->charging_a = sensor_value_to_double(&current);
	info->battery_v = sensor_value_to_double(&volt);

	// TODO: use fuel guage for battery %, it doesn't seem to be returning valid %
	// float battery_pct = sensor_value_to_double(&charge);
	// LOG_DBG("battery_pct: %f", battery_pct);
	uint16_t battery_mv = info->battery_v * 1000;
	uint8_t battery_pct = get_battery_pct(battery_mv);
	info->battery_pct = (int8_t)battery_pct;

	info->temperature = sensor_value_to_double(&temp);

	return 0;
}

SHELL_CMD_REGISTER(show_batt_info, NULL, "battery Info", read_charger_sensors_stub);