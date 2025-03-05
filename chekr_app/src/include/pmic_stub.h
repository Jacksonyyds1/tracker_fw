#pragma once
#include <stdint.h>

typedef struct {
	uint8_t battery_pct;
	float battery_v;
	float charging_a;
	float temperature;
} pmic_get_info_t;

int pmic_toggle_blue_led(void);
int pmic_blip_blue_led(void);
int pmic_blue_led_off(void);
int pmic_get_info(pmic_get_info_t *info);
