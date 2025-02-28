/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <stdio.h>
#include <zephyr/drivers/sensor/npm1300_charger.h>
#include <zephyr/dt-bindings/regulator/npm1300.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/gpio.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/mfd/npm1300.h>

#include "nrf_fuel_gauge.h"
#include "pmic.h"
#include "pmic_leds.h"

LOG_MODULE_REGISTER(manuleds, LOG_LEVEL_ERR);

static const struct device *rgbleds = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_leds));

int init_pmic_leds()
{
    int rc = -1;
    if (!device_is_ready(rgbleds)) {
        LOG_ERR("PMIC LED device not ready.\n");
        return rc;
    }
    LOG_DBG("Found NPM1300 Led controller.  Good!!!");
    return 0;
}

static int set_led_color(SOLID_LED_COLORS_t color)
{
    switch (color) {
    case SOLID_LED_RED:
        led_on(rgbleds, 0);
        led_off(rgbleds, 1);
        led_off(rgbleds, 2);
        break;
    case SOLID_LED_GREEN:
        led_off(rgbleds, 0);
        led_on(rgbleds, 1);
        led_off(rgbleds, 2);
        break;
    case SOLID_LED_BLUE:
        led_off(rgbleds, 0);
        led_off(rgbleds, 1);
        led_on(rgbleds, 2);
        break;
    case SOLID_LED_OFF:
        led_off(rgbleds, 0);
        led_off(rgbleds, 1);
        led_off(rgbleds, 2);
        break;
    }
    return 0;
}

static bool            led_api_active = false;
static LED_API_STATE_t led_api_state;
static void            led_api_timer_handler(struct k_timer *dummy);
K_TIMER_DEFINE(led_api_timer, led_api_timer_handler, NULL);

static void led_api_work_handler(struct k_work *work);
K_WORK_DEFINE(led_api_work, led_api_work_handler);

static void led_api_work_handler(struct k_work *work)
{
    static int led_state = 1;
    switch (led_api_state) {
    case LED_BLE_ADV:
        if (led_state) {
            set_led_color(SOLID_LED_BLUE);
        } else {
            set_led_color(SOLID_LED_OFF);
        }
        led_state = !led_state;
        break;
    case LED_CHARGING:
    case LED_WIFI_TRYING:
    case LED_FOTA:
        if (led_state) {
            set_led_color(SOLID_LED_GREEN);
        } else {
            set_led_color(SOLID_LED_OFF);
        }
        led_state = !led_state;
        break;
#ifdef CONFIG_ML_CAPTURE_IMU_DATA
    case LED_REC_IMU:
        if (led_state) {
            set_led_color(SOLID_LED_RED);
        } else {
            set_led_color(SOLID_LED_OFF);
        }
        led_state = !led_state;
        break;
#endif
    default:
        break;
    }
}

static void led_api_timer_handler(struct k_timer *dummy)
{
    switch (led_api_state) {
    case LED_API_IDLE:
    case LED_API_ERROR:
        // continue doing this
        break;

    case LED_BLE_ADV:
        if (led_api_active) {    // continue doing this
            k_work_submit(&led_api_work);
            k_timer_start(&led_api_timer, K_MSEC(1000), K_MSEC(0));
        } else {
            set_led_color(SOLID_LED_OFF);
        }
        break;

    case LED_CHARGING:
        if (led_api_active) {    // continue doing this
            k_work_submit(&led_api_work);
            k_timer_start(&led_api_timer, K_MSEC(1000), K_MSEC(0));
        } else {
            set_led_color(SOLID_LED_OFF);
        }
        break;
    case LED_FOTA:
        if (led_api_active) {    // continue doing this
            k_work_submit(&led_api_work);
            k_timer_start(&led_api_timer, K_MSEC(500), K_MSEC(0));
        } else {
            set_led_color(SOLID_LED_OFF);
        }
        break;
    case LED_WIFI_TRYING:
        if (led_api_active) {    // continue doing this
            k_work_submit(&led_api_work);
            k_timer_start(&led_api_timer, K_MSEC(250), K_MSEC(0));
        } else {
            set_led_color(SOLID_LED_OFF);
        }
        break;
#ifdef CONFIG_ML_CAPTURE_IMU_DATA
    case LED_REC_IMU:
        if (led_api_active) {    // continue doing this
            k_work_submit(&led_api_work);
            k_timer_start(&led_api_timer, K_MSEC(100), K_MSEC(0));
        } else {
            set_led_color(SOLID_LED_OFF);
        }
        break;
#endif
    default:
        LOG_ERR("No handler for this LED API state");
        break;
    }
}

static void start_ble_ui(LED_API_STATE_t state)
{
    led_api_state = state;
    LOG_DBG("function %s, line %d", __FUNCTION__, __LINE__);
    switch (led_api_state) {
    case LED_BLE_ADV:
        led_api_active = true;
        k_timer_start(&led_api_timer, K_MSEC(500), K_MSEC(0));
        break;
    default:
        break;
    }
}

static void start_wifi_ui(LED_API_STATE_t state)
{
    led_api_state = state;
    LOG_DBG("function %s, line %d", __FUNCTION__, __LINE__);
    switch (led_api_state) {
    case LED_WIFI_TRYING:
        led_api_active = true;
        k_timer_start(&led_api_timer, K_MSEC(250), K_MSEC(0));
        break;
    default:
        break;
    }
}

static void start_charging_ui(LED_API_STATE_t state)
{
    led_api_state = state;
    switch (led_api_state) {
    case LED_CHARGING:
        led_api_active = true;
        k_timer_start(&led_api_timer, K_MSEC(1000), K_MSEC(0));
        break;
    case LED_CHARGED:
        k_timer_stop(&led_api_timer);
        set_led_color(SOLID_LED_GREEN);
        break;
    default:
        break;
    }
}

static void start_fota_ui(LED_API_STATE_t state)
{
    led_api_state = state;
    switch (led_api_state) {
    case LED_FOTA:
        led_api_active = true;
        k_timer_start(&led_api_timer, K_MSEC(500), K_MSEC(0));
        break;
    case LED_CHARGED:
        k_timer_stop(&led_api_timer);
        set_led_color(SOLID_LED_GREEN);
        break;
    default:
        break;
    }
}

#ifdef CONFIG_ML_CAPTURE_IMU_DATA
static void start_imu_recording_ui(LED_API_STATE_t state)
{
    led_api_state = state;
    switch (led_api_state) {
    case LED_REC_IMU:
        led_api_active = true;
        k_timer_start(&led_api_timer, K_MSEC(500), K_MSEC(0));
        break;
    default:
        k_timer_stop(&led_api_timer);
        set_led_color(SOLID_LED_OFF);
        break;
    }
}
#endif

bool        g_ble_active = false;
static bool should_we_proceed(LED_API_STATE_t *state_p)
{
    LED_API_STATE_t state              = *state_p;
    bool            _should_we_proceed = true;
    if (state == LED_BLE_ADV_DONE) {
        g_ble_active = false;
    }
    do {
        if (state == led_api_state) {
            _should_we_proceed = false;
            break;
        }
        if (led_api_state == LED_API_ERROR) {
            //Nothing can make it past this one, supersedes everything
            LOG_DBG("state %d cannot be superceded by %d", led_api_state, state);
            _should_we_proceed = false;
            break;
        }
#ifdef CONFIG_ML_CAPTURE_IMU_DATA
        if (state == LED_REC_IMU) {
            _should_we_proceed = true;
            break;
        }
#endif
        if ((led_api_state == LED_BLE_ADV)
            && ((state != LED_BLE_ADV_DONE) && (state != LED_WIFI_TRYING) && (state != LED_API_ERROR))) {
            LOG_DBG("state %d cannot be superceded by %d", led_api_state, state);
            _should_we_proceed = false;
            break;
        }
        if (state == LED_BLE_ADV_DONE) {
            //When BLE is DONE advertising, we might have to revert to charging
            if (get_charging_active() == true) {
                LOG_DBG("IN CHARGING STATE");
                state = LED_CHARGING;
            } else if (get_charging_complete() == true) {
                LOG_DBG("FULLY CHARGED STATE");
                state = LED_CHARGED;
            } else {
                LOG_DBG("OFF THE CHARGER");
                state = LED_API_IDLE;
            }
        }
        if (state == LED_VBUS_REMOVED) {
            //This normally means we STOP showing charging ( LED_IDLE) EXCEPT when the unit is advertising OR when
            if (g_ble_active == true) {
                LOG_DBG("BLE ON state %d cannot be superceded by %d", led_api_state, state);
                _should_we_proceed = false;
                break;
            }
        }
        if (led_api_state == LED_CHARGING
            && ((state == LED_CHARGED) || (state == LED_VBUS_REMOVED) || (state == LED_FOTA))) {
            //This is permitted
            _should_we_proceed = true;
        }
    } while (0);
    *state_p = state;
    return _should_we_proceed;
}

LED_API_STATE_t led_api_set_state(LED_API_STATE_t state)
{
    LED_API_STATE_t prev_state = led_api_state;
    LOG_DBG("function %s, line %d", __FUNCTION__, __LINE__);
    LOG_DBG("state %d, led_api_state %d", state, led_api_state);

    if (should_we_proceed(&state) == false) {
        LOG_DBG("Short circuted, return");
        return 0;
    } else {
        LOG_DBG("State returned %d", state);
    }

    led_api_active = false;
    k_timer_stop(&led_api_timer);
    led_api_state = state;
    switch (state) {
    case LED_API_IDLE:
        set_led_color(SOLID_LED_OFF);
        break;
    case LED_API_ERROR:
        set_led_color(SOLID_LED_RED);
        break;
    case LED_BLE_ADV:
        g_ble_active = true;
        start_ble_ui(state);
        break;
    case LED_BLE_ADV_DONE:
        g_ble_active = false;
        start_ble_ui(state);
        break;
    case LED_CHARGING:
        start_charging_ui(state);
        break;
    case LED_CHARGED:
        start_charging_ui(state);
        break;
    case LED_VBUS_REMOVED:
        set_led_color(SOLID_LED_OFF);
        break;
    case LED_WIFI_TRYING:
        start_wifi_ui(state);
        break;
    case LED_FOTA:
        start_fota_ui(state);
        break;
#ifdef CONFIG_ML_CAPTURE_IMU_DATA
    case LED_REC_IMU:
        LOG_WRN("Start Recording LED");
        start_imu_recording_ui(state);
        break;
#endif
    default:
        LOG_ERR("No handler for this LED API state (%d)", state);
        return (LED_API_STATE_t)-1;
    }
    return prev_state;
}

int shell_led_api(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        printf("led_api <api_err|ble_adv|charging|charged|wifi_try|fota>\n");
        return -1;
    }
    LED_API_STATE_t choice = LED_API_IDLE;
    if (strstr(argv[1], "api_idle") != NULL) {
        choice = LED_API_IDLE;
    } else if (strstr(argv[1], "api_err") != NULL) {    //RED, always
        choice = LED_API_ERROR;
    } else if (strstr(argv[1], "ble_adv") != NULL) {    //BLUE 0.5 on, 0.5 off
        choice = LED_BLE_ADV;
    } else if (strstr(argv[1], "charging") != NULL) {    //GREEN 1 on, 1 off
        choice = LED_CHARGING;
    } else if (strstr(argv[1], "charged") != NULL) {    //GREEN on
        choice = LED_CHARGED;
    } else if (strstr(argv[1], "wifi_try") != NULL) {    //GREEN 0.25 on, 0.25 off
        choice = LED_WIFI_TRYING;
    } else if (strstr(argv[1], "fota") != NULL) {    //GREEN 0.5 on, 0.5 off
        choice = LED_FOTA;
    }
#ifdef CONFIG_ML_CAPTURE_IMU_DATA
    else if (strstr(argv[1], "imu") != NULL) {
        choice = LED_REC_IMU;
    }
#endif

    printf("CHOICE WAS %s -> %d\n", argv[1], choice);
    led_api_set_state(choice);
    return 0;
}
#include <zephyr/shell/shell.h>
SHELL_CMD_REGISTER(led_api, NULL, "Commands to control LED API", shell_led_api);
