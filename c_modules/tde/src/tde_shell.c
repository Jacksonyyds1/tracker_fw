/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/drivers/sensor.h>
#include <stdlib.h>
#include <stdio.h>
#include <zephyr/drivers/sensor/npm1300_charger.h>
#include <zephyr/dt-bindings/regulator/npm1300.h>
#include <zephyr/drivers/led.h>
#include <nrfx_nvmc.h>

#include "pmic.h"
#include "wifi.h"
#include "modem.h"
#include "app_version.h"
#include "uicr.h"
#include "imu.h"

char *tde0002();    // get wifi fw version
char *tde0022();    // get the wifi mac address
char *tde0026(char *ssid, char *pass);
char *tde0027();    // get the wifi signal strength
char *tde0028();    // get the wifi connected status
char *tde0022();    // get the wifi mac address
char *tde0026(char *ssid, char *pass);
char *tde0027();              // get the wifi signal strength
char *tde0028();              // get the wifi connected status
char *tde0060();              // get the XTAL value
char *tde0061(int newval);    // set the XTAL value temp
char *tde0062(int newval);    // set the XTAL value perm
char *tde0063(int start);     // start or stop rf test mode
// NOTE:  there is no definition for when tde commands would be an error.  what if it is poorly
// formatted?  no idea.

#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

void tde_cmd_handler(const struct shell *sh, size_t argc, char **argv)
{
    int                        cmd_num  = 0;
    char                      *arg2_str = 0;
    int                        arg2_int = -1;
    fuel_gauge_info_t          info;
    const struct device       *rgbleds               = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_leds));
    const struct device *const external_flash_device = DEVICE_DT_GET(DT_NODELABEL(mx25r32));
    const struct device *const pmic_device           = DEVICE_DT_GET(DT_NODELABEL(npm1300_ek_pmic));
    const struct device *const imu_device            = DEVICE_DT_GET(DT_ALIAS(imu));
    //

    // search the arg for a '.', if found take the part preceding it and convert to int.
    //  i.e. tde 0003.12345678 will be converted to 3 and 12345678
    // with no '.' just take the whole arg and convert to int.
    // i.e. tde 0004 will be converted to 4
    char *dot = strchr(argv[1], '.');
    if (dot != NULL) {
        cmd_num  = atoi(strtok(argv[1], "."));
        arg2_str = dot + 1;
        arg2_int = atoi(arg2_str);    // arg2_str may not be a number, specific commands need
                                      // to know what to expect.  if error, arg2_int will be 0
                                      // (yes, what happens if arg2_str is "0", well... 0)
    } else {
        cmd_num = atoi(argv[1]);
    }

    // shell_print(sh, "DEBUG: tde %d.%d \r\n", cmd_num, arg2_int);

    switch (cmd_num) {
    case 0:
        // tde 0000
        if (arg2_str && arg2_int) {
            // tde 0000.1
            // TODO:  set test mode
            shell_print(sh, "tde 0000.Dog Collar \r\n");
        } else if (arg2_str && (arg2_int == 0)) {
            // tde 0000.0
            // TODO:  leave test mode
            shell_print(sh, "tde 0000.0 \r\n");
        }
        break;
    case 1:
        // tde 0001
        // return the current FW version
        shell_print(sh, "tde 0001.%d%d%02d \r\n", APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_VERSION_PATCH);
        break;
    case 2:
        // tde 0002
        // return the current FW version
        // TODO: get real Wifi FW number
        shell_print(sh, "%s", tde0002());
        break;
    case 3:
        // tde 0003.xxxxxxxx
        if (arg2_str) {
            int result = uicr_serial_number_set(arg2_str);
            shell_print(sh, "tde 0003.%d \r\n", result == 0 ? 1 : 0);
        }
        break;
    case 4:
        // tde 0004
        // todo: get serial num from flash
        shell_print(sh, "tde 0004.%s \r\n", uicr_serial_number_get());
        break;
    case 5:
        // tde 0005 - set 3V0_en
        if (arg2_str && arg2_int) {
            wifi_set_3v0_enable(1);
            shell_print(sh, "tde 0005.1 \r\n");
            break;
        } else if (arg2_str && (arg2_int == 0)) {
            wifi_set_3v0_enable(0);
            shell_print(sh, "tde 0005.1 \r\n");
            break;
        }
        shell_print(sh, "tde 0005.0 \r\n");
        break;
    case 6:
        // tde 0006 - MCU flash proof of life
        if (device_is_ready(external_flash_device)) {
            // printk("External flash identified as mx25r32!");
            shell_print(sh, "tde 0006.1 \r\n");
            break;
        } else {
            // printk("External flash mx25r32 NOT seen!");
            shell_print(sh, "tde 0006.0 \r\n");
            break;
        }
        break;
    case 7:
        // tde 0007
        break;
    case 8:
        // tde 0008 - PMIC proof of life
        if (device_is_ready(pmic_device)) {
            // printk("PMIC device identified as NPM1300!");
            shell_print(sh, "tde 0008.NPM1300 \r\n");
            break;
        } else {
            // printk("PMIC device NOT seen!");
            shell_print(sh, "tde 0008.0 \r\n");
            break;
        }
        break;
    case 9:
        // tde 0009 - return charge state 1 or 0
        bool state = get_charging_active();
        shell_print(sh, "tde 0009.%d \r\n", state);
        break;
    case 10:
        // tde 0010 - return batt V
        fuel_gauge_get_latest(&info);
        shell_print(sh, "tde 0010.%d \r\n", (int)(info.voltage * 1000));
        break;
    case 11:
        // tde 0011 - return batt %
        fuel_gauge_get_latest(&info);
        shell_print(sh, "tde 0011.%d \r\n", (int)(info.soc));
        break;
    case 12:
        // tde 0012 - return charge current
        fuel_gauge_get_latest(&info);
        int fraction = ((info.current) - (int)info.current) * 1000;
        shell_print(sh, "tde 0012.%03d \r\n", fraction);
        break;
    case 13:
        // tde 0013 - return ntc resistor value
        int res = pmic_read_NTC_resistor_val();
        shell_print(sh, "tde 0013.%d \r\n", res);
        break;
    case 14:
        // tde 0014 - set LED
        if (arg2_str) {
            int   x, y = -1;
            char *dot2 = strchr(arg2_str, '.');
            x          = atoi(strtok(arg2_str, "."));
            if (dot2 != NULL) {
                y = atoi(dot2 + 1);
            }
            if (x >= 0 && y >= 0) {
                switch (x) {
                case 1:    // red
                    if (y == 0) {
                        led_off(rgbleds, 0);
                    } else {
                        led_on(rgbleds, 0);
                    }
                    break;
                case 2:    // green
                    if (y == 0) {
                        led_off(rgbleds, 1);
                    } else {
                        led_on(rgbleds, 1);
                    }
                    break;
                case 3:    // blue
                    if (y == 0) {
                        led_off(rgbleds, 2);
                    } else {
                        led_on(rgbleds, 2);
                    }
                    break;
                }
                shell_print(sh, "tde 0014.1 \r\n");
                break;
            }
        }
        shell_print(sh, "tde 0014.0 \r\n");
        break;
    case 15:
        // tde 0015 - LTE version
        char cgmr[50];
        memset(cgmr, 0, sizeof(cgmr));
        modem_get_VERSION(cgmr, sizeof(cgmr));
        shell_print(sh, "tde 0015.%s \r\n", cgmr);
        break;
    case 16:
        // tde 0016 - LTE IMEI
        char imei[50];
        memset(imei, 0, sizeof(imei));
        modem_get_IMEI(imei, sizeof(imei));
        shell_print(sh, "tde 0016.%s \r\n", imei);
        break;
    case 17:
        // tde 0017 - LTE ICCID
        char iccid[50];
        memset(iccid, 0, sizeof(iccid));
        modem_get_ICCID(iccid, sizeof(iccid));
        //%XICCID: 8944501611212321532F
        shell_print(sh, "tde 0017.%s \r\n", iccid);
        break;

    case 22:
        if (arg2_str) {
            int N = -1;
            N     = atoi(strtok(arg2_str, "."));
            if (N == 1) {
                // tde 0022.1 - get the BLE mac address
                shell_print(sh, "tde 0022.%s \r\n", uicr_ble_mac_address_get());
            } else if (N == 2) {
                // tde 0022 - get the wifi mac address
                // shell_print(sh, "%s", tde0022());
                shell_print(sh, "tde 0022.%s \r\n", uicr_wifi_mac_address_get());
            }
        }
        break;

    case 26:
        // tde 0022.ssid.pw - get the wifi mac address
        if (arg2_str) {
            char *ssid = arg2_str;
            char *dot2 = strchr(arg2_str, '.');
            if (dot2 != NULL) {
                *dot2 = 0;
                dot2++;
            }
            shell_print(sh, "%s", tde0026(ssid, dot2));
        }
        break;

    case 27:
        // tde 0027 -  get the wifi signal strength
        shell_print(sh, "%s", tde0027());
        break;

    case 28:
        // tde 0028 - get the wifi connected status
        shell_print(sh, "%s", tde0028());
        break;
    case 31:
        // tde 0032 - IMU proof of life
        if (device_is_ready(imu_device)) {
            // printk("IMU device identified as NPM1300!");
            shell_print(sh, "tde 0031.LSM6DSV16X \r\n");
            break;
        } else {
            // printk("IMU device NOT seen!");
            shell_print(sh, "tde 0031.0 \r\n");
            break;
        }
        break;
    case 32:
        // tde 0032 - IMU XYZ
        shell_print(sh, "%s", tde0032());
        break;
    case 33:
        // tde 0033 - GYRO XYZ
        shell_print(sh, "%s", tde0033());
        break;
    case 34:
        // tde 0034 write test flag
        int N   = 0;
        N       = atoi(strtok(arg2_str, "."));
        int ret = uicr_test_flag_set(N);
        shell_print(sh, "tde 0034.%d \r\n", ret == 0 ? 1 : 0);
        break;
    case 35:
        // tde 0035 read test flag
        shell_print(sh, "tde 0035.%02d \r\n", uicr_test_flag_get());
        break;
    case 48:
        // tde 0048 Enter idle current mode, currently base value
        shell_print(sh, "tde 0048.1 \r\n");
        break;
    case 50:
        // tde 0050 - enable/disable vsys
        if (arg2_str && arg2_int) {
            // tde 0050.1
            // enable vsys in pmic
            shell_print(sh, "tde 0050.1 \r\n");
            break;
        } else if (arg2_str && (arg2_int == 0)) {
            // tde 0050.0
            // disable vsys in pmic
            shell_print(sh, "tde 0050.1 \r\n");
            break;
        }
        shell_print(sh, "tde 0050.0 \r\n");
        break;
    case 51:
        // tde 0051 - enable/disable 1v8
        // deprecated: always return 0
        shell_print(sh, "tde 0051.0 \r\n");
        break;
    case 52:
        // tde 0052 - enable/disable vsys from vbat
        if (arg2_str && arg2_int) {
            // tde 0052.1
            // todo: vsys from vbat on?
            shell_print(sh, "tde 0052.1 \r\n");
            break;
        } else if (arg2_str && (arg2_int == 0)) {
            // tde 0052.0
            // todo: vsys from vbat off?
            shell_print(sh, "tde 0052.1 \r\n");
            break;
        }
        shell_print(sh, "tde 0052.0 \r\n");
        break;
    case 53:
        // tde 0053 - enable/disable wifi_1v8
        if (arg2_str && arg2_int) {
            // tde 0053.1
            wifi_1v8_on();
            shell_print(sh, "tde 0053.1 \r\n");
            break;
        } else if (arg2_str && (arg2_int == 0)) {
            // tde 0053.0
            wifi_1v8_off();
            shell_print(sh, "tde 0053.1 \r\n");
            break;
        }
        shell_print(sh, "tde 0053.0 \r\n");
        break;
    case 54:
        // tde 0054 - enable/disable vsys
        if (arg2_str && arg2_int) {
            // tde 0054.1
            modem_power_on();
            shell_print(sh, "tde 0054.1 \r\n");
            break;
        } else if (arg2_str && (arg2_int == 0)) {
            // tde 0054.0
            modem_power_off();
            shell_print(sh, "tde 0054.1 \r\n");
            break;
        }
        shell_print(sh, "tde 0054.0 \r\n");
        break;
    case 55:
        // tde 0055 - Self check results, always report 1 currently
        shell_print(sh, "tde 0055.1 \r\n");
        break;
    case 57:
        // tde 0057 - ship mode
        shell_print(sh, "tde 0057.1 \r\n");
        set_ship_hold(true, "Factory reset");
        break;
    case 58:
        // tde 0058 - write BLE and MAC address
        int  Y         = -1;
        char value[20] = { 0 };
        if (arg2_str) {
            char *dot2 = strchr(arg2_str, '.');
            Y          = atoi(strtok(arg2_str, "."));
            int ret    = -1;
            if (dot2 != NULL) {
                strncpy(value, dot2 + 1, 12);
                switch (Y) {
                case 1:
                    ret = uicr_ble_mac_address_set(value);
                    if (ret == 0) {
                        shell_print(sh, "tde 0058.%d \r\n", Y);
                    }
                    break;
                case 2:
                    ret = uicr_wifi_mac_address_set(value);
                    if (ret == 0) {
                        shell_print(sh, "tde 0058.%d \r\n", Y);
                    }
                    break;
                default:
                    printf("\nThis is an error state");
                    break;
                }
            } else {
                printf("Error, value needs to be provided");
                break;
            }
        }
        break;
    case 59:
        // tde 0059 - get the XTAL value FROM UICR
        int tuning_val = uicr_wifi_tuning_value_get();
        shell_print(sh, "tde 0059.%02d", tuning_val);
        break;
    case 60:
        // tde 0060 - get the XTAL value FROM DA16200
        shell_print(sh, "%s", tde0060());
        break;
    case 61:
        // tde 0061.val - set the XTAL value temp ( FROM DA16200)
        if (arg2_str && arg2_int) {
            shell_print(sh, "%s", tde0061(arg2_int));
        }
        break;
    case 62:
        // tde 0062.val - set the XTAL value permanently ( TO UICR)
        if (arg2_str && arg2_int) {
            int ret = uicr_wifi_tuning_value_set(arg2_int);
            shell_print(sh, "tde 0062.%d", ret == 0 ? 1 : 0);
        }
        break;
    case 63:
        // tde 0063.val - start or stop the RF test mode
        if (arg2_str && (arg2_int == 1 || arg2_int == 0)) {
            shell_print(sh, "%s", tde0063(arg2_int));
        }
        break;
    case 64:
        void tde0064();
        // tde 0064 - start spin monitoring mode for IMU test
        tde0064();
        shell_print(sh, "tde 0064.1 \r\n");
        break;
    case 65:
        char *tde0065();
        // tde 0065 - get spin monitoring result for IMU test
        shell_print(sh, "%s", tde0065());
        break;
    case 66:
        // tde 0066 - Setup the RFFE switch settings
        shell_print(sh, "tde 0066.%d \r\n", modem_set_rffe_settings() ? 0 : 1);
        break;
    case 67:
        // tde 0067 - Set a UICR register to set shipped flag
        if (arg2_str && (arg2_int == 1)) {
            uicr_shipping_flag_set();
        }
        shell_print(sh, "tde 0067.%d \r\n", uicr_shipping_flag_get());
        break;
    case 68:
        version_response_t version;
        if (modem_get_version(&version) == 0) {
            shell_print(
                sh, "tde 0068.nrf9160_ver_%d.%d.%d (%s) \r\n", version.major, version.minor, version.patch, version.githash);
            // shell_print(sh, "Modem Built on %s by %s\n", version.build_date, version.build_machine);
            // shell_print(sh, "Modem FW ver: %s\n", version.modem_fw);
        } else {
            shell_print(sh, "tde 0068. \r\n");
        }
        break;
    case 69:
        // tde 0069 - Set a UICR register to set APPROTECT
        if (arg2_str && (arg2_int == 1)) {
            if (uicr_approtect_set() == 0) {
                uicr_shipping_flag_set();
            }
            shell_print(sh, "tde 0069.%d \r\n", uicr_shipping_flag_get());
        }
        break;
    case 70:
        // tde 0070 - Set a UICR register to set in factory flag
        if (arg2_str && (arg2_int == 1)) {
            uicr_in_factory_flag_set();
        }
        shell_print(sh, "tde 0070.%d \r\n", uicr_in_factory_flag_get());
        break;
    default:
        break;
    }
}

SHELL_CMD_REGISTER(tde, NULL, "TDE commands", tde_cmd_handler);
// We cant do the more preferred method of tde with subcommands due to the formatting
// of certain tde commands like #3 that is formatted as:  tde 0003.xxxxxxxx where xxxxxx == new
// serial number.
