/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
/*
 *
 * SPDX-License-Identifier: LicenseRef-Proprietary
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

// null-terminated max length of string returned from _get routines
#define UICR_STR_MAX_LEN (13)

// This memory range is recommended by FAE
// total 768 bytes available from 0x100-0x400
// Immutable boot loader takes 640.
// We can use 96 bytes at the very end
#define UICR_APP_BASE_ADDR (NRF_UICR_S_BASE + 0x3A0)

//#define MY_UICR_SCHEMA_VERSION 0xBEEF0003 // moved to accomodate more data
#define MY_UICR_SCHEMA_VERSION     0xBEEF0004    // For new units to indicate Ship mode flag should be respected
#define NUM_TEST_SLOTS             12
#define MAGIC_SHIPPED_SIGNATURE    0xDEADBEEF
#define MAGIC_IN_FACTORY_SIGNATURE 0xDEADFFFF
typedef struct _uicr_storage_struct_t
{
    uint32_t uicr_version_number;          // 4 bytes
    uint32_t serial_number[3];             // 12 bytes
    uint32_t ble_mac_addr[3];              // 12 bytes
    uint32_t wifi_mac_addr[3];             // 12 bytes
    uint16_t test_flag[NUM_TEST_SLOTS];    // 24 bytes
    uint32_t da16200_tuning_value;         // 4 bytes
    uint32_t shipping_flag;                // 4 bytes
    uint32_t in_factory_flag;              // 4 bytes
    uint32_t empty[5];                     // 20 bytes
} uicr_storage_struct_t;


// There are only three states that can happen.
//  1. The UICR is identical to the backup.  This is the normal state
//  2. The UICR is different from the backup, but differences are all that the UICR is FF.
//     This is the case when we erased the UICR
//  3. The UICR is different from the backup, and the differences are not FF.
//     This is an error
// If we detect case 2, we should just restore the UICR and return 0
int  uicr_backup_cmp_restore(uint32_t *backup_uicr_storage);
void uicr_export(uint32_t *buf);

// Return 0 if the UICR is valid, -1 if it is not
int uicr_verify();

void uicr_serial_number_set_override(char *serial);    // Testing only, allows for changing to temp serial numbers
int  uicr_serial_number_set(const char *serial_number_string);
int  uicr_ble_mac_address_set(const char *ble_mac_string);
int  uicr_wifi_mac_address_set(const char *wifi_mac_string);

uint32_t uicr_version_get();    // Get what is in UICR, NOT what version the code is
char    *uicr_serial_number_get();
char    *uicr_ble_mac_address_get();
char    *uicr_wifi_mac_address_get();
int      uicr_version_init(void);
int      uicr_test_flag_set(const uint16_t flag_value);
uint16_t uicr_test_flag_get();
int      uicr_wifi_tuning_value_set(const uint32_t tuning_val);
uint32_t uicr_wifi_tuning_value_get();
bool     uicr_in_factory_flag_get();
int      uicr_in_factory_flag_set();
bool     uicr_shipping_flag_get();
int      uicr_shipping_flag_set();
int      uicr_approtect_get();
int      uicr_approtect_set();
