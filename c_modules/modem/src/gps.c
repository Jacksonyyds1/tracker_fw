/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#include "gps.h"
#include <zephyr/logging/log.h>
#include "d1_zbus.h"
#include "modem_interface_types.h"

LOG_MODULE_REGISTER(gps, 4);
K_HEAP_DEFINE(gps_heap, ((sizeof(gps_info_t) + 16) * CONFIG_MAX_GPS_DATA_POINTS));
// allocate more than needed for heap data collection and/or block alignment

K_FIFO_DEFINE(gps_data_fifo);
uint16_t gps_data_fifo_count = 0;

double round_float(float input, int decimal)
{
    int _pow = 1;
    for (int i = 0; i < decimal; i++) {
        _pow = _pow * 10;
    }

    double value;
    if (input >= 0) {
        value = (int)(input * _pow + 0.5f);
    } else {
        value = (int)(input * _pow - 0.5f);
    }

    return value / _pow;
}

// add/append to gps list
void gps_add(gps_info_t *gps_info)
{
    // CONFIG_MAX_GPS_DATA_POINTS
    while (gps_data_fifo_count >= CONFIG_MAX_GPS_DATA_POINTS) {
        // remove oldest gps info
        gps_info_t *gps_info = k_fifo_get(&gps_data_fifo, K_NO_WAIT);
        if (gps_info) {
            // k_free gps info
            LOG_DBG("Over the GPS limit (%d): Freeing oldest gps info", CONFIG_MAX_GPS_DATA_POINTS);
            k_heap_free(&gps_heap, gps_info);
            gps_data_fifo_count--;
        }
    }

    // add gps info to list
    gps_info_t *new_gps_info = k_heap_alloc(&gps_heap, sizeof(gps_info_t), K_NO_WAIT);
    if (!new_gps_info) {
        LOG_ERR("Failed to allocate memory for gps info");
        return;
    }
    LOG_DBG("Adding new gps info to list(%d)", gps_data_fifo_count);
    memcpy(new_gps_info, gps_info, sizeof(gps_info_t));
    k_fifo_alloc_put(&gps_data_fifo, new_gps_info);
    gps_data_fifo_count++;
}

// get gps list as json
int gps_get_json_list(cJSON *jsonObj, int16_t *remaining_space)
{
    if (k_fifo_is_empty(&gps_data_fifo)) {
        return -1;
    }

    if (*remaining_space < 10) {
        return -ENOMEM;
    }

    cJSON *gps_array = cJSON_AddArrayToObject(jsonObj, "GPS");
    if (!gps_array) {
        LOG_ERR("Failed to create gps array");
        return -1;
    }
    *remaining_space -= sizeof("GPS") + 5;    // for 2x" 2x[ and 1x:
    char tempSpace[128];
    while (!k_fifo_is_empty(&gps_data_fifo)) {
        gps_info_t *gps_info = k_fifo_peek_head(&gps_data_fifo);
        if (gps_info->latitude == 0 || gps_info->longitude == 0) {
            gps_info = k_fifo_get(&gps_data_fifo, K_NO_WAIT);    // pop the message off the fifo
            if (gps_info) {
                k_heap_free(&gps_heap, gps_info);
            }
            continue;
        }
        cJSON *gps_obj = cJSON_CreateObject();
        if (!gps_obj) {
            LOG_ERR("Failed to create gps object");
            return -1;
        }
        cJSON_AddNumberToObject(gps_obj, "LAT", round_float((gps_info->latitude / 1000000.0), 6));
        cJSON_AddNumberToObject(gps_obj, "LONG", round_float((gps_info->longitude / 1000000.0), 6));
        cJSON_AddNumberToObject(gps_obj, "ALT", round_float((gps_info->altitude / 100.0), 2));
        cJSON_AddNumberToObject(gps_obj, "CRON", gps_info->timestamp);
        cJSON_AddNumberToObject(gps_obj, "ACC", round_float((gps_info->accuracy / 100.0), 2));

        cJSON_PrintPreallocated(gps_obj, tempSpace, 128, false);
        LOG_DBG("New entry size: %d    remaing_space: %d", strlen(tempSpace), *remaining_space);
        if (*remaining_space < strlen(tempSpace)) {
            cJSON_Delete(gps_obj);
            return 1;    // we're out of space and need to continue on the next loop
        }
        gps_info = k_fifo_get(&gps_data_fifo, K_NO_WAIT);    // pop the message off the fifo
        if (gps_info) {
            k_heap_free(&gps_heap, gps_info);
        }
        *remaining_space -= strlen(tempSpace);

        cJSON_AddItemToArray(gps_array, gps_obj);
        gps_data_fifo_count--;
    }
    gps_clear();
    return 0;
}

// int64_t latitude;          // float * 10000 to get integer value
// int64_t longitude;         // float * 10000 to get integer value
// int32_t altitude;          // float * 100 to get integer value
// uint16_t accuracy;         // float * 100 to get integer value
// uint16_t speed;            // float * 100 to get integer value
// uint16_t heading;		  // float * 100 to get integer value
// uint16_t speed_accuracy;   // float * 100 to get integer value
// uint16_t numSatellites;     // number of satellites used by gnss system
// int32_t secSinceLock;      // seconds since last lock
// int32_t timeToLock;         // seconds to lock
// uint64_t timestamp;         // seconds since epoch

// return gps list count
int gps_get_count()
{
    // return gps list count
    return gps_data_fifo_count;
}

// clear gps list
void gps_clear()
{
    // clear gps list
    gps_info_t *gps_info = k_fifo_get(&gps_data_fifo, K_NO_WAIT);
    while (gps_info) {
        k_heap_free(&gps_heap, gps_info);
        gps_info = k_fifo_get(&gps_data_fifo, K_NO_WAIT);
    }
    gps_data_fifo_count = 0;
}

// get most recent gps value - for shell cmd
gps_info_t *gps_get_last()
{
    // get most recent gps value
    gps_info_t *gps_info = k_fifo_peek_tail(&gps_data_fifo);
    if (gps_info) {
        return gps_info;
    }
    return NULL;
}
