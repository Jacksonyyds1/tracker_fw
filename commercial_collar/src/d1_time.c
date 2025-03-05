/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#include "d1_time.h"
#include <stddef.h>
#include <errno.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <zephyr/sys/timeutil.h>
#if defined(CONFIG_ARCH_POSIX) && defined(CONFIG_EXTERNAL_LIBC)
#include <time.h>
#else
#include <zephyr/posix/time.h>
#endif
#include "imu.h"
#include "ml.h"

LOG_MODULE_REGISTER(d1_time, CONFIG_SPI_MODEM_LOG_LEVEL);
static bool time_set_5340 = false;

bool is_5340_time_set()
{
    return time_set_5340;
}

static int get_y_m_d(struct tm *t, char *date_str)
{
    int   year;
    int   month;
    int   day;
    char *endptr;

    endptr = NULL;
    year   = strtol(date_str, &endptr, 10);
    if ((endptr == date_str) || (*endptr != '-')) {
        return -EINVAL;
    }

    date_str = endptr + 1;

    endptr = NULL;
    month  = strtol(date_str, &endptr, 10);
    if ((endptr == date_str) || (*endptr != '-')) {
        return -EINVAL;
    }

    if ((month < 1) || (month > 12)) {
        LOG_DBG("Invalid month");
        return -EINVAL;
    }

    date_str = endptr + 1;

    endptr = NULL;
    day    = strtol(date_str, &endptr, 10);
    if ((endptr == date_str) || (*endptr != '\0')) {
        return -EINVAL;
    }

    /* Check day against maximum month length */
    if ((day < 1) || (day > 31)) {
        LOG_DBG("Invalid day");
        return -EINVAL;
    }

    t->tm_year = year - 1900;
    t->tm_mon  = month - 1;
    t->tm_mday = day;

    return 0;
}

static int get_h_m_s(struct tm *t, char *time_str)
{
    char *endptr;

    if (*time_str == ':') {
        time_str++;
    } else {
        endptr     = NULL;
        t->tm_hour = strtol(time_str, &endptr, 10);
        if (endptr == time_str) {
            return -EINVAL;
        } else if (*endptr == ':') {
            if ((t->tm_hour < 0) || (t->tm_hour > 23)) {
                LOG_DBG("Invalid hour");
                return -EINVAL;
            }

            time_str = endptr + 1;
        } else {
            return -EINVAL;
        }
    }

    if (*time_str == ':') {
        time_str++;
    } else {
        endptr    = NULL;
        t->tm_min = strtol(time_str, &endptr, 10);
        if (endptr == time_str) {
            return -EINVAL;
        } else if (*endptr == ':') {
            if ((t->tm_min < 0) || (t->tm_min > 59)) {
                LOG_DBG("Invalid minute");
                return -EINVAL;
            }

            time_str = endptr + 1;
        } else {
            return -EINVAL;
        }
    }

    endptr    = NULL;
    t->tm_sec = strtol(time_str, &endptr, 10);
    if ((endptr == time_str) || (*endptr != '\0')) {
        return -EINVAL;
    }

    /* Note range allows for a leap second */
    if ((t->tm_sec < 0) || (t->tm_sec > 60)) {
        LOG_DBG("Invalid second");
        return -EINVAL;
    }

    return 0;
}

////////////////////////////////////////////////////
// modem_set_5340_time()
//  Set the time based on a time/date string in the
// format of:
// shell   [Y-m-d] <H:M:S>
// DA -    +TIME:2024-04-03,18:52:35
// 9160  - 24/04/03,18:52:38-28
//
//  @param time the time string to set
//
//  @return 0 on success, <0 on error
int d1_set_5340_time(char *timeStr, bool force)
{
    if (time_set_5340 && !force) {
        return 0;
    }
    if ((timeStr == NULL) || (strlen(timeStr) < 16)) {
        return -EINVAL;
    }
    struct timespec tp;
    struct tm       tm;
    int             ret;

    char date[16];
    char time[16];
    memset(date, 0, 16);
    memset(time, 0, 16);

    // shell   [Y-m-d] <H:M:S>
    // DA -    +TIME:2024-04-03,18:52:35
    // 9160  - 24/04/03,18:52:38-28

    // test if "+TIME:" is in the string
    // branch on that and change the string so both formats match
    if (strstr(timeStr, "+TIME:") != NULL) {
        // DA format
        // +TIME:2024-04-03,18:52:35
        // 24/04/03,18:52:38-28

        char year[5];
        int  year_i = 0;

        // TODO: this section needs testing, not used yet
        char *colon = strchr(timeStr, ':');
        if (colon == NULL) {
            LOG_DBG("Failed to find colon in |%s|", timeStr);
            return -EINVAL;
        }
        char *comma = strchr(timeStr, ',');
        if (comma == NULL) {
            LOG_DBG("Failed to find comma in time str");
            return -EINVAL;
        }
        strncpy(date, colon + 1, MIN(comma - colon - 1, 15));
        strncpy(time, comma + 1, 8);
        char *dash = strchr(date, '-');
        if (dash == NULL) {
            LOG_DBG("Failed to find dash in |%s|", timeStr);
            return -EINVAL;
        }
        strncpy(year, date, MIN(dash - date, 4));
        year_i = atoi(year);
        if (year_i < 2024) {
            LOG_DBG("Invalid year");
            return -EINVAL;
        }
        LOG_DBG("DA format: %s %s", date, time);
    } else {
        // 9160 format
        // 24/04/03,18:52:38-28
        // +TIME:2024-04-03,18:52:35
        // 24/05/23,20:49:21+00
        char *comma = strchr(timeStr, ',');
        if (comma == NULL) {
            LOG_DBG("Failed to find comma in |%s|", timeStr);
            return -EINVAL;
        }
        char *dash = strchr(timeStr, '-');
        char *plus = strchr(timeStr, '+');
        if (plus != NULL) {
            strncpy(time, comma + 1, MIN(plus - comma - 1, 15));
        } else if (dash != NULL) {
            strncpy(time, comma + 1, MIN(dash - comma - 1, 15));
        } else {
            LOG_DBG("Failed to find dash or plus in |%s|", timeStr);
            return -EINVAL;
        }

        strncat(date, "20", 3);    // 9160 reports 24, so we need to add 20
        strncat(date, timeStr, MIN(comma - timeStr, 14));
        char *tmpPtr = date;
        while ((dash = strchr(tmpPtr, '/'))) {
            *dash  = '-';
            tmpPtr = dash + 1;
        }
        // LOG_DBG("9160 format: %s %s", date, time);
    }

    clock_gettime(CLOCK_REALTIME, &tp);

    ret = get_y_m_d(&tm, date);
    if (ret != 0) {
        LOG_DBG("Failed to get date");
        return -EINVAL;
    }
    ret = get_h_m_s(&tm, time);
    if (ret != 0) {
        LOG_DBG("Failed to get time");
        return -EINVAL;
    }


    tp.tv_sec = timeutil_timegm(&tm);
    if (tp.tv_sec == -1) {
        LOG_WRN("Failed to calculate seconds since Epoch");
        return -EINVAL;
    }
    tp.tv_nsec = 0;

    ret = clock_settime(CLOCK_REALTIME, &tp);
    if (ret != 0) {
        LOG_WRN("Could not set date %d", ret);
        return -EINVAL;
    }

    LOG_WRN("Time set to %s %s", date, time);
    time_set_5340 = true;

#if defined(CONFIG_ML_ENABLE)
    LOG_INF("Starting ML");
    ml_start();
#endif
    // LOG_DBG(
    //     "%d-%02u-%02u "
    //     "%02u:%02u:%02u UTC",
    //     tm.tm_year + 1900,
    //     tm.tm_mon + 1,
    //     tm.tm_mday,
    //     tm.tm_hour,
    //     tm.tm_min,
    //     tm.tm_sec);

    return 0;
}

void do_d1_get_time(const struct shell *sh, size_t argc, char **argv)
{
    struct timespec tp;
    struct tm       tm;
    char            buf[64];
    int             ret;

    clock_gettime(CLOCK_REALTIME, &tp);
    gmtime_r(&tp.tv_sec, &tm);

    ret = strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    if (ret == 0) {
        LOG_WRN("Failed to format time");
        return;
    }

    shell_print(sh, "%s", buf);
}

void do_d1_set_time(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_print(sh, "Usage: %s <time>\n", argv[0]);
        return;
    }
    int ret = d1_set_5340_time(argv[1], true);
    if (ret != 0) {
        shell_print(sh, "Failed to set time\n");
    } else {
        shell_print(sh, "Time set\n");
    }
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_d1time,
    SHELL_CMD(get, NULL, "get current time", do_d1_get_time),
    SHELL_CMD(set, NULL, "set time with string", do_d1_set_time),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(d1time, &sub_d1time, "Commands to test the d1_time", NULL);
