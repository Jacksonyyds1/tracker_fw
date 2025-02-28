/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include "uicr.h"
#include <stdint.h>
#include <errno.h>
#include <nrfx_nvmc.h>
#include "stdio.h"
#include <zephyr/shell/shell.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <uicr.h>
LOG_MODULE_REGISTER(uicr, LOG_LEVEL_ERR);

static const uicr_storage_struct_t *const singleton_uicr_storage = (uicr_storage_struct_t *)UICR_APP_BASE_ADDR;
static char _serial_number_string[UICR_STR_MAX_LEN]              = { 0 };    // Serial number, null terminated 12 digit
static char _ble_mac_addr_string[UICR_STR_MAX_LEN]  = { 0 };    // BLE mac addr AABBCCDDEEFF, null terminated 12 char
static char _wifi_mac_addr_string[UICR_STR_MAX_LEN] = { 0 };    // Wifi mac addr AABBCCDDEEFF, null terminated 12 char
char        serial_override[13]                     = "";       // Serial number, null terminated 12 digit

void
uicr_serial_number_set_override(char *serial)
{
    strncpy(serial_override, serial, 12);
}


char *
uicr_serial_number_get()
{
    if (serial_override[0] != '\0') {
        return serial_override;
    }
    _serial_number_string[0] = (singleton_uicr_storage->serial_number[0] >> 0) & 0xFF;
    _serial_number_string[1] = (singleton_uicr_storage->serial_number[0] >> 8) & 0xFF;
    _serial_number_string[2] = (singleton_uicr_storage->serial_number[0] >> 16) & 0xFF;
    _serial_number_string[3] = (singleton_uicr_storage->serial_number[0] >> 24) & 0xFF;

    _serial_number_string[4] = (singleton_uicr_storage->serial_number[1] >> 0) & 0xFF;
    _serial_number_string[5] = (singleton_uicr_storage->serial_number[1] >> 8) & 0xFF;
    _serial_number_string[6] = (singleton_uicr_storage->serial_number[1] >> 16) & 0xFF;
    _serial_number_string[7] = (singleton_uicr_storage->serial_number[1] >> 24) & 0xFF;

    _serial_number_string[8]  = (singleton_uicr_storage->serial_number[2] >> 0) & 0xFF;
    _serial_number_string[9]  = (singleton_uicr_storage->serial_number[2] >> 8) & 0xFF;
    _serial_number_string[10] = (singleton_uicr_storage->serial_number[2] >> 16) & 0xFF;
    _serial_number_string[11] = (singleton_uicr_storage->serial_number[2] >> 24) & 0xFF;

    _serial_number_string[12] = '\0';

    return _serial_number_string;
}

void
uicr_export(uint32_t *buf)
{
    uint32_t *ptr = (uint32_t *)UICR_APP_BASE_ADDR;
    for (int i = 0; i < sizeof(uicr_storage_struct_t) / sizeof(uint32_t); i++) {
        buf[i] = ptr[i];
    }
}


// There are only three states that can happen.
//  1. The UICR is identical to the backup.  This is the normal state
//  2. The UICR is different from the backup, but differences are all that the UICR is FF.
//     This is the case when we erased the UICR
//  3. The UICR is different from the backup, and the differences are not FF.
//     This is an error
// If we detect case 2, we should just restore the UICR and return 0
int
uicr_backup_cmp_restore(uint32_t *backup_uicr_storage)
{
    // Case 1
    const uint32_t *const uicr_data = (const uint32_t *)singleton_uicr_storage;
    int                   changed   = 0;
    for (int i = 0; i < sizeof(uicr_storage_struct_t) / sizeof(uint32_t); i++) {
        if (uicr_data[i] != backup_uicr_storage[i]) {
            // Case 1
            if (uicr_data[i] == backup_uicr_storage[i]) {
                continue;
            }
            // Case 2
            if (uicr_data[i] == 0xFFFFFFFF && backup_uicr_storage[i] != 0xFFFFFFFF) {
                LOG_ERR("UICR at idx %d differs from backup, restoring", i);
                nrfx_nvmc_word_write((uint32_t) & (uicr_data[i]), backup_uicr_storage[i]);
                changed = 1;
                continue;
            }
            // Case 3
            return -EINVAL;
        }
    }
    return changed;
}

// Return 0 if the UICR is valid, -1 if it is not
int
uicr_verify()
{
    char *blemacstr  = uicr_ble_mac_address_get();     // a 12 character string of the ble mac address
    char *wifimacstr = uicr_wifi_mac_address_get();    // a 12 character string of the wifi mac address

    // Check the mac addresses to see if they seems valid
    // The mac address needs to be even to be valid (per the DA documentation)
    // 01:23:45:67:89:01
    int wmacbyte = wifimacstr[0] - '0';
    if ((wmacbyte & 0x01) != 0) {
        LOG_ERR("First byte of wifi mac address in UICR are not even so are invalid per DA16200");
        return -1;
    }
    // Purina's mac addresses are BC:08:66:XX:XX:XX
    if ((wifimacstr[0] != 'b' && wifimacstr[0] != 'B') || (blemacstr[0] != 'b' && blemacstr[0] != 'B')
        || (wifimacstr[1] != 'c' && wifimacstr[1] != 'C') || (blemacstr[1] != 'c' && blemacstr[1] != 'C')
        || (wifimacstr[2] != '0' || blemacstr[2] != '0') || (wifimacstr[3] != '8' || blemacstr[3] != '8')
        || (wifimacstr[4] != '6' || blemacstr[4] != '6') || (wifimacstr[5] != '6' || blemacstr[5] != '6')) {
        LOG_ERR("mac address is not purina's");
        return -1;
    }

    char *sn = uicr_serial_number_get();
    if (strlen(sn) < 12 || sn[0] != 'D' || sn[1] != 'T') {
        LOG_ERR("Serial number is not a dog tracker serial number");
        return -1;
    }
    if (singleton_uicr_storage->da16200_tuning_value == -1) {
        if (uicr_shipping_flag_get()) {
            LOG_ERR("DA16200 tuning value is not set");
        }
        return -1;
    }
    return 0;
}


int
uicr_serial_number_set(const char *serial_number_string)
{
    // The serial number format is (e.g.) DT1AM640000E
    // For the dog tracker, the DT is fixed
    // 1 implies gen1
    // A is device variation ( regular, small, large, premium, test)
    // G/X/M is production location ( Guadalajara, Xiamen, Malaysia)
    // 64 is Date code, 6 = month is june, 4 is build year, 2024
    // 0000E is the serial number, in hex
    if (serial_number_string[0] != 'D' || serial_number_string[1] != 'T') {
        LOG_ERR("This is not a valid dog tracker serial number( starts with DT) \n");
        return -EROFS;
    }
    if (strlen(serial_number_string) != 12) {
        LOG_ERR("This is not a valid dog tracker serial number( needs 12 digits) \n");
        return -EROFS;
    }
    if (singleton_uicr_storage->serial_number[0] != 0xFFFFFFFF && singleton_uicr_storage->serial_number[1] != 0xFFFFFFFF
        && singleton_uicr_storage->serial_number[2] != 0xFFFFFFFF) {
        LOG_ERR("Serial number is already set \n");
        return -EROFS;
    }
    LOG_DBG("\n Going to write to %08x\n", (uint32_t) & (singleton_uicr_storage->serial_number[0]));
    LOG_DBG("\n Going to write to %08x\n", (uint32_t) & (singleton_uicr_storage->serial_number[1]));
    LOG_DBG("\n Going to write to %08x\n", (uint32_t) & (singleton_uicr_storage->serial_number[2]));
    nrfx_nvmc_word_write((uint32_t) & (singleton_uicr_storage->serial_number[0]), *(uint32_t *)&serial_number_string[0]);
    nrfx_nvmc_word_write((uint32_t) & (singleton_uicr_storage->serial_number[1]), *(uint32_t *)&serial_number_string[4]);
    nrfx_nvmc_word_write((uint32_t) & (singleton_uicr_storage->serial_number[2]), *(uint32_t *)&serial_number_string[8]);
    return 0;
}

static void
sn_read_handler(const struct shell *sh, size_t argc, char **argv)
{
    printf("device serial number is %s\n", uicr_serial_number_get());
}
static void
sn_write_test(const struct shell *sh, size_t argc, char **argv)
{
    uicr_serial_number_set("TD1AM640000E");
    uicr_serial_number_set("DT1AM64000");
    uicr_serial_number_set("DT1AM6400000M");
    uicr_serial_number_set("DT1AM640000E");
}
char *
uicr_ble_mac_address_get()
{
    _ble_mac_addr_string[0] = (singleton_uicr_storage->ble_mac_addr[0] >> 0) & 0xFF;
    _ble_mac_addr_string[1] = (singleton_uicr_storage->ble_mac_addr[0] >> 8) & 0xFF;
    _ble_mac_addr_string[2] = (singleton_uicr_storage->ble_mac_addr[0] >> 16) & 0xFF;
    _ble_mac_addr_string[3] = (singleton_uicr_storage->ble_mac_addr[0] >> 24) & 0xFF;

    _ble_mac_addr_string[4] = (singleton_uicr_storage->ble_mac_addr[1] >> 0) & 0xFF;
    ;
    _ble_mac_addr_string[5] = (singleton_uicr_storage->ble_mac_addr[1] >> 8) & 0xFF;
    _ble_mac_addr_string[6] = (singleton_uicr_storage->ble_mac_addr[1] >> 16) & 0xFF;
    _ble_mac_addr_string[7] = (singleton_uicr_storage->ble_mac_addr[1] >> 24) & 0xFF;

    _ble_mac_addr_string[8] = (singleton_uicr_storage->ble_mac_addr[2] >> 0) & 0xFF;
    ;
    _ble_mac_addr_string[9]  = (singleton_uicr_storage->ble_mac_addr[2] >> 8) & 0xFF;
    _ble_mac_addr_string[10] = (singleton_uicr_storage->ble_mac_addr[2] >> 16) & 0xFF;
    _ble_mac_addr_string[11] = (singleton_uicr_storage->ble_mac_addr[2] >> 24) & 0xFF;

    _ble_mac_addr_string[12] = '\0';

    return _ble_mac_addr_string;
}

int
uicr_ble_mac_address_set(const char *ble_mac_addr_string)
{
    if (singleton_uicr_storage->ble_mac_addr[0] != 0xFFFFFFFF && singleton_uicr_storage->ble_mac_addr[1] != 0xFFFFFFFF
        && singleton_uicr_storage->ble_mac_addr[2] != 0xFFFFFFFF) {
        LOG_ERR("BLE MAC is already set \n");
        return -EROFS;
    }
    if (strlen(ble_mac_addr_string) != 12) {
        LOG_ERR("This is not a valid BLE MAC address( %d should be 12 characters) \n", strlen(ble_mac_addr_string));
        return -EROFS;
    }
    for (int i = 0; i < 12; i++) {
        if (!(((ble_mac_addr_string[i] >= '0') && (ble_mac_addr_string[i] <= '9'))
              || ((ble_mac_addr_string[i] >= 'a') && (ble_mac_addr_string[i] <= 'f'))
              || ((ble_mac_addr_string[i] >= 'A') && (ble_mac_addr_string[i] <= 'F')))) {
            LOG_ERR("MAC address can only have [0-9/A-F/a-f] characters\n");
            return -EROFS;
        }
    }
    LOG_DBG("\n Going to write to %08x\n", (uint32_t) & (singleton_uicr_storage->ble_mac_addr[0]));
    LOG_DBG("\n Going to write to %08x\n", (uint32_t) & (singleton_uicr_storage->ble_mac_addr[1]));
    LOG_DBG("\n Going to write to %08x\n", (uint32_t) & (singleton_uicr_storage->ble_mac_addr[2]));
    nrfx_nvmc_word_write((uint32_t) & (singleton_uicr_storage->ble_mac_addr[0]), *(uint32_t *)&ble_mac_addr_string[0]);
    nrfx_nvmc_word_write((uint32_t) & (singleton_uicr_storage->ble_mac_addr[1]), *(uint32_t *)&ble_mac_addr_string[4]);
    nrfx_nvmc_word_write((uint32_t) & (singleton_uicr_storage->ble_mac_addr[2]), *(uint32_t *)&ble_mac_addr_string[8]);
    return 0;
}
static void
ble_mac_read_handler(const struct shell *sh, size_t argc, char **argv)
{
    printf("BLE MAC IS %s\n", uicr_ble_mac_address_get());
}
static void
ble_mac_write_test(const struct shell *sh, size_t argc, char **argv)
{
    char ble_mac_addr[] = "112233445566";
    uicr_ble_mac_address_set(ble_mac_addr);
}
char *
uicr_wifi_mac_address_get()
{
    _wifi_mac_addr_string[0] = (singleton_uicr_storage->wifi_mac_addr[0] >> 0) & 0xFF;
    _wifi_mac_addr_string[1] = (singleton_uicr_storage->wifi_mac_addr[0] >> 8) & 0xFF;
    _wifi_mac_addr_string[2] = (singleton_uicr_storage->wifi_mac_addr[0] >> 16) & 0xFF;
    _wifi_mac_addr_string[3] = (singleton_uicr_storage->wifi_mac_addr[0] >> 24) & 0xFF;

    _wifi_mac_addr_string[4] = (singleton_uicr_storage->wifi_mac_addr[1] >> 0) & 0xFF;
    _wifi_mac_addr_string[5] = (singleton_uicr_storage->wifi_mac_addr[1] >> 8) & 0xFF;
    _wifi_mac_addr_string[6] = (singleton_uicr_storage->wifi_mac_addr[1] >> 16) & 0xFF;
    _wifi_mac_addr_string[7] = (singleton_uicr_storage->wifi_mac_addr[1] >> 24) & 0xFF;

    _wifi_mac_addr_string[8]  = (singleton_uicr_storage->wifi_mac_addr[2] >> 0) & 0xFF;
    _wifi_mac_addr_string[9]  = (singleton_uicr_storage->wifi_mac_addr[2] >> 8) & 0xFF;
    _wifi_mac_addr_string[10] = (singleton_uicr_storage->wifi_mac_addr[2] >> 16) & 0xFF;
    _wifi_mac_addr_string[11] = (singleton_uicr_storage->wifi_mac_addr[2] >> 24) & 0xFF;

    _wifi_mac_addr_string[12] = '\0';

    return _wifi_mac_addr_string;
}

int
uicr_wifi_mac_address_set(const char *wifi_mac_addr_string)
{
    if (singleton_uicr_storage->wifi_mac_addr[0] != 0xFFFFFFFF && singleton_uicr_storage->wifi_mac_addr[1] != 0xFFFFFFFF
        && singleton_uicr_storage->wifi_mac_addr[2] != 0xFFFFFFFF) {
        LOG_ERR("WIFI MAC is already set \n");
        return -EROFS;
    }
    if (strlen(wifi_mac_addr_string) != 12) {
        LOG_ERR("This is not a valid WIFI MAC address( should be 12 characters) \n");
        return -EROFS;
    }
    for (int i = 0; i < 12; i++) {
        if (!(((wifi_mac_addr_string[i] >= '0') && (wifi_mac_addr_string[i] <= '9'))
              || ((wifi_mac_addr_string[i] >= 'a') && (wifi_mac_addr_string[i] <= 'f'))
              || ((wifi_mac_addr_string[i] >= 'A') && (wifi_mac_addr_string[i] <= 'F')))) {
            LOG_ERR("MAC address can only have [0-9/A-F/a-f] characters\n");
            return -EROFS;
        }
    }

    LOG_DBG("\n Going to write to %08x\n", (uint32_t) & (singleton_uicr_storage->wifi_mac_addr[0]));
    LOG_DBG("\n Going to write to %08x\n", (uint32_t) & (singleton_uicr_storage->wifi_mac_addr[1]));
    LOG_DBG("\n Going to write to %08x\n", (uint32_t) & (singleton_uicr_storage->wifi_mac_addr[2]));
    nrfx_nvmc_word_write((uint32_t) & (singleton_uicr_storage->wifi_mac_addr[0]), *(uint32_t *)&wifi_mac_addr_string[0]);
    nrfx_nvmc_word_write((uint32_t) & (singleton_uicr_storage->wifi_mac_addr[1]), *(uint32_t *)&wifi_mac_addr_string[4]);
    nrfx_nvmc_word_write((uint32_t) & (singleton_uicr_storage->wifi_mac_addr[2]), *(uint32_t *)&wifi_mac_addr_string[8]);
    return 0;
}

int
uicr_test_flag_set(const uint16_t flag_value)
{
    int i = 0;
    for (i = 0; i < NUM_TEST_SLOTS; i++) {
        if (0xFFFF == nrfx_nvmc_otp_halfword_read((uint32_t) & (singleton_uicr_storage->test_flag[i]))) {
            // printf("slot %d is empty\n",i);
            break;
        }
    }
    if (i == NUM_TEST_SLOTS) {
        LOG_ERR("no empty slots");
        return -EROFS;
    }
    uint32_t addr = (uint32_t) & (singleton_uicr_storage->test_flag[i]);
    nrfx_nvmc_halfword_write(addr, flag_value);
    return 0;
}
uint16_t
uicr_test_flag_get()
{
    int i = 0;
    for (i = 0; i < NUM_TEST_SLOTS; i++) {
        if (0xFFFF == nrfx_nvmc_otp_halfword_read((uint32_t) & (singleton_uicr_storage->test_flag[i]))) {
            // printf("slot %d is empty\n",i);
            break;
        }
    }
    if (i == 0) {
        LOG_ERR("No used slots");
        return -EROFS;
    }
    return nrfx_nvmc_otp_halfword_read((uint32_t) & (singleton_uicr_storage->test_flag[i - 1]));
}
static void
wifi_mac_read_handler(const struct shell *sh, size_t argc, char **argv)
{
    printf("WIFI MAC IS %s\n", uicr_wifi_mac_address_get());
}
static void
wifi_mac_write_test(const struct shell *sh, size_t argc, char **argv)
{
    char wifi_mac_addr[] = "ABCDEF123456";
    uicr_wifi_mac_address_set(wifi_mac_addr);
}

// Get what is in UICR, NOT what version the code is
uint32_t
uicr_version_get()
{
    return (singleton_uicr_storage->uicr_version_number);
}

uint32_t
uicr_wifi_tuning_value_get()
{
    // printf("TUNING VALUE IS %02d\n", singleton_uicr_storage->da16200_tuning_value);
    return (singleton_uicr_storage->da16200_tuning_value);
}

int
uicr_wifi_tuning_value_set(const uint32_t tuning_val)
{
    if (tuning_val > 128) {
        LOG_ERR("tuning_val %x \n", tuning_val);
        return -EROFS;
    }
    if (singleton_uicr_storage->da16200_tuning_value != 0xFFFFFFFF) {
        LOG_ERR("tuning_val already set to %x \n", singleton_uicr_storage->da16200_tuning_value);
        return -EROFS;
    }

    LOG_DBG("\n Going to write to %08x\n", (uint32_t) & (singleton_uicr_storage->da16200_tuning_value));
    nrfx_nvmc_word_write((uint32_t) & (singleton_uicr_storage->da16200_tuning_value), tuning_val);
    return 0;
}

bool
uicr_shipping_flag_get()
{
    uint32_t ver = singleton_uicr_storage->uicr_version_number & 0x0000FFFF;
    if (ver < 4) {
        return true;
    }
    // If the shipped flag is correct, then we have positive confirmation
    // that this unit shipped, this is for backwards compatibility for units
    // that don't have a in factory flag
    uint32_t shipped = singleton_uicr_storage->shipping_flag;
    return (shipped == MAGIC_SHIPPED_SIGNATURE);
}

bool
uicr_in_factory_flag_get()
{
    // If we shipped, we aren't in the factory
    uint32_t shipped = singleton_uicr_storage->shipping_flag;
    if (shipped == MAGIC_SHIPPED_SIGNATURE) {
        return false;
    }

    uint32_t in_factory = singleton_uicr_storage->in_factory_flag;
    return (in_factory == MAGIC_IN_FACTORY_SIGNATURE);
}


int
uicr_in_factory_flag_set()
{
    uint32_t fflag = singleton_uicr_storage->in_factory_flag;
    if (fflag == MAGIC_IN_FACTORY_SIGNATURE) {
        return 0;
    }

    if (fflag != 0xFFFFFFFF) {
        LOG_ERR("In factory flag is incorrectly set to %x \n", fflag);
        return -EROFS;
    }

    LOG_DBG("UICR write to %08x\n", (uint32_t) & (singleton_uicr_storage->in_factory_flag));
    nrfx_nvmc_word_write((uint32_t) & (singleton_uicr_storage->in_factory_flag), MAGIC_IN_FACTORY_SIGNATURE);
    return 0;
}

int
uicr_shipping_flag_set()
{
    // If someone sets the shipping flag, also set the in factory flag
    uicr_in_factory_flag_set();

    uint32_t sflag = singleton_uicr_storage->shipping_flag;
    if (sflag == MAGIC_SHIPPED_SIGNATURE) {
        return 0;
    }

    if (sflag != 0xFFFFFFFF) {
        LOG_ERR("The shipping flag is incorrectly set to %x \n", sflag);
        return -EROFS;
    }

    LOG_DBG("\n Going to write to %08x\n", (uint32_t) & (singleton_uicr_storage->shipping_flag));
    nrfx_nvmc_word_write((uint32_t) & (singleton_uicr_storage->shipping_flag), MAGIC_SHIPPED_SIGNATURE);
    return 0;
}

int
uicr_approtect_get(void)
{
    return nrfx_nvmc_uicr_word_read((uint32_t *)NRF_UICR);
}

int
uicr_approtect_set(void)
{
    nrfx_nvmc_word_write((uint32_t)NRF_UICR_S, 0);
    return 0;
}

static void
uicr_tune_read_handler(const struct shell *sh, size_t argc, char **argv)
{
    printf("TUNING VALUE IS %02d\n", uicr_wifi_tuning_value_get());
}
static void
uicr_tune_write_handler(const struct shell *sh, size_t argc, char **argv)
{
    uicr_wifi_tuning_value_set(55);
}

// NOTE: this needs to be done at system init time
int
uicr_version_init(void)
{
    if (singleton_uicr_storage->uicr_version_number == 0xFFFFFFFF) {
        printk("Going to write to %08x\n", (uint32_t) & (singleton_uicr_storage->uicr_version_number));
        nrfx_nvmc_word_write((uint32_t) & (singleton_uicr_storage->uicr_version_number), MY_UICR_SCHEMA_VERSION);
    } else {
        printk("UICR data already exists, version is %08x\n", singleton_uicr_storage->uicr_version_number);
        if (singleton_uicr_storage->uicr_version_number == MY_UICR_SCHEMA_VERSION) {
            printk("\n UICR data already exists, version match(is %08x)\n", singleton_uicr_storage->uicr_version_number);
        } else {
            printk(
                "\n UICR data already exists, version Mistmatch (%08x!= %08x)\n",
                singleton_uicr_storage->uicr_version_number,
                MY_UICR_SCHEMA_VERSION);
        }
    }
    return 0;
}
static void
uicr_init_handler(const struct shell *sh, size_t argc, char **argv)
{
    uicr_version_init();
}
static void
uicr_raw_handler(const struct shell *sh, size_t argc, char **argv)
{
    for (int i = 0; i < sizeof(uicr_storage_struct_t) / sizeof(uint32_t); i++) {
        printf("[%08x] : %08x\n", (uint32_t)(UICR_APP_BASE_ADDR + 4 * i), *(uint32_t *)(UICR_APP_BASE_ADDR + 4 * i));
    }
}
static void
test_flag_test_handler(const struct shell *sh, size_t argc, char **argv)
{
    for (int i = 0; i < NUM_TEST_SLOTS; i++) {
        uicr_test_flag_set(i * 11);
        LOG_ERR("TF is %08x", uicr_test_flag_get());
    }
}
static void
uicr_pretty_handler(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "UICR version number is %08x", uicr_version_get());
    shell_print(sh, "Serial number is %s", uicr_serial_number_get());
    shell_print(sh, "BLE MAC is %s", uicr_ble_mac_address_get());
    shell_print(sh, "Wifi MAC is %s", uicr_wifi_mac_address_get());
    shell_print(sh, "Test Flag is %04x", uicr_test_flag_get());
    shell_print(sh, "Wifi XTAL value is %02d", uicr_wifi_tuning_value_get());
    shell_print(sh, "In Factory flag is %d", uicr_in_factory_flag_get());
    shell_print(sh, "Shipping mode flag is %d", uicr_shipping_flag_get());
    shell_print(sh, "Protect flag is %08x", uicr_approtect_get());
}


SHELL_CMD_REGISTER(sn_read, NULL, "UICR read serial number", sn_read_handler);
SHELL_CMD_REGISTER(sn_write, NULL, "UICR write serial number", sn_write_test);
SHELL_CMD_REGISTER(ble_mac_read, NULL, "UICR read BLE MAC ADDRESS", ble_mac_read_handler);
SHELL_CMD_REGISTER(ble_mac_write, NULL, "UICR write BLE MAC ADDRESS", ble_mac_write_test);
SHELL_CMD_REGISTER(wifi_mac_read, NULL, "UICR read wifi MAC ADDRESS", wifi_mac_read_handler);
SHELL_CMD_REGISTER(wifi_mac_write, NULL, "UICR write wifi MAC ADDRESS", wifi_mac_write_test);
SHELL_CMD_REGISTER(uicr_init, NULL, "UICR init uicr_version", uicr_init_handler);
SHELL_CMD_REGISTER(uicr_raw, NULL, "UICR init uicr_version", uicr_raw_handler);
SHELL_CMD_REGISTER(uicr_pretty, NULL, "pretty print UICR", uicr_pretty_handler);
SHELL_CMD_REGISTER(test_flag_test, NULL, "UICR init uicr_version", test_flag_test_handler);
SHELL_CMD_REGISTER(uicr_tune_read, NULL, "UICR read BLE MAC ADDRESS", uicr_tune_read_handler);
SHELL_CMD_REGISTER(uicr_tune_write, NULL, "UICR write BLE MAC ADDRESS", uicr_tune_write_handler);
#include <zephyr/init.h>
SYS_INIT(uicr_version_init, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
