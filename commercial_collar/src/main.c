/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
/*
 *
 * SPDX-License-Identifier: LicenseRef-Proprietary
 */

#include <stdlib.h>
#include <getopt.h>

#include <zephyr/kernel.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/heap_listener.h>

#include "app_version.h"
#include "ble.h"
#include "imu.h"
#include "login.h"
#include "modem.h"
#include "ml.h"
#include "wifi.h"
#include "storage.h"
#include "net_mgr.h"
#include "uicr.h"
#include "watchdog.h"
#include "wi.h"

#include "pmic.h"
#include "fota.h"
extern shadow_doc_t              shadow_doc;
LOG_MODULE_REGISTER(main, LOG_LEVEL_DBG);

#define SLEEP_TIME_MS 100

void fota_timer_check_work_handler(struct k_work *work)
{
    LOG_INF("Checking for FOTA updates");
    fota_update_all_devices();
}

K_WORK_DEFINE(fota_timer_check_work, fota_timer_check_work_handler);
void fota_timer_check_timer_cb(struct k_timer *dummy)
{
    k_work_submit(&fota_timer_check_work);
}
K_TIMER_DEFINE(fota_timer_check_timer, fota_timer_check_timer_cb, NULL);

bool initDevices()
{
    wr_init();
    if (pmic_init(true, true)) {
        LOG_ERR("Error: PMIC initialization failed.\n");
        return false;
    }

    // NB we must init IMU and ML before modem, since the modem will try to start
    // ML as soon as it has a date

    if (imu_init(shadow_doc.mot_det)) {
        LOG_ERR("Error: IMU initialization failed.");
        return false;
    }

    if (ml_init()) {
        LOG_ERR("Error: ML initialization failed.");
        return false;
    }

#if (CONFIG_WATCHDOG)
    if (watchdog_init()) {
        LOG_ERR("Error: watchdog initialization failed.\n");
        return false;
    }
#endif

    if (modem_init()) {
        LOG_ERR("Error: 9160 Modem initialization failed.");
        return false;
    }

    if (wifi_init()) {
        LOG_ERR("Error: Wifi initialization failed.");
        return false;
    }

    if (net_mgr_init()) {
        LOG_ERR("Error: Wifi app initialization failed.");
        return false;
    }

    if (ble_init()) {
        LOG_ERR("Error: BLE initialization failed.");
        return false;
    }

    return true;
}

#ifdef CONFIG_SYS_HEAP_LISTENER

extern struct k_heap _system_heap;
extern struct k_heap log_heap;
extern struct k_heap wifi_heap;
extern struct k_heap mqtt_heap;
extern struct k_heap gps_heap;

static struct
{
    const char    *heap_name;
    struct k_heap *heap;
} heaps[] = { { "system_heap", &_system_heap },
              { "log_heap", &log_heap },
              { "wifi_heap", &wifi_heap },
              { "mqtt_heap", &mqtt_heap },
              { "gps_heap", &gps_heap } };

#define N_HEAP_BLKS 512
struct heap_usage
{
    void   *ptr;
    size_t  bytes;
    k_tid_t caller;
} heap_usage[N_HEAP_BLKS];

void on_heap_alloc(uintptr_t heap_id, void *mem, size_t bytes)
{
    for (int i = 0; i < N_HEAP_BLKS; i++) {
        if (heap_usage[i].ptr == NULL) {
            heap_usage[i].ptr    = mem;
            heap_usage[i].bytes  = bytes;
            heap_usage[i].caller = k_current_get();
            return;
        }
    }
    LOG_INF("Too many allocated blocks to track! alloc %zu at %p", bytes, mem);
}
HEAP_LISTENER_ALLOC_DEFINE(alloc_listener, HEAP_ID_FROM_POINTER(&_system_heap.heap), on_heap_alloc);

void on_heap_free(uintptr_t heap_id, void *mem, size_t bytes)
{
    for (int i = 0; i < N_HEAP_BLKS; i++) {
        if (heap_usage[i].ptr == mem) {
            heap_usage[i].ptr    = NULL;
            heap_usage[i].bytes  = 0;
            heap_usage[i].caller = 0;
            return;
        }
    }
    LOG_INF("Freeing untracked mem @ %p", mem);
}
HEAP_LISTENER_FREE_DEFINE(free_listener, HEAP_ID_FROM_POINTER(&_system_heap.heap), on_heap_free);

static void print_stats(const struct shell *sh, int heap_idx)
{
    int                     err;
    struct sys_memory_stats stats;

    err = sys_heap_runtime_stats_get(&heaps[heap_idx].heap->heap, &stats);
    if (err) {
        shell_error(sh, "Failed to read kernel system heap statistics (err %d)", err);
        return;
    }

    shell_print(
        sh,
        "\n%s (%d%%):",
        heaps[heap_idx].heap_name,
        stats.allocated_bytes * 100 / (stats.free_bytes + stats.allocated_bytes));
    shell_print(sh, "\tfree:           %zu", stats.free_bytes);
    shell_print(sh, "\tallocated:      %zu", stats.allocated_bytes);
    shell_print(sh, "\tmax. allocated: %zu", stats.max_allocated_bytes);
}

void do_print_heap(const struct shell *sh, int argc, char **argv)
{
    for (int i = 0; i < N_HEAP_BLKS; i++) {
        if (heap_usage[i].ptr) {
            shell_print(
                sh,
                "Alloc %zu @ %p from %s",
                heap_usage[i].bytes,
                heap_usage[i].ptr,
                k_thread_name_get(heap_usage[i].caller));
        }
    }
    for (int i = 0; i < ARRAY_SIZE(heaps); i++) {
        print_stats(sh, i);
    }
}

SHELL_CMD_REGISTER(heap, NULL, "Print heap blocks in use", do_print_heap);
#endif

////////////////////////////////////////////////////////////////////////////////////
//
// Main
//
int main(void)
{
    /*
	   Before anything else,
	   mark the currently running firmware image as OK,
	   which will install it permanently after an OTA
	 */
    boot_write_img_confirmed();

#ifdef CONFIG_SYS_HEAP_LISTENER
    heap_listener_register(&alloc_listener);
    heap_listener_register(&free_listener);
#endif

    if (!initDevices()) {
        LOG_ERR("Error: Device initialization failed.\n");
        return -1;
    }

    storage_init();

    LOG_INF("Commercial Collar version %d.%d.%d (%s)", APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_VERSION_PATCH, GIT_HASH);

    LOG_INF("Built on %s by %s", DBUILD_DATE, DBUILD_MACHINE);
#ifdef CONFIG_RELEASE_BUILD
    /*
	 * Protection checks for release firmware
	 * =====================================
	 * IF this unit is still in the factory (shipped flag is not set) then do nothing
	 *    (NB this is a potential vulnerability if you can clear the flag in the backup uicr
	 * copy) ELSE if the debug access port is locked, then disable the uart ELSE present the
	 * user with a login prompt
	 */
    if (IS_ENABLED(CONFIG_RELEASE_BUILD) && uicr_shipping_flag_get()) {
        // unit is ex-factory
        if (uicr_approtect_get() == 0) {
            if (!disable_console()) {
                logout();
            }
        } else {
            logout();
        }
    } else {
        // ensure we are logged in, despite the config settings
        shell_set_root_cmd(NULL);
        if (uicr_shipping_flag_get() == 0) {
            // still in factory ... drop all logging to WRN
            STRUCT_SECTION_FOREACH(shell, sh)
            {
                z_shell_log_backend_enable(sh->log_backend, (void *)sh, LOG_LEVEL_WRN);
            }
        }
    }
#endif

    k_timer_start(&fota_timer_check_timer, K_SECONDS(43200), K_SECONDS(43200));
}
