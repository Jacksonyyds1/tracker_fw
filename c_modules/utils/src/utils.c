/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
/*
 *
 * SPDX-License-Identifier: LicenseRef-Proprietary
 */

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/logging/log.h>
#include <zephyr/posix/time.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/timeutil.h>
#if defined(CONFIG_ARCH_POSIX) && defined(CONFIG_EXTERNAL_LIBC)
#include <time.h>
#else
#include <zephyr/posix/time.h>
#endif
LOG_MODULE_REGISTER(utils, LOG_LEVEL_DBG);

// current time with millisecond resolution
uint64_t
utils_get_currentmillis(void)
{
    static uint64_t last_timestamp;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    uint64_t t = (ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
    __ASSERT(t >= last_timestamp, "clock_gettime error");
    last_timestamp = t;
    return t;
}

uint64_t
utils_get_currentmicros(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    uint64_t t = (ts.tv_sec) * 1000 + ts.tv_nsec / 1000;
    return t;
}

int
utils_set_currentmillis(uint64_t currentmillis)
{
    struct timespec ts;
    ts.tv_sec  = currentmillis / 1000;
    ts.tv_nsec = (currentmillis % 1000) * 1000000;
    int ret    = clock_settime(CLOCK_REALTIME, &ts);
    if (ret != 0) {
        LOG_ERR("Failed to set system time: ret=%d", ret);
        return -1;
    } else {
        LOG_DBG("set clock to %llu", currentmillis);
    }
    return 0;
}

uint64_t
utils_get_utc(void)
{
    struct timespec tp;
    struct tm       tm;
    clock_gettime(CLOCK_REALTIME, &tp);
    gmtime_r(&tp.tv_sec, &tm);
    int64_t currTime = timeutil_timegm64(&tm);
    return currTime;
}


// load settings value stuff.  TODO: why isn't there an API already in settings to do this?
// why so complicated?!
struct direct_immediate_value
{
    size_t  len;
    void   *dest;
    uint8_t fetched;
};

static int
direct_loader_immediate_value(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg, void *param)
{
    const char                    *next;
    size_t                         name_len;
    int                            rc;
    struct direct_immediate_value *one_value = (struct direct_immediate_value *)param;

    name_len = settings_name_next(name, &next);

    if (name_len == 0) {
        if (len == one_value->len) {
            rc = read_cb(cb_arg, one_value->dest, len);
            if (rc >= 0) {
                one_value->fetched = 1;
                return 0;
            }

            LOG_ERR("failed to load, rc=%d", rc);
            return rc;
        }
        return -EINVAL;
    }

    /* other keys aren't served by the callback
	 * Return success in order to skip them
	 * and keep storage processing.
	 */
    return 0;
}
int
utils_load_setting(const char *name, void *dest, size_t len)
{
    int                           rc;
    struct direct_immediate_value dov;

    dov.fetched = 0;
    dov.len     = len;
    dov.dest    = dest;

    rc = settings_load_subtree_direct(name, direct_loader_immediate_value, (void *)&dov);
    if (rc == 0) {
        if (!dov.fetched) {
            rc = -ENOENT;
        }
    }

    return rc;
}