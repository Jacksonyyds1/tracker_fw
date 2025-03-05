/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#pragma once
#include "modem_interface_types.h"
#include <cJSON.h>

void        gps_add(gps_info_t *gps_info);
gps_info_t *gps_get_last();
void        gps_get_extra();
void        gps_add_extra(gps_info_t *gps_info);
void        gps_clear();
int         gps_get_count();
int         gps_get_json_list(cJSON *jsonObj, int16_t *remaining_space);
