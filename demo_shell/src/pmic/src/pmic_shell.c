#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/drivers/sensor.h>
#include <stdlib.h>
#include <stdio.h>
#include <zephyr/drivers/sensor/npm1300_charger.h>
#include <zephyr/dt-bindings/regulator/npm1300.h>

#include "pmic.h"

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
    shell_fprintf(sh, SHELL_NORMAL, "I: %s%d.%04d ", ((current.val1 < 0) || (current.val2 < 0)) ? "-" : "",
        abs(current.val1), abs(current.val2) / 100);
    shell_fprintf(sh, SHELL_NORMAL, "T: %d.%02d\n", temp.val1, temp.val2 / 10000);
    shell_fprintf(sh, SHELL_NORMAL, "Charger Status: %d, Error: %d\n", status.val1, error.val1);
}

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

void pmic_read_reg_cmd(const struct shell *sh, size_t argc, char **argv)
{
    uint8_t reg = strtol(argv[1], NULL, 16); 
    uint8_t pmic_event_base_addr = 0x0;
    uint16_t addr  = (pmic_event_base_addr << 8) + reg;
    uint8_t buf;
    pmic_read_i2c(addr, &buf);
    shell_print(sh, "PMIC Reg 0x%02x: 0x%02X\n", addr, buf);
}

void pmic_write_reg_cmd(const struct shell *sh, size_t argc, char **argv)
{
    uint8_t reg = strtol(argv[1], NULL, 16);
    uint8_t data = strtol(argv[2], NULL, 16);
    uint8_t pmic_event_base_addr = 0x0;
    uint16_t addr  = (pmic_event_base_addr << 8) + reg;
    pmic_write_i2c(addr, data);
}

void pmic_reboot_cmd(const struct shell *sh, size_t argc, char **argv)
{
    uint8_t data = 0x01;
    uint16_t addr  = 0x01;
    pmic_write_i2c(addr, data);
}

void read_fuel_gauge_info(const struct shell *sh, size_t argc, char **argv) {
    fuel_gauge_info_t info;
    fuel_gauge_get_latest(&info);
    shell_print(sh, "V: %.3f, I: %.3f, T: %.2f, ", info.voltage, info.current, info.temp);
    shell_print(sh, "State Of Charge: %.2f, Time Till Empty: %.2f, Time Till Full: %.2f\n", info.soc, info.tte, info.ttf);
}



SHELL_CMD_REGISTER(set_led_color, NULL, "set PMIC Led color", set_led_color_cmd);
SHELL_CMD_REGISTER(show_fuel_gauge, NULL, "Fuel Gauge Info", read_fuel_gauge_info);

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_pmic,
    SHELL_CMD(read_reg, NULL, "reads a register on the PMIC", pmic_read_reg_cmd),
    SHELL_CMD(write_reg, NULL, "writes a register on the PMIC", pmic_write_reg_cmd),
    SHELL_CMD(reboot, NULL, "reboots the PMIC and all power rails", pmic_reboot_cmd),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(pmic, &sub_pmic, "Commands to control the pmic", NULL);