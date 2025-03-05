/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/shell/shell.h>
#include <stdio.h>
#include <zephyr/drivers/sensor/npm1300_charger.h>
#include <zephyr/dt-bindings/regulator/npm1300.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/fs/fs.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/mfd/npm1300.h>
#include <zephyr/zbus/zbus.h>
#if !defined(CONFIG_AVOID_ZBUS)
#include <d1_zbus.h>
#endif
#include "nrf_fuel_gauge.h"
#include "pmic.h"
#include "pmic_leds.h"
#include "modem.h"
#include "uicr.h"
#include "wi.h"
#if defined(CONFIG_REBOOT_ON_USB_CONNECT_TIME_IN_SECONDS)
#include <zephyr/sys/reboot.h>
#endif
#include <zephyr/sys/util.h>
LOG_MODULE_REGISTER(pmic, CONFIG_PMIC_LOG_LEVEL);

#include "modem.h"

static float   max_charge_current;
static float   term_charge_current;
static int64_t ref_time;

static const struct battery_model battery_model = {
#include "battery_model.inc"
};

static goldilocks_t last_temp_alert = JUST_RIGHT;

#define PMIC_GPIO_BASE_ADDR 0x06
#define PMIC_GPIO_2_REG     0x2

#define PMIC_LDO_BASE_ADDR  0x08
#define PMIC_LDO_STATUS_REG 0x04
#define LDSW_OFFSET_EN_SET  0x0
#define LDSW_OFFSET_EN_CLR  0x1

#define PMIC_EVENT_BASE_ADDR    0x0
#define PMIC_EVENT_CHARGER1_SET 0xC
#define PMIC_EVENT_CHARGER1_GET 0xb
#define PMIC_EVENT_CHARGER1_CLR 0xb
#define PMIC_EVENT_CHARGER2_SET 0x10
#define PMIC_EVENT_CHARGER2_GET 0xf
#define PMIC_EVENT_CHARGER2_CLR 0xf
#define PMIC_EVENT_GPIO_SET     0x24
#define PMIC_EVENT_GPIO_FORCE   0x22
#define PMIC_EVENT_GPIO_CLR     0x23
#define PMIC_EVENT_VBUS_GET     0x17
#define PMIC_EVENT_VBUS_SET     0x18
#define PMIC_EVENT_VBUS_CLR     0x17

#define PMIC_CHARGE1_STATUS_SUPP_MASK               0x1
#define PMIC_CHARGE1_STATUS_TRICKLE_STARTED_MASK    0x2
#define PMIC_CHARGE1_STATUS_CONST_CURR_STARTED_MASK 0x4
#define PMIC_CHARGE1_STATUS_CONST_VOLT_STARTED_MASK 0x8
#define PMIC_CHARGE1_STATUS_CHARGE_COMPLETED_MASK   0x10
#define PMIC_CHARGE1_STATUS_ERROR_MASK              0x20

#define PMIC_CHARGE2_STATUS_BATT_DETECTED_MASK     0x1
#define PMIC_CHARGE2_STATUS_BATT_REMOVED_MASK      0x2
#define PMIC_CHARGE2_STATUS_BATT_NEEDS_CHARGE_MASK 0x4

#define PMIC_CHARGE2_STATUS_VBUS_DETECTED_MASK 0x1
#define PMIC_CHARGE2_STATUS_VBUS_REMOVED_MASK  0x2

#define PMIC_LDO_STATUS_BASE_ADDR 0x08
#define PMIC_LDO_STATUS_REG       0x04
#define PMIC_LDO_CFG_REG          0x07
#define PMIC_LDO_STATUS_LDO1_MASK 0x1
#define PMIC_LDO_STATUS_LDO2_MASK 0x4

#define PMIC_SHIP_BASE_ADDR  0xB
#define PMIC_SHIP_ENABLE_REG 0x2
#define PMIC_SHIP_CFG_REG    0x4
#define PMIC_SHIP_STROBE_REG 0x1
#define PMIC_SHIP_STAT_REG   0x5

#define PMIC_ENABLE_SHIPMODE_MASK 0x1

#define PMIC_BUCKCTRL_BASE_ADDR    0x4
#define PMIC_BUCKCTRL0_VOLTAGE_REG 0x15

#define PMIC_ADC_BASE_ADDR 0x05
#define PMIC_ADC_NTC_REG   0x0a

#define PMIC_BCHARGER_BASE_ADDR 0x3
#define PMIC_NTCCOLD_REG        0x10
#define PMIC_NTCCOLDLSB_REG     0x11
#define PMIC_NTCCOOL_REG        0x12
#define PMIC_NTCCOOLLSB_REG     0x13
#define PMIC_NTCWARM_REG        0x14
#define PMIC_NTCWARMLSB_REG     0x15
#define PMIC_NTCHOT_REG         0x16
#define PMIC_NTCHOTLSB_REG      0x17
#define PMIC_CHARGE_STATUS_REG  0x34

#define PMIC_VBUS_BASE_ADDR  0x2
#define PMIC_VBUS_STATUS_REG 0x7

static const struct device *regulators = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_regulators));
static const struct device *LDO1       = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_ldo1));
static const struct device *LDO2       = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_ldo2));
static const struct device *BUCK1 =
    DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_buck1));    // left for future, not currently needed
// static const struct device *BUCK2 = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_buck2));  // left for
// future, not currently needed

static const struct device      *charger  = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_charger));
static const struct device      *mfd      = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_pmic));
static const struct gpio_dt_spec pmic_int = GPIO_DT_SPEC_GET(DT_ALIAS(pmicint), gpios);
static struct gpio_callback      pmic_int_cb;

typedef enum _charge_state
{
    CHARGING_IDLE,
    CHARGING_ACTIVE,
    CHARGING_ERROR,
    CHARGING_COMPLETE,
} charge_status;
static charge_status charging_status = CHARGING_IDLE;

static bool                    vbus_present = false;
static pmic_state_change_cb_t  state_cb;
static pmic_temp_change_cb_t   temp_cb;
static pmic_batt_shutdown_cb_t batt_cb;
static PMIC_LTE_POWER_STATE_t  modem_power_state = PMIC_LTE_POWER_ON;

struct k_work_q pmic_work_q;
K_THREAD_STACK_DEFINE(pmic_stack_area, 2048);

typedef enum
{
    PMIC_WORK_BATTERY_STATE_UPDATE = 0,
    PMIC_WORK_SHUTDOWN_LDO_1       = 1,
} PMIC_WORK_TYPES_t;

typedef struct
{
    workref_t        *work;
    PMIC_WORK_TYPES_t type;
    void             *data;
} pmic_work_t;

#if defined(CONFIG_REBOOT_ON_USB_CONNECT_TIME_IN_SECONDS)
int64_t last_usb_connect_time = 0;
#endif

int fuel_gauge_get_latest(fuel_gauge_info_t *latest);

uint8_t pmic_read_register_val(uint8_t base, uint8_t reg)
{
    uint8_t buf[1];
    mfd_npm1300_reg_read(mfd, base, reg, buf);
    return buf[0];
}

//  MUST BE CALLED WHEN VBUS IS NOT PRESENT
// TODO at start get VBUS state and update as needed to guard this.
int set_ship_hold(bool state, const char *reason)
{
    uint8_t buf[1];
    mfd_npm1300_reg_read(mfd, PMIC_SHIP_BASE_ADDR, PMIC_SHIP_STAT_REG, buf);
    // LOG_DBG("Ship mode pin state: %d", buf[0]);
    if (state) {
        pmic_save_reset_reason(reason);
        buf[0] = PMIC_ENABLE_SHIPMODE_MASK;
    } else {
        buf[0] = 0;
    }
    LOG_DBG("Setting ship mode to %d", buf[0]);

    mfd_npm1300_reg_write(mfd, PMIC_SHIP_BASE_ADDR, PMIC_SHIP_ENABLE_REG, buf[0]);
    return 0;
}

#define PMIC_VBUS_STATE_BASE_ADDR 0x2
#define PMIC_VBUS_STATUS_GET      0x7
void batt_info_work_handler(struct k_work *work)
{
    workref_t   *wr  = CONTAINER_OF(work, workref_t, work);
    pmic_work_t *msg = (pmic_work_t *)wr->reference;

    // there used to be more than one type of message ... but now there's just one and
    // we know what it is so we don't need the allocated message!
    k_free(msg);

    fuel_gauge_info_t batt_info;
    fuel_gauge_get_latest(&batt_info);
#if !defined(CONFIG_AVOID_ZBUS)
    float data = batt_info.soc;
    int   err  = zbus_chan_pub(&BATTERY_PERCENTAGE_UPDATE, &data, K_SECONDS(1));
    if (err) {
        LOG_ERR("zbus_chan_pub, error: %d", err);
        wr_put(wr);
        return;
    }
    static bool send_initial_vbus_state = false;

    if (send_initial_vbus_state == false) {
        send_initial_vbus_state = true;
        uint8_t reg             = 0;
        mfd_npm1300_reg_read(mfd, PMIC_VBUS_STATE_BASE_ADDR, PMIC_VBUS_STATUS_GET, &reg);
        vbus_present = (reg & 0x01);
        //LOG_WRN("Initial VBUS state: %d", vbus_present);
        err = zbus_chan_pub(&USB_POWER_STATE_UPDATE, &vbus_present, K_SECONDS(1));
        if (err) {
            LOG_ERR("zbus_chan_pub, error: %d", err);
            wr_put(wr);
            return;
        }
    }
#endif

    if (batt_info.temp <= 0) {
        LOG_DBG("No battery found.");
    } else {
        // check temp alerts if not plugged in
        if (!get_vbus_present()) {
            goldilocks_t curr_temp_range;
            if (batt_info.temp < CONFIG_MIN_ALERT_BATTERY_TEMP / 10.0) {
                curr_temp_range = TOO_COLD;
            } else if (batt_info.temp > CONFIG_MAX_ALERT_BATTERY_TEMP / 10.0) {
                curr_temp_range = TOO_HOT;
            } else {
                curr_temp_range = JUST_RIGHT;
            }
            if (last_temp_alert != curr_temp_range && temp_cb) {
                temp_cb(curr_temp_range);
            }
            last_temp_alert = curr_temp_range;
        }
        // check limits
        float max_batt_temp = CONFIG_MAX_OPERATIONAL_BATTERY_TEMP / 10.0;
        float min_batt_temp = CONFIG_MIN_OPERATIONAL_BATTERY_TEMP / 10.0;
        if ((batt_info.temp > max_batt_temp) || (batt_info.temp < min_batt_temp)) {
            if (uicr_shipping_flag_get()) {
                if (batt_cb) {
                    batt_cb();
                }
                LOG_ERR("Battery Temp out of range: %f", batt_info.temp);
                set_ship_hold(true, "Too hot or too cold");
            }
        }

        float min_v_for_9160 = CONFIG_MIN_VOLTAGE_FOR_9160 / 10.0;
        float min_v_for_5340 = CONFIG_MIN_VOLTAGE_FOR_5340 / 10.0;
        if (batt_info.voltage < min_v_for_5340) {
            if (uicr_shipping_flag_get()) {
                LOG_ERR("battery too low for 5340. Power down");
                LOG_PANIC();
                set_ship_hold(true, "Battery dead");
            }
        }
        if (batt_info.voltage < min_v_for_9160) {
            if (uicr_shipping_flag_get()) {
                LOG_ERR("Battery Voltage too low for 9160, turning it off: %f", batt_info.voltage);
                if (modem_power_state == PMIC_LTE_POWER_ON) {
                    // pmic_power_off_modem(true);
                    modem_power_off();
                    modem_power_state = PMIC_LTE_POWER_OFF;    // a slightly diff state than
                                                               // Soft_off, represents that the
                                                               // PMIC wanted it off for low V
                                                               // reasons, not the business
                                                               // logic
                }
            }
        } else if (batt_info.voltage > (min_v_for_9160 + 0.1)) {    // added some pad to prevent rapid on/off
            if (modem_power_state == PMIC_LTE_POWER_OFF) {
                LOG_INF("Battery Voltage is good for 9160, turning it on: %f", batt_info.voltage);
                modem_power_on();
            }
        }
    }
    wr_put(wr);
}

void batt_info_timer_handler(struct k_timer *dummy)
{
    pmic_work_t *batt_work = k_malloc(sizeof(pmic_work_t));
    if (batt_work == NULL) {
        LOG_ERR("k_malloc failed");
        return;
    }
    batt_work->work = wr_get(batt_work, __LINE__);
    if (batt_work->work == NULL) {
        LOG_ERR("out of workrefs!");
        k_free(batt_work);
        return;
    }
    batt_work->type = PMIC_WORK_BATTERY_STATE_UPDATE;
    k_work_init(&batt_work->work->work, batt_info_work_handler);

    k_work_submit_to_queue(&pmic_work_q, &batt_work->work->work);
}

K_TIMER_DEFINE(batt_info_timer, batt_info_timer_handler, NULL);

void pmic_int_work_handler(struct k_work *work)
{
    uint8_t buf[1];
    mfd_npm1300_reg_read(mfd, PMIC_EVENT_BASE_ADDR, PMIC_EVENT_CHARGER1_GET, buf);
    static uint8_t prev_state;

    if (buf[0] & PMIC_CHARGE1_STATUS_SUPP_MASK) {
        if (!(buf[0] & prev_state)) {
            LOG_INF("Supplement Mode Started");
        }
        charging_status = CHARGING_ACTIVE;
    } else if (buf[0] & PMIC_CHARGE1_STATUS_TRICKLE_STARTED_MASK) {
        if (!(buf[0] & prev_state)) {
            LOG_INF("Trickle Charging Started");
        }
        if (uicr_shipping_flag_get()) {
            led_api_set_state(LED_CHARGING);
        }
        charging_status = CHARGING_ACTIVE;
    } else if (buf[0] & PMIC_CHARGE1_STATUS_CONST_CURR_STARTED_MASK) {
        if (!(buf[0] & prev_state)) {
            LOG_INF("Const Curr Charging Started");
        }
        if (uicr_shipping_flag_get()) {
            led_api_set_state(LED_CHARGING);
        }
        charging_status = CHARGING_ACTIVE;
    } else if (buf[0] & PMIC_CHARGE1_STATUS_CONST_VOLT_STARTED_MASK) {
        if (!(buf[0] & prev_state)) {
            LOG_INF("Const V Charging Started");
        }
        if (uicr_shipping_flag_get()) {
            led_api_set_state(LED_CHARGING);
        }
        charging_status = CHARGING_ACTIVE;
    } else if (buf[0] & PMIC_CHARGE1_STATUS_CHARGE_COMPLETED_MASK) {
        if (!(buf[0] & prev_state)) {
            LOG_INF("Charging Stopped");
        }
        if (uicr_shipping_flag_get()) {
            led_api_set_state(LED_CHARGED);
        }
        charging_status = CHARGING_COMPLETE;
    } else if (buf[0] & PMIC_CHARGE1_STATUS_ERROR_MASK) {
        if (!(buf[0] & prev_state)) {
            LOG_ERR("Charging Error");
        }
        if (uicr_shipping_flag_get()) {
            led_api_set_state(LED_API_ERROR);
        }
        charging_status = CHARGING_ERROR;
    }
    prev_state = buf[0];

    if (buf[0] != 0x0) {
        // ack/clear all the charger1 events
        mfd_npm1300_reg_write(mfd, PMIC_EVENT_BASE_ADDR, PMIC_EVENT_CHARGER1_CLR, buf[0]);
    }

    mfd_npm1300_reg_read(mfd, PMIC_EVENT_BASE_ADDR, PMIC_EVENT_VBUS_GET, buf);

    uint8_t reg = 0;
    mfd_npm1300_reg_read(mfd, PMIC_BUCKCTRL_BASE_ADDR, PMIC_BUCKCTRL0_VOLTAGE_REG, &reg);
    if (buf[0] & PMIC_CHARGE2_STATUS_VBUS_DETECTED_MASK) {
        LOG_INF("Vbus Detected");
        vbus_present = true;
        if (reg & 0x4) {
            reg = reg & 0xFb;    // clear bit 3
            mfd_npm1300_reg_write(
                mfd,
                PMIC_BUCKCTRL_BASE_ADDR,
                PMIC_BUCKCTRL0_VOLTAGE_REG,
                reg);    // disable pulldowns - buck0
        }
#if defined(CONFIG_REBOOT_ON_USB_CONNECT_TIME_IN_SECONDS)
        int64_t now = k_uptime_get();
        if ((uicr_shipping_flag_get() && last_usb_connect_time != 0)
            && ((now - last_usb_connect_time) < (CONFIG_REBOOT_ON_USB_CONNECT_TIME_IN_SECONDS * 1000))) {
            LOG_INF("Rebooting due to USB Double insertion");
            pmic_reboot("USB Double insertion");
        }
        last_usb_connect_time = now;
#endif
#if !defined(CONFIG_AVOID_ZBUS)
        bool true_state = true;
        int  err        = zbus_chan_pub(&USB_POWER_STATE_UPDATE, &true_state, K_SECONDS(1));
        if (err) {
            LOG_ERR("zbus_chan_pub, error: %d", err);
            return;
        }
#endif
    } else if (buf[0] & PMIC_CHARGE2_STATUS_VBUS_REMOVED_MASK) {
        reg = reg | 0x4;    // set bit 3
        mfd_npm1300_reg_write(
            mfd,
            PMIC_BUCKCTRL_BASE_ADDR,
            PMIC_BUCKCTRL0_VOLTAGE_REG,
            reg);    // discharge buck0 wth pulldowns
        LOG_INF("Vbus Removed");
        vbus_present    = false;
        charging_status = CHARGING_IDLE;
        if (uicr_shipping_flag_get()) {
            led_api_set_state(LED_API_IDLE);
        }
#if !defined(CONFIG_AVOID_ZBUS)
        bool false_state = false;
        int  err         = zbus_chan_pub(&USB_POWER_STATE_UPDATE, &false_state, K_SECONDS(1));
        if (err) {
            LOG_ERR("zbus_chan_pub, error: %d", err);
            return;
        }
#endif
    }

    if (buf[0] != 0x0) {
        // ack/clear all the charger2 events
        mfd_npm1300_reg_write(mfd, PMIC_EVENT_BASE_ADDR, PMIC_EVENT_VBUS_CLR, buf[0]);
    }

    // set charging_enable?

    // call back user callback if set
    if (state_cb) {
        state_cb((charging_status == CHARGING_ACTIVE) ? PMIC_CHARGING_STARTED : PMIC_CHARGING_STOPPED);
    }
}
K_WORK_DEFINE(pmic_int_work, pmic_int_work_handler);

void pmic_int_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    k_work_submit_to_queue(&pmic_work_q, &pmic_int_work);
}

/* PMIC Initialization */
int pmic_init(bool turn_on_lte, bool turn_on_wifi)
{
    int     rc = -1;
    uint8_t buf[1];

    LOG_DBG("PMIC Init ...");

    if (!device_is_ready(regulators)) {
        LOG_ERR("Error: Regulator device is not ready\n");
        return rc;
    }
    LOG_DBG("Found NPM1300 Regulator.  Good!!!");

    if (!device_is_ready(charger)) {
        LOG_ERR("Charger device not ready.\n");
        return rc;
    }
    LOG_DBG("Found NPM1300 charger.  Good!!!");

    if (!fuel_gauge_init(charger)) {
        LOG_DBG("Fuel gauge initialized.");
    } else {
        LOG_ERR("Fuel gauge initialization failed.");
        return rc;
    }

    if (!fuel_gauge_init(charger)) {
        LOG_DBG("Fuel gauge initialized.");
    } else {
        LOG_ERR("Fuel gauge initialization failed.");
        return rc;
    }

    k_work_queue_init(&pmic_work_q);
    struct k_work_queue_config pmic_work_q_cfg = {
        .name     = "pmic_work_q",
        .no_yield = 0,
    };
    k_work_queue_start(&pmic_work_q, pmic_stack_area, K_THREAD_STACK_SIZEOF(pmic_stack_area), 3, &pmic_work_q_cfg);

    init_pmic_leds();

    regulator_set_mode(LDO1, NPM1300_LDSW_MODE_LDSW);
    regulator_set_mode(LDO2, NPM1300_LDSW_MODE_LDSW);

    // force back to 1.8V.  Set to 1.9V in dts as workaround for npm1300 errata #27
    regulator_set_voltage(BUCK1, 1800000, 1800000);

    if (gpio_pin_configure_dt(&pmic_int, GPIO_INPUT) != 0) {
        LOG_ERR("Error: failed to configure %s pin %d\n", pmic_int.port->name, pmic_int.pin);
        return rc;
    }
    // enable interrupt on button for rising edge
    if (gpio_pin_interrupt_configure_dt(&pmic_int, GPIO_INT_EDGE_TO_ACTIVE) != 0) {
        LOG_ERR("Error: failed to configure interrupt on %s pin %d\n", pmic_int.port->name, pmic_int.pin);
        return rc;
    }
    // initialize callback structure for button interrupt
    gpio_init_callback(&pmic_int_cb, pmic_int_handler, BIT(pmic_int.pin));

    // attach callback function to button interrupt
    gpio_add_callback(pmic_int.port, &pmic_int_cb);

    // set interrupts on pmic
    mfd_npm1300_reg_write(mfd, PMIC_GPIO_BASE_ADDR, 0x11, 0x1);
    mfd_npm1300_reg_write(mfd, PMIC_GPIO_BASE_ADDR, PMIC_GPIO_2_REG,
                          0x5);    // set GPIO1 as INT out

    mfd_npm1300_reg_write(
        mfd,
        PMIC_EVENT_BASE_ADDR,
        PMIC_EVENT_CHARGER1_SET,
        0x14);    // CC+CHARGEDONE events will trigger INT
    mfd_npm1300_reg_write(
        mfd,
        PMIC_EVENT_BASE_ADDR,
        PMIC_EVENT_VBUS_SET,
        0x3);    // all charge2 events will trigger INT

    mfd_npm1300_reg_write(mfd, PMIC_EVENT_BASE_ADDR, PMIC_EVENT_CHARGER1_CLR,
                          0xff);    // clear all charge events
    mfd_npm1300_reg_write(mfd, PMIC_EVENT_BASE_ADDR, PMIC_EVENT_VBUS_CLR,
                          0xff);    // clear all charge events

    // set temperature thresholds for charging
    mfd_npm1300_reg_write(mfd, PMIC_BCHARGER_BASE_ADDR, PMIC_NTCCOLD_REG, PMIC_NTCCOLD_THRESHOLD);
    mfd_npm1300_reg_write(mfd, PMIC_BCHARGER_BASE_ADDR, PMIC_NTCCOLDLSB_REG, PMIC_NTCCOLDLSB_THRESHOLD);
    mfd_npm1300_reg_write(mfd, PMIC_BCHARGER_BASE_ADDR, PMIC_NTCCOOL_REG, PMIC_NTCCOOL_THRESHOLD);
    mfd_npm1300_reg_write(mfd, PMIC_BCHARGER_BASE_ADDR, PMIC_NTCCOOLLSB_REG, PMIC_NTCCOOLLSB_THRESHOLD);
    mfd_npm1300_reg_write(mfd, PMIC_BCHARGER_BASE_ADDR, PMIC_NTCWARM_REG, PMIC_NTCWARM_THRESHOLD);
    mfd_npm1300_reg_write(mfd, PMIC_BCHARGER_BASE_ADDR, PMIC_NTCWARMLSB_REG, PMIC_NTCWARMLSB_THRESHOLD);
    mfd_npm1300_reg_write(mfd, PMIC_BCHARGER_BASE_ADDR, PMIC_NTCHOT_REG, PMIC_NTCHOT_THRESHOLD);
    mfd_npm1300_reg_write(mfd, PMIC_BCHARGER_BASE_ADDR, PMIC_NTCHOTLSB_REG, PMIC_NTCHOTLSB_THRESHOLD);

    // read LDO switch states
    mfd_npm1300_reg_read(mfd, PMIC_LDO_STATUS_BASE_ADDR, PMIC_LDO_STATUS_REG, buf);
    if (buf[0] & PMIC_LDO_STATUS_LDO1_MASK) {
        modem_power_state = PMIC_LTE_POWER_ON;
        LOG_DBG("LDO1 (nrf9160) is on");
    } else {
        modem_power_state = PMIC_LTE_POWER_OFF;
        LOG_DBG("LDO1 (nrf9160) is off");
    }

    // read charger status
    mfd_npm1300_reg_read(mfd, PMIC_CHARGE_STATUS_REG, PMIC_BCHARGER_BASE_ADDR, buf);
    if (buf[0] == 0) {
        LOG_DBG("Charging Status: IDLE");
        charging_status = CHARGING_IDLE;
    } else if (buf[0] == 1) {
        LOG_DBG("Charging Status: COMPLETE");
        charging_status = CHARGING_COMPLETE;
    } else {
        LOG_DBG("Charging Status: ACTIVE");
        charging_status = CHARGING_ACTIVE;
    }

    mfd_npm1300_reg_read(mfd, PMIC_VBUS_BASE_ADDR, PMIC_VBUS_STATUS_REG, buf);
    if (buf[0] & 0x1) {
        LOG_DBG("VBUS_STATUS: PLUGGED IN");
        vbus_present = true;
    } else {
        LOG_DBG("VBUS_STATUS: NOT PLUGGED IN");
        vbus_present = false;
    }

    LOG_DBG("Found NPM1300 LED/GPIO/MFD.  Good!!!");

    regulator_set_mode(LDO1, NPM1300_LDSW_MODE_LDSW);
    regulator_set_mode(LDO2, NPM1300_LDSW_MODE_LDSW);

    if (turn_on_lte) {
        pmic_power_on_modem();
    }
    if (turn_on_wifi) {
        set_switch_state(PMIC_SWITCH_WIFI, true);
    }

    mfd_npm1300_reg_read(mfd, PMIC_EVENT_BASE_ADDR, PMIC_EVENT_CHARGER1_GET, buf);

    if (buf[0] & PMIC_CHARGE1_STATUS_SUPP_MASK) {
        LOG_DBG("Supplement Mode Started");
        // set_led_color(SOLID_LED_GREEN);
        charging_status = CHARGING_ACTIVE;
    } else if (buf[0] & PMIC_CHARGE1_STATUS_TRICKLE_STARTED_MASK) {
        LOG_DBG("Trickle Charging Started");
        led_api_set_state(LED_CHARGING);
        charging_status = CHARGING_ACTIVE;
    } else if (buf[0] & PMIC_CHARGE1_STATUS_CONST_CURR_STARTED_MASK) {
        LOG_DBG("Const Curr Charging Started");
        led_api_set_state(LED_CHARGING);
        charging_status = CHARGING_ACTIVE;
    } else if (buf[0] & PMIC_CHARGE1_STATUS_CONST_VOLT_STARTED_MASK) {
        LOG_DBG("Const V Charging Started");
        led_api_set_state(LED_CHARGING);
        charging_status = CHARGING_ACTIVE;
    } else if (buf[0] & PMIC_CHARGE1_STATUS_CHARGE_COMPLETED_MASK) {
        LOG_DBG("Charging Stopped");
        led_api_set_state(LED_CHARGED);
        charging_status = CHARGING_COMPLETE;
    } else if (buf[0] & PMIC_CHARGE1_STATUS_ERROR_MASK) {
        LOG_DBG("Charging Error");
        led_api_set_state(LED_API_ERROR);
        charging_status = CHARGING_ERROR;
    }

    k_timer_start(&batt_info_timer, K_SECONDS(1), K_SECONDS(60));

    return 0;
}
bool get_charging_active()
{
    return (charging_status == CHARGING_ACTIVE);
}

bool get_charging_complete()
{
    return (charging_status == CHARGING_COMPLETE);
}

bool get_vbus_present()
{
    return vbus_present;
}

int set_switch_state(PMIC_SWITCHES_t pwr_switch, bool newState)
{
    int     rc = -1;
    uint8_t buf[1];
    uint8_t buf2[1];

    mfd_npm1300_reg_read(mfd, PMIC_LDO_STATUS_BASE_ADDR, PMIC_LDO_STATUS_REG, buf);

    switch (pwr_switch) {
    case PMIC_SWITCH_VSYS:
        if (newState) {
            mfd_npm1300_reg_read(mfd, PMIC_LDO_STATUS_BASE_ADDR, PMIC_LDO_CFG_REG, buf2);
            mfd_npm1300_reg_write(mfd, PMIC_LDO_BASE_ADDR, PMIC_LDO_CFG_REG,
                                  buf2[0] & 0xBF);    // clear bit 0x40
            for (int i = 0; i < 10; i++) {
                mfd_npm1300_reg_read(mfd, PMIC_LDO_STATUS_BASE_ADDR, PMIC_LDO_CFG_REG, buf2);
                // LOG_DBG("discharge reg buf2[0] = 0x%02X", buf2[0]);
                mfd_npm1300_reg_write(
                    mfd,
                    PMIC_LDO_BASE_ADDR,
                    PMIC_LDO_CFG_REG,
                    buf2[0] & 0xBF);    // disable LDO1 active discharge

                regulator_enable(LDO1);
                k_sleep(K_MSEC(2));
                mfd_npm1300_reg_read(mfd, PMIC_LDO_STATUS_BASE_ADDR, PMIC_LDO_STATUS_REG, buf);
                // LOG_DBG("ldo 1 status buf[0] = 0x%02X", buf[0]);
                if (buf[0] & PMIC_LDO_STATUS_LDO1_MASK) {
                    modem_power_state = PMIC_LTE_POWER_ON;
                    LOG_DBG("LDO1 is now on");
                    return 0;
                }
                k_sleep(K_MSEC(10));
            }
            LOG_ERR("Failed to turn on LDO1");
            return -1;
        } else {
            for (int i = 0; i < 10; i++) {
                mfd_npm1300_reg_read(mfd, PMIC_LDO_STATUS_BASE_ADDR, PMIC_LDO_CFG_REG, buf2);
                // LOG_DBG("discharge reg buf2[0] = 0x%02X", buf2[0]);
                mfd_npm1300_reg_write(
                    mfd,
                    PMIC_LDO_BASE_ADDR,
                    PMIC_LDO_CFG_REG,
                    buf2[0] | 0x40);    // force LDO1 active discharge
                regulator_disable(LDO1);
                k_sleep(K_MSEC(2));
                mfd_npm1300_reg_read(mfd, PMIC_LDO_STATUS_BASE_ADDR, PMIC_LDO_STATUS_REG, buf);
                // LOG_DBG("ldo 1 status buf[0] = 0x%02X", buf[0]);
                if (!(buf[0] & PMIC_LDO_STATUS_LDO1_MASK)) {
                    modem_power_state = PMIC_LTE_POWER_OFF;
                    LOG_DBG("LDO1 is now off");
                    return 0;
                }
                k_sleep(K_MSEC(10));
            }
            LOG_ERR("Failed to turn off LDO1");
            return -1;
        }
        break;
    case PMIC_SWITCH_WIFI:
        if (newState) {
            mfd_npm1300_reg_read(mfd, PMIC_LDO_STATUS_BASE_ADDR, PMIC_LDO_CFG_REG, buf2);
            mfd_npm1300_reg_write(mfd, PMIC_LDO_BASE_ADDR, PMIC_LDO_CFG_REG,
                                  buf2[0] & 0x7F);    // clear bit 0x80
            for (int i = 0; i < 10; i++) {
                if (buf[0] & PMIC_LDO_STATUS_LDO2_MASK) {
                    return 0;
                }
                regulator_enable(LDO2);
                k_sleep(K_USEC(250));
                mfd_npm1300_reg_read(mfd, PMIC_LDO_STATUS_BASE_ADDR, PMIC_LDO_STATUS_REG, buf);
                k_sleep(K_MSEC(10));
            }
            return -1;
        } else {
            for (int i = 0; i < 10; i++) {
                if (!(buf[0] & PMIC_LDO_STATUS_LDO2_MASK)) {
                    mfd_npm1300_reg_read(mfd, PMIC_LDO_STATUS_BASE_ADDR, PMIC_LDO_CFG_REG, buf2);
                    mfd_npm1300_reg_write(
                        mfd,
                        PMIC_LDO_BASE_ADDR,
                        PMIC_LDO_CFG_REG,
                        buf2[0] | 0x80);    // force LDO2 active discharge
                    return 0;
                }
                regulator_disable(LDO2);
                k_sleep(K_USEC(250));
                mfd_npm1300_reg_read(mfd, PMIC_LDO_STATUS_BASE_ADDR, PMIC_LDO_STATUS_REG, buf);
                k_sleep(K_MSEC(10));
            }
            return -1;
        }
        break;
    }

    return rc;
}

// code below copied from ncs 2.5 samples/driers/pmic/native/npm1300_fuel_gauge and changed to hit
// here where needed.
static int read_fg_sensors(const struct device *charger, float *voltage, float *current, float *temp)
{
    struct sensor_value value;
    int                 ret = -1;

    ret = sensor_sample_fetch(charger);
    if (ret < 0) {
        return ret;
    }

    if (charger == NULL || voltage == NULL || current == NULL || temp == NULL) {
        return ret;
    }

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
    struct sensor_value                   value;
    struct nrf_fuel_gauge_init_parameters parameters = { .model = &battery_model };
    int                                   ret;

    ret = read_fg_sensors(charger, &parameters.v0, &parameters.i0, &parameters.t0);
    if (ret < 0) {
        return ret;
    }

    /* Store charge nominal and termination current, needed for ttf calculation */
    sensor_channel_get(charger, SENSOR_CHAN_GAUGE_DESIRED_CHARGING_CURRENT, &value);
    max_charge_current  = (float)value.val1 + ((float)value.val2 / 1000000);
    term_charge_current = max_charge_current / 10.f;

    nrf_fuel_gauge_init(&parameters, NULL);

    ref_time = k_uptime_get();

    return 0;
}

int fuel_gauge_get_latest(fuel_gauge_info_t *latest)
{
    float voltage;
    float current;
    float temp;
    float soc;
    float tte;
    float ttf;
    float delta;
    int   ret;

    ret = read_fg_sensors(charger, &voltage, &current, &temp);
    if (ret < 0) {
        printk("Error: Could not read from charger device\n");
        return ret;
    }

    delta = (float)k_uptime_delta(&ref_time) / 1000.f;

    soc = nrf_fuel_gauge_process(voltage, current, temp, delta, NULL);
    soc = MAX(soc, 0);
    soc = MIN(soc, 100);
    tte = nrf_fuel_gauge_tte_get();
    ttf = nrf_fuel_gauge_ttf_get(-max_charge_current, -term_charge_current);

    // printk("V: %.3f, I: %.3f, T: %.2f, ", voltage, current, temp);
    // printk("SoC: %.2f, TTE: %.0f, TTF: %.0f\n", soc, tte, ttf);

    latest->voltage = voltage;
    latest->current = current;
    latest->temp    = temp;
    latest->soc     = soc;
    latest->tte     = tte;
    latest->ttf     = ttf;
    return 0;
}

int pmic_read_chip_id()
{
    // cant find reg for chip ID
    return 0x1234;
}

int pmic_read_NTC_resistor_val()
{
    uint8_t buf[1];
    mfd_npm1300_reg_read(mfd, PMIC_ADC_BASE_ADDR, PMIC_ADC_NTC_REG, buf);
    switch (buf[0]) {
    case 0x0:
        return 0;
    case 0x1:
        return 10000;
    case 0x2:
        return 47000;
    case 0x3:
        return 100000;
    }
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

int pmic_state_set_callback(pmic_state_change_cb_t cb)
{
    // NOTE: we could allow more than 1 user callback and iterate over them
    if (state_cb) {
        return -1;
    }

    state_cb = cb;
    return 0;
}

int pmic_temp_set_callback(pmic_temp_change_cb_t cb)
{
    temp_cb = cb;
    return 0;
}

int pmic_shutdown_set_callback(pmic_batt_shutdown_cb_t cb)
{
    batt_cb = cb;
    return 0;
}

int pmic_save_reset_reason(const char *reason)
{
    const char      *fname = "/lfs1/reset.txt";
    struct fs_file_t entry;
    int              ret;

    fs_file_t_init(&entry);
    if ((ret = fs_open(&entry, fname, FS_O_CREATE | FS_O_WRITE))) {
        LOG_ERR("Unable to create %s [%d]", fname, ret);
        return ret;
    }
    ret = fs_write(&entry, reason, strlen(reason));
    if (ret <= 0) {
        LOG_ERR("Failed to write reset reason [%d]", ret);
    } else {
        ret = 0;
    }
    fs_close(&entry);
    return ret;
}

const char *pmic_get_reset_reason(void)
{
    static char reason[128];

    const char      *fname = "/lfs1/reset.txt";
    struct fs_file_t entry;
    int              ret;

    fs_file_t_init(&entry);
    if ((ret = fs_open(&entry, fname, FS_O_READ))) {
        LOG_WRN("No saved reason");
        return NULL;
    }
    ret = fs_read(&entry, reason, sizeof(reason));
    fs_close(&entry);
    if (ret > 0) {
        reason[ret] = 0;
        return (const char *)reason;
    }
    LOG_WRN("Failed to read reset reason");
    return NULL;
}

int pmic_reboot(const char *reason)
{
    uint8_t  data = 0x01;
    uint16_t addr = 0x01;
    LOG_PANIC();
    pmic_save_reset_reason(reason);
    pmic_write_i2c(addr, data);
    return 0;
}

PMIC_LTE_POWER_STATE_t pmic_is_9160_powered()
{
    return modem_power_state;
}

void pmic_LDO_disable_work_handler(struct k_work *item)
{
    pmic_work_t *msg = CONTAINER_OF(item, pmic_work_t, work);
    k_free(msg);

    LOG_DBG("Disabling LDO1");
    pmic_power_off_modem(true);
}

int pmic_power_on_modem()
{
    if (modem_power_state == PMIC_LTE_POWER_ON) {
        LOG_DBG("9160 already powered on");
        return -EALREADY;
    }

    // if ((modem_power_state == PMIC_LTE_SOFT_OFF) || (modem_power_state ==
    // PMIC_LTE_POWER_OFF)) {
    //  toggle it off first, since its in the process of soft-shutdown
    set_switch_state(PMIC_SWITCH_VSYS, false);
    k_sleep(K_MSEC(50));
    //}

    LOG_DBG("Powering on 9160: %d", modem_power_state);
    set_switch_state(PMIC_SWITCH_VSYS, true);
    return 0;
}

int pmic_power_off_modem(bool now)
{
    if (now) {
        LOG_DBG("Powering off 9160");
        set_switch_state(PMIC_SWITCH_VSYS, false);
    }
    return 0;
}

void pmic_set_modem_low_power_state()
{
    LOG_DBG("Setting modem to low power state.");
    modem_power_state = PMIC_LTE_SOFT_OFF;
}
