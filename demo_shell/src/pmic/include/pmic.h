#pragma once
#include <zephyr/device.h>

typedef enum  {
    PMIC_SWITCH_VSYS = 0,
    PMIC_SWITCH_WIFI = 1,
} PMIC_SWITCHES_t;


typedef enum {
    PMIC_LED_OFF = 0,
    PMIC_LED_RED = 1,
    PMIC_LED_GREEN = 2,
    PMIC_LED_BLUE = 3,
    PMIC_LED_PURPLE = 4,
    PMIC_LED_YELLOW = 5,
    PMIC_LED_CYAN = 6,
} PMIC_LED_COLORS_t;

typedef struct {
    float voltage;
    float current;
    float temp;
    float soc;
    float tte;
    float ttf;    
} fuel_gauge_info_t;

int pmic_init(void);
int pmic_write_i2c(uint16_t addr, uint8_t val);
int pmic_read_i2c(uint16_t addr, uint8_t *buf);

int set_switch_state(PMIC_SWITCHES_t pwr_switch, bool newState);
int set_led_color(PMIC_LED_COLORS_t color);
int fuel_gauge_init(const struct device *charger);
int fuel_gauge_get_latest(fuel_gauge_info_t* latest);