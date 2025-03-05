/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#include "log_telemetry.h"
#include "d1_zbus.h"
#include <zephyr/logging/log.h>
#include "utils.h"
#include "uicr.h"

LOG_MODULE_REGISTER(log_telem, CONFIG_LOG_TELEM_LOG_LEVEL);
K_HEAP_DEFINE(
    log_heap,
    (((sizeof(telemetry_log_data_t) + 16) * CONFIG_MAX_TELEMETRY_LOG_FIFO_SIZE)
     + (CONFIG_MAX_TELEMETRY_LOG_MSG_SIZE * CONFIG_MAX_TELEMETRY_LOG_FIFO_SIZE)));
// allocate more than needed for heap data collection and/or block alignment

K_FIFO_DEFINE(telemetry_log_data_fifo);
uint16_t telemetry_log_data_fifo_count = 0;

int free_log_msg(telemetry_log_data_t *telem_log_info)
{
    if (!telem_log_info) {
        return -EINVAL;
    }
    if (telem_log_info->log) {
        k_heap_free(&log_heap, telem_log_info->log);
    }
    k_heap_free(&log_heap, telem_log_info);
    return 0;
}

int log_telemetry_add(const char *log_msg, int loglevel, int log_type)
{
    while (telemetry_log_data_fifo_count >= CONFIG_MAX_TELEMETRY_LOG_FIFO_SIZE) {
        // remove oldest telemetry log
        telemetry_log_data_t *telemetry_log_data = k_fifo_get(&telemetry_log_data_fifo, K_NO_WAIT);
        if (telemetry_log_data) {
            // free telemetry log
            LOG_WRN(
                "Over the telemetry log limit (%d): Freeing oldest telemetry log: "
                "%p",
                telemetry_log_data_fifo_count,
                (void *)telemetry_log_data);
            LOG_WRN("log(%d): %s", strlen(telemetry_log_data->log), telemetry_log_data->log);
            free_log_msg(telemetry_log_data);
            telemetry_log_data_fifo_count--;
        }
    }

    telemetry_log_data_t *new_log_info = k_heap_alloc(&log_heap, sizeof(telemetry_log_data_t), K_NO_WAIT);
    if (!new_log_info) {
        LOG_ERR(
            "Failed to allocate memory for log message(%d/%d): %d",
            telemetry_log_data_fifo_count,
            CONFIG_MAX_TELEMETRY_LOG_FIFO_SIZE,
            sizeof(telemetry_log_data_t));
        return -ENOMEM;
    }
    new_log_info->log = NULL;
    new_log_info->log = k_heap_alloc(&log_heap, MIN(strlen(log_msg) + 1, CONFIG_MAX_TELEMETRY_LOG_MSG_SIZE), K_NO_WAIT);
    if (!new_log_info->log) {
        LOG_ERR("Failed to allocate memory for log message string");
        free_log_msg(new_log_info);
        return -ENOMEM;
    }

    memset(new_log_info->log, 0, MIN(strlen(log_msg) + 1, CONFIG_MAX_TELEMETRY_LOG_MSG_SIZE));
    strncpy(new_log_info->log, log_msg, MIN(strlen(log_msg), CONFIG_MAX_TELEMETRY_LOG_MSG_SIZE - 1));
    new_log_info->log_type  = log_type;
    new_log_info->log_level = loglevel;
    new_log_info->timestamp = get_unix_time();
    k_fifo_put(&telemetry_log_data_fifo, new_log_info);
    telemetry_log_data_fifo_count++;
    LOG_DBG("Added new telemetry log to list: %d", telemetry_log_data_fifo_count);
    return 0;
}

int format_telem_log_string(char *log_str, telemetry_log_data_t *telem_log_info, int str_len)
{
    if (!log_str || !telem_log_info) {
        return -EINVAL;
    }
    snprintf(
        log_str,
        CONFIG_MAX_TELEMETRY_LOG_MSG_SIZE + 100,
        "%lld,%d,%s",
        telem_log_info->timestamp,
        telem_log_info->log_type,
        telem_log_info->log);
    return 0;
}

// get gps list as json
int telem_log_get_json_list(cJSON *jsonObj, int16_t *remaining_space)
{
    if (k_fifo_is_empty(&telemetry_log_data_fifo)) {
        return -ENOENT;
    }

    if (*remaining_space < 64) {
        return -ENOMEM;
    }
    cJSON *log_array = cJSON_AddArrayToObject(jsonObj, "LOGS");
    if (!log_array) {
        LOG_ERR("Failed to create log array");
        return -1;
    }
    *remaining_space -= sizeof("LOGS") + 5;    // for 2x" 2x[ and 1x:

    char tempSpace[CONFIG_MAX_TELEMETRY_LOG_MSG_SIZE + 100];
    while (!k_fifo_is_empty(&telemetry_log_data_fifo)) {
        telemetry_log_data_t *telem_log_info = k_fifo_peek_head(&telemetry_log_data_fifo);
        if (telem_log_info) {
            char logFmtString[CONFIG_MAX_TELEMETRY_LOG_MSG_SIZE + 100];
            int  ret = format_telem_log_string(logFmtString, telem_log_info, CONFIG_MAX_TELEMETRY_LOG_MSG_SIZE + 100);
            if (ret != 0) {
                LOG_ERR("Failed to format log string");
                return ret;
            }
            cJSON *j_str = cJSON_CreateString(logFmtString);
            if (!j_str) {
                LOG_ERR("Failed to create log string");
                return ret;
            }
            // math to determine if we have enough space to add this log
            cJSON_PrintPreallocated(j_str, tempSpace, CONFIG_MAX_TELEMETRY_LOG_MSG_SIZE + 100, false);
            if (*remaining_space < strlen(tempSpace)) {
                cJSON_Delete(j_str);
                LOG_DBG("log_print telemetry log to list: %d", telemetry_log_data_fifo_count);
                return 1;    // we're out of space and need to continue on the next
                             // loop
            }

            telemetry_log_data_t *telem_log_info2 = k_fifo_get(
                &telemetry_log_data_fifo,
                K_NO_WAIT);    // pop the message off the fifo
            cJSON_AddItemToArray(log_array, j_str);
            *remaining_space -= strlen(tempSpace);
            telemetry_log_data_fifo_count--;
            if (telem_log_info2) {
                free_log_msg(telem_log_info2);
            }
        }
    }

    telem_log_clear();
    return 0;
}

int telem_log_count()
{
    // return log list count
    return telemetry_log_data_fifo_count;
}

void telem_log_clear()
{
    // clear log list
    telemetry_log_data_t *telem_log_info = k_fifo_get(&telemetry_log_data_fifo, K_NO_WAIT);
    while (telem_log_info) {
        free_log_msg(telem_log_info);
        telem_log_info = k_fifo_get(&telemetry_log_data_fifo, K_NO_WAIT);
    }
    telemetry_log_data_fifo_count = 0;
}
