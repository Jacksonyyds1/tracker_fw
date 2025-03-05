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


LOG_MODULE_REGISTER(pmic, LOG_LEVEL_DBG);

#include "pmic.h"

static const struct device *regulators = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_regulators));
static const struct device *LDO1 = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_ldo1));
static const struct device *LDO2 = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_ldo2));
static const struct device *BUCK1 = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_buck1));  // left for future, not currently needed
//static const struct device *BUCK2 = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_buck2));  // left for future, not currently needed

static const struct device *charger = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_charger));
static const struct device *rgbleds = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_leds));



/* IMU Initialization */
int pmic_init(void)
{
    int rc = -1;

    LOG_DBG("PMIC Init ...");

    if (!device_is_ready(regulators))
    {
        LOG_ERR("Error: Regulator device is not ready\n");
        return rc;
    }
    LOG_DBG("Found NPM1300 Regulator.  Good!!!");

    if (!device_is_ready(charger))
    {
        LOG_ERR("Charger device not ready.\n");
        return rc;
    }
    LOG_DBG("Found NPM1300 charger.  Good!!!");

    if (!device_is_ready(rgbleds))
    {
        LOG_ERR("PMIC LED device not ready.\n");
        return rc;
    }

    regulator_set_mode(LDO1, NPM1300_LDSW_MODE_LDSW);
    regulator_set_mode(LDO2, NPM1300_LDSW_MODE_LDSW);

    // force back to 1.8V.  Set to 1.9V in dts as workaround for npm1300 errata #27
    regulator_set_voltage(BUCK1, 1800000, 1800000);
    

    return 0;
}


int set_switch_state(PMIC_SWITCHES_t pwr_switch, bool newState)
{
    int rc = -1;

    switch (pwr_switch) {
        case PMIC_SWITCH_VSYS:
            if (newState) {
                regulator_enable(LDO1);
            }
            else {
                regulator_disable(LDO1);
            }
            break;
        case PMIC_SWITCH_WIFI:
            if (newState) {
                regulator_enable(LDO2);
            }
            else {
                regulator_disable(LDO2);
            }
            break;
    }

    return rc;
}

int set_led_color(PMIC_LED_COLORS_t color) {
    switch (color) {
        case PMIC_LED_RED:
            led_on(rgbleds, 0);
            led_off(rgbleds, 1);
            led_off(rgbleds, 2);
            break;
        case PMIC_LED_GREEN:
            led_off(rgbleds, 0);
            led_on(rgbleds, 1);
            led_off(rgbleds, 2);
            break;
        case PMIC_LED_BLUE:
            led_off(rgbleds, 0);
            led_off(rgbleds, 1);
            led_on(rgbleds, 2);
            break;
        case PMIC_LED_PURPLE:
            led_on(rgbleds, 0);
            led_off(rgbleds, 1);
            led_on(rgbleds, 2);
            break;
        case PMIC_LED_YELLOW:
            led_on(rgbleds, 0);
            led_on(rgbleds, 1);
            led_off(rgbleds, 2);
            break;
        case PMIC_LED_CYAN:
            led_off(rgbleds, 0);
            led_on(rgbleds, 1);
            led_on(rgbleds, 2);
            break;
        case PMIC_LED_OFF:
            led_off(rgbleds, 0);
            led_off(rgbleds, 1);
            led_off(rgbleds, 2);
            break;
        
    }
    return 0;
}




