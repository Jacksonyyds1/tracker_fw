/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#pragma once
#include <zephyr/device.h>

typedef enum
{
    PMIC_SWITCH_VSYS = 0,
    PMIC_SWITCH_WIFI = 1,
} PMIC_SWITCHES_t;

typedef struct
{
    float voltage;
    float current;
    float temp;
    float soc;
    float tte;
    float ttf;
} fuel_gauge_info_t;

typedef enum
{
    PMIC_CHARGING_STARTED,
    PMIC_CHARGING_STOPPED,
    PMIC_ERROR,
} pmic_state_t;

typedef enum
{
    PMIC_NTCCOLD_THRESHOLD    = 0xA4,
    PMIC_NTCCOLDLSB_THRESHOLD = 0x02,
    PMIC_NTCCOOL_THRESHOLD    = 0x91,
    PMIC_NTCCOOLLSB_THRESHOLD = 0x00,
    PMIC_NTCWARM_THRESHOLD    = 0x54,
    PMIC_NTCWARMLSB_THRESHOLD = 0x01,
    PMIC_NTCHOT_THRESHOLD     = 0x54,
    PMIC_NTCHOTLSB_THRESHOLD  = 0x01,

} pmic_temp_thresholds_t;

typedef enum
{
    PMIC_LTE_POWER_ON  = 0,
    PMIC_LTE_POWER_OFF = 1,
    PMIC_LTE_SOFT_OFF  = 2,
} PMIC_LTE_POWER_STATE_t;

typedef enum
{
    TOO_COLD,
    JUST_RIGHT,
    TOO_HOT
} goldilocks_t;

typedef int (*pmic_state_change_cb_t)(pmic_state_t new_state);
typedef int (*pmic_temp_change_cb_t)(goldilocks_t cur_state);
typedef int (*pmic_batt_shutdown_cb_t)(void);

int pmic_init(bool turn_on_lte, bool turn_on_wifi);
int pmic_write_i2c(uint16_t addr, uint8_t val);
int pmic_read_i2c(uint16_t addr, uint8_t *buf);

int                    set_switch_state(PMIC_SWITCHES_t pwr_switch, bool newState);
int                    fuel_gauge_init(const struct device *charger);
int                    fuel_gauge_get_latest(fuel_gauge_info_t *latest);
int                    pmic_read_chip_id();
bool                   get_charging_active();
bool                   get_charging_complete();
bool                   get_vbus_present();
int                    pmic_read_NTC_resistor_val();
int                    set_ship_hold(bool state, const char *reason);
int                    pmic_state_set_callback(pmic_state_change_cb_t cb);
int                    pmic_temp_set_callback(pmic_temp_change_cb_t cb);
int                    pmic_shutdown_set_callback(pmic_batt_shutdown_cb_t cb);
int                    pmic_reboot(const char *reason);
PMIC_LTE_POWER_STATE_t pmic_is_9160_powered();
int                    pmic_power_off_modem(bool now);
int                    pmic_power_on_modem();
void                   pmic_set_modem_low_power_state();
uint8_t                pmic_read_register_val(uint8_t base, uint8_t reg);
int                    pmic_save_reset_reason(const char *reason);
const char            *pmic_get_reset_reason(void);
