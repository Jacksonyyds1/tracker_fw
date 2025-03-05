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
#include "modem.h"

static float max_charge_current;
static float term_charge_current;
static int64_t ref_time;

static const struct battery_model battery_model = {
#include "battery_model.inc"
};

LOG_MODULE_REGISTER(pmic, LOG_LEVEL_DBG);


#define PMIC_GPIO_BASE_ADDR 0x06
#define PMIC_GPIO_2_REG 0x2

#define PMIC_LDO_BASE_ADDR 0x08
#define PMIC_LDO_STATUS_REG 0x04
#define LDSW_OFFSET_EN_SET 0x0
#define LDSW_OFFSET_EN_CLR 0x1

#define PMIC_EVENT_BASE_ADDR 0x0
#define PMIC_EVENT_CHARGER1_SET 0xC
#define PMIC_EVENT_CHARGER1_GET 0xb
#define PMIC_EVENT_CHARGER1_CLR 0xb
#define PMIC_EVENT_CHARGER2_SET 0x10
#define PMIC_EVENT_CHARGER2_GET 0xf
#define PMIC_EVENT_CHARGER2_CLR 0xf
#define PMIC_EVENT_GPIO_SET 0x24
#define PMIC_EVENT_GPIO_FORCE 0x22
#define PMIC_EVENT_GPIO_CLR 0x23
#define PMIC_EVENT_VBUS_GET 0x17
#define PMIC_EVENT_VBUS_SET 0x18
#define PMIC_EVENT_VBUS_CLR 0x17

#define PMIC_CHARGE1_STATUS_SUPP_MASK 0x1
#define PMIC_CHARGE1_STATUS_TRICKLE_STARTED_MASK 0x2
#define PMIC_CHARGE1_STATUS_CONST_CURR_STARTED_MASK 0x4
#define PMIC_CHARGE1_STATUS_CONST_VOLT_STARTED_MASK 0x8
#define PMIC_CHARGE1_STATUS_CHARGE_COMPLETED_MASK 0x10
#define PMIC_CHARGE1_STATUS_ERROR_MASK 0x20

#define PMIC_CHARGE2_STATUS_BATT_DETECTED_MASK 0x1
#define PMIC_CHARGE2_STATUS_BATT_REMOVED_MASK 0x2
#define PMIC_CHARGE2_STATUS_BATT_NEEDS_CHARGE_MASK 0x4

#define PMIC_CHARGE2_STATUS_VBUS_DETECTED_MASK 0x1
#define PMIC_CHARGE2_STATUS_VBUS_REMOVED_MASK 0x2





static const struct device *regulators = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_regulators));
static const struct device *LDO1 = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_ldo1));
static const struct device *LDO2 = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_ldo2));
static const struct device *BUCK1 = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_buck1));  // left for future, not currently needed
//static const struct device *BUCK2 = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_buck2));  // left for future, not currently needed

static const struct device *charger = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_charger));
static const struct device *rgbleds = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_leds));
static const struct device *mfd = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_pmic));
static const struct gpio_dt_spec pmic_int = GPIO_DT_SPEC_GET(DT_ALIAS(pmicint), gpios);
static struct gpio_callback pmic_int_cb;

int fuel_gauge_get_latest(fuel_gauge_info_t* latest);

void batt_info_work_handler(struct k_work *work)
{
    fuel_gauge_info_t batt_info;
    fuel_gauge_get_latest(&batt_info);
    uint8_t *data;
    data = (uint8_t*)&batt_info.soc;
    modem_send_command(MESSAGE_TYPE_BATTERY_LEVEL, 0x1, data, sizeof(float));
}

K_WORK_DEFINE(batt_info_work, batt_info_work_handler);

void batt_info_timer_handler(struct k_timer *dummy)
{
    k_work_submit(&batt_info_work);
}

K_TIMER_DEFINE(batt_info_timer, batt_info_timer_handler, NULL);


void pmic_int_work_handler(struct k_work *work)
{
    uint8_t buf[1];
    mfd_npm1300_reg_read(mfd, PMIC_EVENT_BASE_ADDR, PMIC_EVENT_CHARGER1_GET, buf);

    if (buf[0] & PMIC_CHARGE1_STATUS_SUPP_MASK) {
        LOG_DBG("Supplement Mode Started");
        set_led_color(PMIC_LED_GREEN);
    }
    else if (buf[0] & PMIC_CHARGE1_STATUS_TRICKLE_STARTED_MASK) {
        LOG_DBG("Trickle Charging Started");
        set_led_color(PMIC_LED_GREEN);
    }
    else if (buf[0] & PMIC_CHARGE1_STATUS_CONST_CURR_STARTED_MASK) {
        LOG_DBG("Const Curr Charging Started");
        set_led_color(PMIC_LED_GREEN);
    }
    else if (buf[0] & PMIC_CHARGE1_STATUS_CONST_VOLT_STARTED_MASK) {
        LOG_DBG("Const V Charging Started");
        set_led_color(PMIC_LED_GREEN);
    }
    else if (buf[0] & PMIC_CHARGE1_STATUS_CHARGE_COMPLETED_MASK) {
        LOG_DBG("Charging Stopped");
        set_led_color(PMIC_LED_OFF);
    }
    else if (buf[0] & PMIC_CHARGE1_STATUS_ERROR_MASK) {
        LOG_DBG("Charging Error");
        set_led_color(PMIC_LED_RED);
    }

    if (buf[0] != 0x0) {
        // ack/clear all the charger1 events 
        mfd_npm1300_reg_write(mfd, PMIC_EVENT_BASE_ADDR, PMIC_EVENT_CHARGER1_CLR, buf[0]);
    }

    mfd_npm1300_reg_read(mfd, PMIC_EVENT_BASE_ADDR, PMIC_EVENT_VBUS_GET, buf);

    if (buf[0] & PMIC_CHARGE2_STATUS_VBUS_DETECTED_MASK) {
        LOG_DBG("Vbus Detected");
        //set_led_color(PMIC_LED_OFF);   // leaving here for now, till we see what/if any color we want here.
    }
    else if (buf[0] & PMIC_CHARGE2_STATUS_VBUS_REMOVED_MASK) {
        LOG_DBG("Vbus Removed");
        set_led_color(PMIC_LED_OFF);
    }


    if (buf[0] != 0x0) {
        // ack/clear all the charger2 events 
        mfd_npm1300_reg_write(mfd, PMIC_EVENT_BASE_ADDR, PMIC_EVENT_VBUS_CLR, buf[0]);
    }

    // set charging_enable?
}
K_WORK_DEFINE(pmic_int_work, pmic_int_work_handler);


void pmic_int_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    k_work_submit(&pmic_int_work);
}



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

    if (!fuel_gauge_init(charger)) {
        LOG_DBG("Fuel gauge initialized.");
    }
    else {
        LOG_ERR("Fuel gauge initialization failed.");
        return rc;
    }

    if (!device_is_ready(rgbleds))
    {
        LOG_ERR("PMIC LED device not ready.\n");
        return rc;
    }
    LOG_DBG("Found NPM1300 Led controller.  Good!!!");

    regulator_set_mode(LDO1, NPM1300_LDSW_MODE_LDSW);
    regulator_set_mode(LDO2, NPM1300_LDSW_MODE_LDSW);

    // force back to 1.8V.  Set to 1.9V in dts as workaround for npm1300 errata #27
    regulator_set_voltage(BUCK1, 1800000, 1800000);
    

    if (gpio_pin_configure_dt(&pmic_int, GPIO_INPUT) != 0)
    {
        LOG_ERR("Error: failed to configure %s pin %d\n",
            pmic_int.port->name, pmic_int.pin);
        return rc;
    }
    // enable interrupt on button for rising edge
    if (gpio_pin_interrupt_configure_dt(&pmic_int, GPIO_INT_EDGE_TO_ACTIVE) != 0) {
        LOG_ERR("Error: failed to configure interrupt on %s pin %d\n",
            pmic_int.port->name, pmic_int.pin);
        return rc;
    }
    // initialize callback structure for button interrupt
    gpio_init_callback(&pmic_int_cb, pmic_int_handler, BIT(pmic_int.pin));

    // attach callback function to button interrupt
    gpio_add_callback(pmic_int.port, &pmic_int_cb);

    // set interrupts on pmic
    mfd_npm1300_reg_write(mfd, PMIC_GPIO_BASE_ADDR, 0x11, 0x1);
    mfd_npm1300_reg_write(mfd, PMIC_GPIO_BASE_ADDR, PMIC_GPIO_2_REG, 0x5); // set GPIO1 as INT out

    mfd_npm1300_reg_write(mfd, PMIC_EVENT_BASE_ADDR, PMIC_EVENT_CHARGER1_SET, 0x4); // all charge events will trigger INT
    mfd_npm1300_reg_write(mfd, PMIC_EVENT_BASE_ADDR, PMIC_EVENT_VBUS_SET, 0x3); // all charge2 events will trigger INT

    mfd_npm1300_reg_write(mfd, PMIC_EVENT_BASE_ADDR, PMIC_EVENT_CHARGER1_CLR, 0xff); // clear all charge events
    mfd_npm1300_reg_write(mfd, PMIC_EVENT_BASE_ADDR, PMIC_EVENT_VBUS_CLR, 0xff); // clear all charge events

    LOG_DBG("Found NPM1300 LED/GPIO/MFD.  Good!!!");

    regulator_set_mode(LDO1, NPM1300_LDSW_MODE_LDSW);
    regulator_set_mode(LDO2, NPM1300_LDSW_MODE_LDSW);

    return 0;
}



int set_switch_state(PMIC_SWITCHES_t pwr_switch, bool newState)
{
    int rc = -1;
    uint8_t retryCount = 0;
    uint8_t buf[1];
    mfd_npm1300_reg_read(mfd, PMIC_LDO_BASE_ADDR, PMIC_LDO_STATUS_REG, buf);
    bool LDO_VSYS_enabled = (buf[0] & 0x1) == 0x1;
    bool LDO_WIFI_enabled = (buf[0] & 0x4) == 0x4;

    switch (pwr_switch) {
        case PMIC_SWITCH_VSYS:
            if (newState && !LDO_VSYS_enabled) {
                while(!LDO_VSYS_enabled) {
                    rc = mfd_npm1300_reg_write(mfd, PMIC_LDO_BASE_ADDR, LDSW_OFFSET_EN_SET, 1U);
                    mfd_npm1300_reg_read(mfd, PMIC_LDO_BASE_ADDR, PMIC_LDO_STATUS_REG, buf);
                    LDO_VSYS_enabled = (buf[0] & 0x1) == 0x1;
                    retryCount++;
                    if (retryCount > 10) {
                        printk("Error: Failed to enable LDO1\n");
                        break;
                    }
                }
            }
            else if (!newState && LDO_VSYS_enabled){
                while(LDO_VSYS_enabled) {
                    rc = mfd_npm1300_reg_write(mfd, PMIC_LDO_BASE_ADDR, LDSW_OFFSET_EN_CLR, 1U);
                    mfd_npm1300_reg_read(mfd, PMIC_LDO_BASE_ADDR, PMIC_LDO_STATUS_REG, buf);
                    LDO_VSYS_enabled = (buf[0] & 0x1) == 0x1;
                    retryCount++;
                    if (retryCount > 10) {
                        printk("Error: Failed to disable LDO1\n");
                        break;
                    }
                }
            }
            break;
        case PMIC_SWITCH_WIFI:
            if (newState && !LDO_WIFI_enabled) {
                while(!LDO_WIFI_enabled) {
                    rc = mfd_npm1300_reg_write(mfd, PMIC_LDO_BASE_ADDR, LDSW_OFFSET_EN_SET + 2U, 1U); 
                    mfd_npm1300_reg_read(mfd, PMIC_LDO_BASE_ADDR, PMIC_LDO_STATUS_REG, buf);
                    LDO_WIFI_enabled = (buf[0] & 0x4) == 0x4;
                    retryCount++;
                    if (retryCount > 10) {
                        printk("Error: Failed to enable LDO2\n");
                        break;
                    }
                }
            }
            else if (!newState && LDO_WIFI_enabled){
                while (LDO_WIFI_enabled) {
                    rc = mfd_npm1300_reg_write(mfd, PMIC_LDO_BASE_ADDR, LDSW_OFFSET_EN_CLR + 2U, 1U);
                    mfd_npm1300_reg_read(mfd, PMIC_LDO_BASE_ADDR, PMIC_LDO_STATUS_REG, buf);
                    LDO_WIFI_enabled = (buf[0] & 0x4) == 0x4;
                    retryCount++;
                    if (retryCount > 10) {
                        printk("Error: Failed to disable LDO2\n");
                        break;
                    }
                }
                
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


// code below copied from ncs 2.5 samples/driers/pmic/native/npm1300_fuel_gauge and changed to hit here where needed.
static int read_fg_sensors(const struct device *charger, float *voltage, float *current, float *temp)
{
    struct sensor_value value;
    int ret = -1;

    ret = sensor_sample_fetch(charger);
    if (ret < 0) {
        return ret;
    }

    if (charger == NULL || voltage == NULL || current == NULL || temp == NULL) { return ret; }

    sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &value);
    *voltage = (float)value.val1 + ((float)value.val2 / 1000000);

    sensor_channel_get(charger, SENSOR_CHAN_GAUGE_TEMP, &value);
    *temp = (float)value.val1 + ((float)value.val2 / 1000000);

    sensor_channel_get(charger, SENSOR_CHAN_GAUGE_AVG_CURRENT, &value);
    *current = (float)value.val1 + ((float)value.val2 / 1000000);

    return 0;
}

int fuel_gauge_init(const struct device *charger)
{
    struct sensor_value value;
    struct nrf_fuel_gauge_init_parameters parameters = { .model = &battery_model };
    int ret;

    ret = read_fg_sensors(charger, &parameters.v0, &parameters.i0, &parameters.t0);
    if (ret < 0) {
        return ret;
    }

    /* Store charge nominal and termination current, needed for ttf calculation */
    sensor_channel_get(charger, SENSOR_CHAN_GAUGE_DESIRED_CHARGING_CURRENT, &value);
    max_charge_current = (float)value.val1 + ((float)value.val2 / 1000000);
    term_charge_current = max_charge_current / 10.f;

    nrf_fuel_gauge_init(&parameters, NULL);

    ref_time = k_uptime_get();

    return 0;
}


int fuel_gauge_get_latest(fuel_gauge_info_t* latest)
{
    float voltage;
    float current;
    float temp;
    float soc;
    float tte;
    float ttf;
    float delta;
    int ret;

    ret = read_fg_sensors(charger, &voltage, &current, &temp);
    if (ret < 0) {
        printk("Error: Could not read from charger device\n");
        return ret;
    }

    delta = (float) k_uptime_delta(&ref_time) / 1000.f;

    soc = nrf_fuel_gauge_process(voltage, current, temp, delta, NULL);
    tte = nrf_fuel_gauge_tte_get();
    ttf = nrf_fuel_gauge_ttf_get(-max_charge_current, -term_charge_current);

    // printk("V: %.3f, I: %.3f, T: %.2f, ", voltage, current, temp);
    // printk("SoC: %.2f, TTE: %.0f, TTF: %.0f\n", soc, tte, ttf);

    latest->voltage = voltage;
    latest->current = current;
    latest->temp = temp;
    latest->soc = soc;
    latest->tte = tte;
    latest->ttf = ttf;
    return 0;
}


int pmic_write_i2c(uint16_t addr, uint8_t val)
{
    uint8_t addr1 = (addr >> 8) & 0xFF;
    uint8_t addr2 = addr & 0xFF;
    return mfd_npm1300_reg_write(mfd, addr1, addr2, val);
}

int pmic_read_i2c(uint16_t addr, uint8_t *buf)
{
    uint8_t addr1 = (addr >> 8) & 0xFF;
    uint8_t addr2 = addr & 0xFF;
    return mfd_npm1300_reg_read(mfd, addr1, addr2, buf);
}