
#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>
#include <zephyr/kernel.h>
#include <cJSON.h>

#define MAX_WIFI_OBJS 32


typedef struct {
    char ssid[32];
    float rssi;
    int8_t channel;
} wifi_obj_t;

typedef struct  {
    wifi_obj_t wifi[MAX_WIFI_OBJS];
    size_t count;
} wifi_arr_t;

// RETURN CHAR* MUST BE FREE'd BY CALLER!
char* json_batterylevel(float battLevel);
char* json_wifi_data(wifi_arr_t* data, float battLevel);

