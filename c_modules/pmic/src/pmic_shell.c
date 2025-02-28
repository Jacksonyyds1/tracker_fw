/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/drivers/sensor.h>
#include <stdlib.h>
#include <stdio.h>
#include <zephyr/drivers/sensor/npm1300_charger.h>
#include <zephyr/dt-bindings/regulator/npm1300.h>

#include "pmic.h"
#include "pmic_leds.h"

static const struct device *charger = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_charger));

void read_charger_sensors(const struct shell *sh, size_t argc, char **argv)
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
    shell_fprintf(
        sh,
        SHELL_NORMAL,
        "I: %s%d.%04d ",
        ((current.val1 < 0) || (current.val2 < 0)) ? "-" : "",
        abs(current.val1),
        abs(current.val2) / 100);
    shell_fprintf(sh, SHELL_NORMAL, "T: %d.%02d\n", temp.val1, temp.val2 / 10000);
    shell_fprintf(sh, SHELL_NORMAL, "Charger Status: %d, Error: %d\n", status.val1, error.val1);
}
#if 0
void set_led_color_cmd(const struct shell *sh, size_t argc, char **argv)
{
    int rc;
    uint8_t color;

    if (argc < 2) {
        shell_print(sh, "Usage: %s <color>\n", argv[0]);
        shell_print(sh, "\t<color> is a number in the following table");
        shell_print(sh, "\t0: OFF");
        shell_print(sh, "\t1: RED");
        shell_print(sh, "\t2: GREEN");
        shell_print(sh, "\t3: BLUE");
        shell_print(sh, "\t4: PURPLE");
        shell_print(sh, "\t5: YELLOW");
        shell_print(sh, "\t6: CYAN");
        return;
    }

    color = atoi(argv[1]);


    rc = set_led_color(color);
    if (rc) {
        shell_print(sh, "Error setting LED color: %d\n", rc);
    }
}
#endif
void pmic_read_reg_cmd(const struct shell *sh, size_t argc, char **argv)
{
    uint16_t reg = strtol(argv[1], NULL, 16);
    uint8_t  buf;
    pmic_read_i2c(reg, &buf);
    shell_print(sh, "PMIC Reg 0x%02x: 0x%02X\n", reg, buf);
}

void pmic_write_reg_cmd(const struct shell *sh, size_t argc, char **argv)
{
    uint16_t reg  = strtol(argv[1], NULL, 16);
    uint8_t  data = strtol(argv[2], NULL, 16);
    pmic_write_i2c(reg, data);
}

void pmic_reboot_cmd(const struct shell *sh, size_t argc, char **argv)
{
    pmic_reboot("Shell request");
}

// void read_fuel_gauge_info(const struct shell *sh, size_t argc, char **argv)
// {
// 	fuel_gauge_info_t info;
// 	fuel_gauge_get_latest(&info);
// 	shell_print(sh, "V: %.3f, I: %.3f, T: %.2f, ", info.voltage, info.current, info.temp);
// 	shell_print(sh, "State Of Charge: %.2f, Time Till Empty: %.2f, Time Till Full: %.2f\n",
// 		    info.soc, info.tte, info.ttf);

// }

void read_fuel_gauge_info(const struct shell *sh, size_t argc, char **argv)
{
    fuel_gauge_info_t info;
    fuel_gauge_get_latest(&info);
    shell_print(sh, "Battery status:");
    shell_print(sh, "Voltage:         %.3f V", info.voltage);
    shell_print(sh, "Current:         %.3f mA", info.current);
    shell_print(sh, "Temp:            %.2f C", info.temp);

    shell_print(sh, "Charge Level   : %.2f%%", info.soc);
    shell_print(sh, "Time Till Empty: %.2f s", info.tte);
    shell_print(sh, "Time Till Full:  %.2f s", info.ttf);

    bool usb = get_vbus_present();
    shell_print(sh, "USB:             %s", usb ? "Connected" : "Not Connected");

    bool charging = get_charging_active();
    shell_print(sh, "Charging:        %s", charging ? "Yes" : "No");
}

void pmic_shiphold_cmd(const struct shell *sh, size_t argc, char **argv)
{
    k_sleep(K_MSEC(5000));
    set_ship_hold(true, "Shell request");
}

void pmic_ldo_cmd(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 3) {
        shell_print(sh, "Usage: %s <ldo> <state>\n", argv[0]);
        return;
    }

    PMIC_SWITCHES_t ldo   = atoi(argv[1]);
    bool            state = atoi(argv[2]);

    set_switch_state(ldo, state);
}

void pmic_reset_reason_cmd(const struct shell *sh, size_t argc, char **argv)
{
    const char *check_reset_reason(void);
    shell_print(sh, "Last pmic reboot: %s", pmic_get_reset_reason());
    shell_print(sh, "5340 reset: %s", check_reset_reason());
    if (argc > 1) {
        pmic_save_reset_reason(argv[1]);
    }
    return;
}

#define PMIC_LDO_STATUS_BASE_ADDR 0x08
#define PMIC_LDO_STATUS_REG       0x04
#define PMIC_LDO_STATUS_LDO1_MASK 0x1
#define PMIC_LDO_STATUS_LDO2_MASK 0x4
void pmic_status_cmd(const struct shell *sh, size_t argc, char **argv)
{
    fuel_gauge_info_t info;
    fuel_gauge_get_latest(&info);
    shell_print(sh, "Battery status:");
    shell_print(sh, "Voltage:         %.3f V", info.voltage);
    shell_print(sh, "Current:         %.3f mA", info.current);
    shell_print(sh, "Temp:            %.2f C", info.temp);

    shell_print(sh, "Charge Level   : %.2f%%", info.soc);
    shell_print(sh, "Time Till Empty: %.2f s", info.tte);
    shell_print(sh, "Time Till Full:  %.2f s", info.ttf);

    bool usb = get_vbus_present();
    shell_print(sh, "\nUSB:             %s", usb ? "Connected" : "Not Connected");

    bool charging = get_charging_active();
    shell_print(sh, "\nCharging:        %s", charging ? "Yes" : "No");

    PMIC_LTE_POWER_STATE_t pstate = pmic_is_9160_powered();
    switch (pstate) {
    case PMIC_LTE_POWER_ON:
        shell_print(sh, "\nLTE Power:       On");
        break;
    case PMIC_LTE_POWER_OFF:
        shell_print(sh, "\nLTE Power:       Low-V Off");
        break;
    case PMIC_LTE_SOFT_OFF:
        shell_print(sh, "\nLTE Power:       Soft Off");
        break;
    }

    uint8_t reg;
    reg = pmic_read_register_val(PMIC_LDO_STATUS_BASE_ADDR, PMIC_LDO_STATUS_REG);
    if (reg & PMIC_LDO_STATUS_LDO1_MASK) {
        shell_print(sh, "LDO1 (nrf9160) is on");
    } else {
        shell_print(sh, "LDO1 (nrf9160) is off");
    }

    reg = pmic_read_register_val(PMIC_LDO_STATUS_BASE_ADDR, PMIC_LDO_STATUS_REG);
    if (reg & PMIC_LDO_STATUS_LDO2_MASK) {
        shell_print(sh, "LDO1 (da16200) is on");
    } else {
        shell_print(sh, "LDO1 (da16200) is off");
    }
    shell_print(sh, "\n");
}

// SHELL_CMD_REGISTER(set_led_color, NULL, "set PMIC Led color", set_led_color_cmd);
SHELL_CMD_REGISTER(show_fuel_gauge, NULL, "Fuel Gauge Info", read_fuel_gauge_info);

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_pmic,
    SHELL_CMD(read_reg, NULL, "reads a register on the PMIC", pmic_read_reg_cmd),
    SHELL_CMD(write_reg, NULL, "writes a register on the PMIC", pmic_write_reg_cmd),
    SHELL_CMD(reboot, NULL, "reboots the PMIC and all power rails", pmic_reboot_cmd),
    SHELL_CMD(ldo, NULL, "control ldos", pmic_ldo_cmd),
    SHELL_CMD(set_shiphold, NULL, "sets ship_hold", pmic_shiphold_cmd),
    SHELL_CMD(reset_reason, NULL, "set the reset reason", pmic_reset_reason_cmd),
    SHELL_CMD(status, NULL, "PMIC status", pmic_status_cmd),
    SHELL_SUBCMD_SET_END);


SHELL_CMD_REGISTER(pmic, &sub_pmic, "Commands to control the pmic", NULL);