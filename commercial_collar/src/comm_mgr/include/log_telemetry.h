/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */

#pragma once
#include "d1_zbus.h"

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>

void telem_log_clear();
int  log_telemetry_add(const char *log_msg, int loglevel, int log_type);
int  telem_log_count();
int  telem_log_get_json_list(cJSON *jsonObj, int16_t *remaining_space);

// TODO: put Heather's list here
enum telemLogTypes
{
    TELEMETRY_LOG_TYPE_UNKNOWN = -1,
    TELEMETRY_LOG_TYPE_INFO    = 1,
    TELEMETRY_LOG_TYPE_DEBUG   = 2,
    TELEMETRY_LOG_TYPE_ERROR   = 3,
};

#if 1
#define LOG_TELEMETRY_DBG(logType, fmt, ...) \
    do {                                     \
        ARG_UNUSED(logType);                 \
    } while (0)
#define LOG_TELEMETRY_INF(logType, fmt, ...) \
    do {                                     \
        ARG_UNUSED(logType);                 \
    } while (0)
#define LOG_TELEMETRY_WRN(logType, fmt, ...) \
    do {                                     \
        ARG_UNUSED(logType);                 \
    } while (0)
#define LOG_TELEMETRY_ERR(logType, fmt, ...) \
    do {                                     \
        ARG_UNUSED(logType);                 \
    } while (0)

#else
#define LOG_TELEMETRY_DBG(logType, fmt, ...)                                                \
    do {                                                                                    \
        char log_telemetry_str[CONFIG_MAX_TELEMETRY_LOG_MSG_SIZE];                          \
        snprintf(log_telemetry_str, CONFIG_MAX_TELEMETRY_LOG_MSG_SIZE, fmt, ##__VA_ARGS__); \
        log_telemetry_add(log_telemetry_str, 4, logType);                                   \
        LOG_DBG(fmt, ##__VA_ARGS__);                                                        \
    } while (0)

#define LOG_TELEMETRY_INF(logType, fmt, ...)                                                \
    do {                                                                                    \
        char log_telemetry_str[CONFIG_MAX_TELEMETRY_LOG_MSG_SIZE];                          \
        snprintf(log_telemetry_str, CONFIG_MAX_TELEMETRY_LOG_MSG_SIZE, fmt, ##__VA_ARGS__); \
        log_telemetry_add(log_telemetry_str, 3, logType);                                   \
        LOG_INF(fmt, ##__VA_ARGS__);                                                        \
    } while (0)

#define LOG_TELEMETRY_WRN(logType, fmt, ...)                                                \
    do {                                                                                    \
        char log_telemetry_str[CONFIG_MAX_TELEMETRY_LOG_MSG_SIZE];                          \
        snprintf(log_telemetry_str, CONFIG_MAX_TELEMETRY_LOG_MSG_SIZE, fmt, ##__VA_ARGS__); \
        log_telemetry_add(log_telemetry_str, 2, logType);                                   \
        LOG_WRN(fmt, ##__VA_ARGS__);                                                        \
    } while (0)

#define LOG_TELEMETRY_ERR(logType, fmt, ...)                                                \
    do {                                                                                    \
        char log_telemetry_str[CONFIG_MAX_TELEMETRY_LOG_MSG_SIZE];                          \
        snprintf(log_telemetry_str, CONFIG_MAX_TELEMETRY_LOG_MSG_SIZE, fmt, ##__VA_ARGS__); \
        log_telemetry_add(log_telemetry_str, 1, logType);                                   \
        LOG_ERR(fmt, ##__VA_ARGS__);                                                        \
    } while (0)
#endif

typedef struct telemetry_log_data
{
    uint32_t _reserved;
    char    *log;
    int      log_type;
    int      log_level;
    uint64_t timestamp;
} telemetry_log_data_t;