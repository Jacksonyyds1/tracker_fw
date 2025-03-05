/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#pragma once
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef enum
{
    SOLID_LED_OFF   = 0,
    SOLID_LED_RED   = 1,
    SOLID_LED_GREEN = 2,
    SOLID_LED_BLUE  = 3,
} SOLID_LED_COLORS_t;

typedef enum
{
    LED_API_IDLE,
    LED_API_ERROR,

    LED_BLE_ADV,
    LED_BLE_ADV_DONE,

    LED_CHARGING,
    LED_CHARGED,
    LED_VBUS_REMOVED,

    LED_WIFI_TRYING,

    LED_FOTA,
#ifdef CONFIG_ML_CAPTURE_IMU_DATA
    LED_REC_IMU,
#endif
} LED_API_STATE_t;

int init_pmic_leds();
/**
 * Set the LED state
 * @param state the new state
 * @returns the previous state
 */
LED_API_STATE_t led_api_set_state(LED_API_STATE_t state);
#ifdef __cplusplus
}
#endif
