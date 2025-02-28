/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#pragma once

#define BT_UUID_TRACKER_SERVICE_VAL BT_UUID_128_ENCODE(0x9d1589a6, 0xcea6, 0x4df1, 0x96d9, 0x1697cd4dc1e7)

// public interfaces
int  tracker_service_init(void);
bool tracker_service_is_onboarded(void);
int  fota_notify(char *fota_state);