/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#include "commMgr.h"
#include "fota.h"
#include "net_mgr.h"
#include "modem.h"
#include "wifi.h"
#include "wifi_at.h"
#include <cJSON_os.h>
#include "d1_json.h"
#include "modem_interface_types.h"
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include "app_version.h"
#include "nrf53_upgrade.h"
#include "watchdog.h"
#include <zephyr/fs/fs.h>
#include "tracker_service.h"
#include "radioMgr.h"

LOG_MODULE_REGISTER(comm_mgr_fota, CONFIG_COMM_MGR_LOG_LEVEL);

#define FILENAME_MATCH_FOR_9160    "nrf9160"
#define FILENAME_MATCH_FOR_da16200 "da16200"
#define FILENAME_MATCH_FOR_5340    "nrf5340"

static const char *const nrf_fota_error_strings[] = { [FOTA_DOWNLOAD_ERROR_CAUSE_NO_ERROR]        = "No Error",
                                                      [FOTA_DOWNLOAD_ERROR_CAUSE_DOWNLOAD_FAILED] = "Download Failed",
                                                      [FOTA_DOWNLOAD_ERROR_CAUSE_INVALID_UPDATE]  = "Invalid Update",
                                                      [FOTA_DOWNLOAD_ERROR_CAUSE_TYPE_MISMATCH]   = "Type Mismatch",
                                                      [FOTA_DOWNLOAD_ERROR_CAUSE_INTERNAL]        = "Internal Error",
                                                      [FOTA_DOWNLOAD_ERROR_MAX_ERROR]             = "Unknown Error" };

static const int nestle_fota_error_codes[] = {
    [FOTA_DOWNLOAD_ERROR_CAUSE_NO_ERROR] = 0,          [FOTA_DOWNLOAD_ERROR_CAUSE_DOWNLOAD_FAILED] = -301,
    [FOTA_DOWNLOAD_ERROR_CAUSE_INVALID_UPDATE] = -600, [FOTA_DOWNLOAD_ERROR_CAUSE_TYPE_MISMATCH] = -600,
    [FOTA_DOWNLOAD_ERROR_CAUSE_INTERNAL] = -600,       [FOTA_DOWNLOAD_ERROR_MAX_ERROR] = -99
};

// save current RID foreach device, just in case we do simultaneous updates
typedef struct
{
    char     request_id[64];
    uint8_t  target_version[3];
    char     file_name_and_path[64];
    bool     requested;
    bool     response_received;
    bool     in_progress;
    bool     complete;
    uint64_t last_state_time;
} fota_device_t;
char          g_final_url[2000] = { 0 };
fota_device_t nrf9160_fota      = { 0 };
fota_device_t da16200_fota      = { 0 };
fota_device_t nrf5340_fota      = { 0 };
bool          fota_in_progress  = false;

extern int httpresultcode;

#define FOTA_STATE_MAX_TIME_IN_SEC 60

extern da_state_t da_state;

// TODO: this is not the best solution, what happens if someone requests an update while we are
// already checking/updating?
static comm_device_type_t update_device_type       = COMM_DEVICE_NONE;
static bool               update_check_in_progress = false;

static void fota_in_progress_check_work();
K_WORK_DEFINE(commMgrFota_in_progress_work, fota_in_progress_check_work);
static void schedule_fota_in_progress_check_work()
{
    k_work_submit(&commMgrFota_in_progress_work);
}
K_TIMER_DEFINE(fota_in_progress_check_timer, schedule_fota_in_progress_check_work, NULL);

static void fota_version_check_work(struct k_work *item);
K_WORK_DEFINE(commMgrFota_check_version_work, fota_version_check_work);
static void schedule_fota_check_version_work(struct k_timer *dummy)
{
    k_work_submit(&commMgrFota_check_version_work);
}
K_TIMER_DEFINE(fota_version_check_timer, schedule_fota_check_version_work, NULL);

static void fota_work_callback(struct k_work *item);
K_WORK_DEFINE(commMgrFota_work, fota_work_callback);
static void schedule_fota_work(struct k_timer *dummy)
{
    k_work_submit(&commMgrFota_work);
}
K_TIMER_DEFINE(fota_check_timer, schedule_fota_work, NULL);

/////////////////////////////////////////////////
// fota_check_for_fota_in_progress()
// External api
void fota_check_for_fota_in_progress(struct k_timer *dummy)
{
    k_timer_start(&fota_in_progress_check_timer, K_SECONDS(shadow_doc.fota_in_progress_duration), K_NO_WAIT);
}

static void da_http_monitor_work(struct k_work *work);
K_WORK_DEFINE(commMgrFota_monitor_work, da_http_monitor_work);

int g_last_http_amount = 0;

static void schedule_da_http_monitor_work(struct k_timer *dummy)
{
    k_work_submit(&commMgrFota_monitor_work);
}
K_TIMER_DEFINE(fota_da_http_monitor_timer, schedule_da_http_monitor_work, NULL);

static void da_http_monitor_work(struct k_work *work)
{
    // Check that the http dowload is still in progress
    int amt = wifi_at_get_http_amt_downloaded();
    LOG_WRN("da16200 download monitor - checking download: %d bytes so far", amt);
    if (amt == g_last_http_amount) {
        LOG_WRN("da16200 download monitor - download stalled, signal failure");
        fota_status_update(2, FOTA_DOWNLOAD_ERROR_CAUSE_DOWNLOAD_FAILED, COMM_DEVICE_NRF5340);
        k_timer_stop(&fota_da_http_monitor_timer);
        return;
    }
    g_last_http_amount = amt;
}
static void fota_start_da_http_monitoring()
{
    g_last_http_amount = 0;
    k_timer_start(&fota_da_http_monitor_timer, K_SECONDS(10), K_SECONDS(10));
}
static void fota_stop_da_http_monitoring()
{
    k_timer_stop(&fota_da_http_monitor_timer);
}

/////////////////////////////////////////////////
// fota_set_in_progress_timer()
// External api
int fota_set_in_progress_timer(int dur, bool save)
{
    if (dur <= 0 && dur > 200) {
        LOG_ERR("Invalid value: %d", dur);
        return -EINVAL;
    }

    if (dur != shadow_doc.fota_in_progress_duration) {
        shadow_doc.fota_in_progress_duration = dur;
        if (save) {
            write_shadow_doc();
        }
        LOG_DBG("Changed reconnect work interval to %d sec", dur);
    }
    return 0;
}

static void fota_in_progress_check_work()
{
    LOG_DBG("Checking for nrf5340 FOTA in progress");
    int              ret;
    struct fs_file_t update_notes;
    fs_file_t_init(&update_notes);
    struct fs_dirent entry;

    if (fs_stat("/lfs1/fota_in_progress.txt", &entry) != 0) {
        // LOG_DBG("No nrf5340 FOTA-in-progress found");
        return;
    }

    if ((ret = fs_open(&update_notes, "/lfs1/fota_in_progress.txt", FS_O_READ)) != 0) {
        LOG_DBG("Cannot open nrf5340 FOTA-in-progress");
        return;
    }

    char file_data[64] = { 0 };
    if (fs_read(&update_notes, file_data, 64) < 0) {
        LOG_ERR("Failed to read nrf5340 FOTA-in-progress");
        fs_close(&update_notes);
        return;
    }
    if (fs_close(&update_notes) < 0) {
        LOG_ERR("Failed to close nrf5340 FOTA-in-progress");
        return;
    }

    nrf5340_fota.in_progress = true;

    // read 4 elements from string, split by '\n'
    char *token = strtok(file_data, "\n");
    if (token != NULL) {
        strncpy(nrf5340_fota.request_id, token, 63);
        nrf5340_fota.request_id[63] = '\0';
    }
    token = strtok(NULL, "\n");
    if (token != NULL) {
        nrf5340_fota.target_version[0] = atoi(token);
    }
    token = strtok(NULL, "\n");
    if (token != NULL) {
        nrf5340_fota.target_version[1] = atoi(token);
    }
    token = strtok(NULL, "\n");
    if (token != NULL) {
        nrf5340_fota.target_version[2] = atoi(token);
    }
    LOG_DBG(
        "Found FOTA-in-progress for nrf5340 - request_id: %s, target version: %d.%d.%d",
        nrf5340_fota.request_id,
        nrf5340_fota.target_version[0],
        nrf5340_fota.target_version[1],
        nrf5340_fota.target_version[2]);

    if ((nrf5340_fota.target_version[0] == APP_VERSION_MAJOR) && (nrf5340_fota.target_version[1] == APP_VERSION_MINOR)
        && (nrf5340_fota.target_version[2] == APP_VERSION_PATCH)) {
        LOG_WRN("FOTA for nrf5340 - target version matches current version, FOTA successful.");
        fota_notify("5340_FOTA_DONE");
        // This is the last update of the 3 processors (for now) so since there is
        // files present, we know we compelted, we simply keep trying to send the done
        // until we do. No retry limit for now

        if (fota_status_update(3, 100, COMM_DEVICE_NRF5340) != 0) {
            LOG_ERR("Failed to send FOTA complete message for nrf5340, will retry");
            // retry in 10 seconds
            fota_check_for_fota_in_progress(NULL);
        }

    } else {
        LOG_ERR(
            "FOTA for nrf5340 - target version does not match current version, FOTA "
            "failed.");
        fota_notify("5340_FOTA_FAIL");
        fota_status_update(2, FOTA_DOWNLOAD_ERROR_CAUSE_INTERNAL, COMM_DEVICE_NRF5340);
    }

    nrf5340_fota.complete          = false;
    nrf5340_fota.requested         = false;
    nrf5340_fota.response_received = false;
    nrf5340_fota.in_progress       = false;
    commMgr_fota_end(COMM_DEVICE_NRF5340);    // Let commMgr know that we are done with fota

    // deletet the file and the bin
    fs_unlink("/lfs1/fota_in_progress.txt");
    fs_unlink("/lfs1/nrf5340_fota.bin");
    return;
}

/////////////////////////////////////////////////
// fota_handle_da_event()
// External api
int fota_handle_da_event(da_event_t status)
{
    int ret;
    if (status.events & DA_EVENT_TYPE_HTTP_COMPLETE) {
        fota_notify("5340_FLASH_START");
        fota_stop_da_http_monitoring();
        if (httpresultcode != 0) {
            LOG_WRN("FOTA for nrf5340 - download failed, HTTP result code: %d", httpresultcode);
            fota_status_update(2, FOTA_DOWNLOAD_ERROR_CAUSE_DOWNLOAD_FAILED, COMM_DEVICE_NRF5340);
            return 0;
        }
        if (nrf5340_fota.in_progress) {
            LOG_DBG("FOTA for nrf5340 - download complete, starting upgrade");
            // watchdog_disable();  // needed?  maybe extend time?   maybe kick now?

            char             file_data[65] = { 0 };
            struct fs_file_t update_notes;
            fs_file_t_init(&update_notes);
            fs_open(&update_notes, "/lfs1/fota_in_progress.txt", FS_O_WRITE | FS_O_CREATE);
            snprintf(file_data, 65, "%s\n", nrf5340_fota.request_id);
            ret = fs_write(&update_notes, file_data, strlen(file_data));
            snprintf(
                file_data,
                65,
                "%d\n%d\n%d\n",
                nrf5340_fota.target_version[0],
                nrf5340_fota.target_version[1],
                nrf5340_fota.target_version[2]);
            ret = fs_write(&update_notes, file_data, strlen(file_data));
            fs_close(&update_notes);

            int ret = nrf53_upgrade_with_file("/lfs1/nrf5340_fota.bin");
            if (ret != 0) {
                LOG_ERR("Failed to upgrade nrf5340 - %d", ret);
                fota_status_update(2, FOTA_DOWNLOAD_ERROR_CAUSE_INTERNAL, COMM_DEVICE_NRF5340);
            } else {
            }
            // "/lfs1/nrf5340_fota.bin"
            // call 5340 update from file
            // store off the new version, and fota-in-progress flag, on boot read and
            // delete the inprogress file and the bin file
        }
    }
    return 0;
}

static void fota_version_check_work(struct k_work *item)
{
    int ret;
    // see which device we're waiting on and check the version
    // if it matches the target version, then send the complete message
    // if it doesn't match, then send the error message

    if (nrf9160_fota.in_progress == true && nrf9160_fota.complete == false) {
        version_response_t *ver = get_cached_version_info();
        LOG_DBG(
            "Checking version for nrf9160 - current version: %d.%d.%d, target version: %d.%d.%d",
            ver->major,
            ver->minor,
            ver->patch,
            nrf9160_fota.target_version[0],
            nrf9160_fota.target_version[1],
            nrf9160_fota.target_version[2]);
        if ((ver->major == nrf9160_fota.target_version[0]) && (ver->minor == nrf9160_fota.target_version[1])
            && (ver->patch == nrf9160_fota.target_version[2])) {
            ret = fota_status_update(3, 100, COMM_DEVICE_NRF9160);
            if (ret != 0) {
                LOG_ERR(
                    "Failed to send FOTA complete message for nrf9160, will "
                    "retry");
                k_timer_start(&fota_version_check_timer, K_SECONDS(45), K_NO_WAIT);
                return;
            }
        } else {
            fota_status_update(2, FOTA_DOWNLOAD_ERROR_CAUSE_INTERNAL, COMM_DEVICE_NRF9160);
        }
    } else if (da16200_fota.in_progress == true && da16200_fota.complete == false) {
        LOG_DBG("Checking version for da16200");
        if ((da_state.version[0] == da16200_fota.target_version[0])
            && (da_state.version[1] == da16200_fota.target_version[1])
            && (da_state.version[2] == da16200_fota.target_version[2])) {
            ret = fota_status_update(3, 100, COMM_DEVICE_DA16200);
            if (ret != 0) {
                k_timer_start(&fota_version_check_timer, K_SECONDS(45), K_NO_WAIT);
                return;
            }
        } else {
            fota_status_update(2, FOTA_DOWNLOAD_ERROR_CAUSE_INTERNAL, COMM_DEVICE_DA16200);
        }
    }
}

static void fota_work_callback(struct k_work *item)
{
    int timer_period = 10;
    // start w/ 9160, then da16200, then 5340
    if (!nrf9160_fota.complete) {
        if (!nrf9160_fota.requested) {
            LOG_WRN("Checking for updates for nrf9160");
            if (check_for_updates(true, COMM_DEVICE_NRF9160) == 0) {
                goto reset_timer;
            }
        } else {
            if ((nrf9160_fota.last_state_time + (FOTA_STATE_MAX_TIME_IN_SEC * 1000)) < k_uptime_get()) {
                LOG_ERR("FOTA for nrf9160 - timeout waiting state change");
                nrf9160_fota.complete = true;
                nrf9160_fota.in_progress = false;
                commMgr_fota_end(COMM_DEVICE_NRF9160);    // Let commMgr know that we
                                                          // are done with fota
                goto reset_timer;
            }
            if (nrf9160_fota.response_received) {
                if (nrf9160_fota.in_progress) {
                    timer_period = 30;    // come back in 30 seconds
                    LOG_WRN("FOTA for nrf9160 - still in progress");
                    goto reset_timer;
                } else {
                    timer_period = 10;    // come back in 10 seconds
                    LOG_ERR("FOTA for nrf9160 - responded but not in progress, curious...");
                    goto reset_timer;
                }
            } else {
                // seems to be taking a while to respond
                timer_period = 10;    // come back in 10 seconds
                LOG_WRN("FOTA for nrf9160 - no response yet");
                goto reset_timer;
            }
        }
        goto reset_timer;
    }

    if (!da16200_fota.complete) {
        if (!da16200_fota.requested) {
            LOG_WRN("Checking for updates for da16200");
            if (check_for_updates(true, COMM_DEVICE_DA16200) == 0) {
                goto reset_timer;
            }
        } else {
            if ((da16200_fota.last_state_time + (FOTA_STATE_MAX_TIME_IN_SEC * 1000)) < k_uptime_get()) {
                LOG_ERR("FOTA for da16200 - timeout waiting state change");
                da16200_fota.complete = true;
                da16200_fota.in_progress = false;
                commMgr_fota_end(COMM_DEVICE_DA16200);    // Let commMgr know that we
                                                          // are done with fota
                goto reset_timer;
            }
            if (da16200_fota.response_received) {
                if (da16200_fota.in_progress) {
                    timer_period = 60;    // come back in 30 seconds
                    LOG_WRN("FOTA for da16200 - still in progress");
                    goto reset_timer;
                } else {
                    timer_period = 10;    // come back in 10 seconds
                    LOG_ERR("FOTA for da16200 - responded but not in progress, curious...");
                    goto reset_timer;
                }
            } else {
                // seems to be taking a while to respond
                timer_period = 10;    // come back in 10 seconds
                LOG_WRN("FOTA for da16200 - no response yet");
                goto reset_timer;
            }
        }
        goto reset_timer;
    }

    if (!nrf5340_fota.complete) {
        if (!nrf5340_fota.requested) {
            LOG_WRN("Checking for updates for nrf5340");
            if (check_for_updates(true, COMM_DEVICE_NRF5340) == 0) {
                goto reset_timer;
            }
        } else {
            if ((nrf5340_fota.last_state_time + (FOTA_STATE_MAX_TIME_IN_SEC * 1000)) < k_uptime_get()) {
                LOG_ERR("FOTA for nrf5340 - timeout waiting state change");
                nrf5340_fota.complete = true;
                nrf5340_fota.in_progress = false;
                commMgr_fota_end(COMM_DEVICE_NRF5340);    // Let commMgr know that we
                                                          // are done with fota
                goto reset_timer;
            }
            if (nrf5340_fota.response_received) {
                if (nrf5340_fota.in_progress) {
                    timer_period = 30;    // come back in 30 seconds
                    LOG_WRN("FOTA for nrf5340 - still in progress");
                    goto reset_timer;
                } else {
                    timer_period = 10;    // come back in 10 seconds
                    LOG_ERR("FOTA for nrf5340 - responded but not in progress, curious...");
                    goto reset_timer;
                }
            } else {
                // seems to be taking a while to respond
                timer_period = 10;    // come back in 10 seconds
                LOG_WRN("FOTA for nrf5340 - no response yet");
                goto reset_timer;
            }
        }
        goto reset_timer;
    }

    // cleanup:
    //  I guess we're done with all the fota's
    //  reset all the state structs and clear the in_progress flag
    LOG_DBG("FOTA check/update all complete");
    nrf9160_fota.complete          = false;
    nrf9160_fota.requested         = false;
    nrf9160_fota.response_received = false;
    nrf9160_fota.in_progress       = false;
    da16200_fota.complete          = false;
    da16200_fota.requested         = false;
    da16200_fota.response_received = false;
    da16200_fota.in_progress       = false;
    nrf5340_fota.complete          = false;
    nrf5340_fota.requested         = false;
    nrf5340_fota.response_received = false;
    nrf5340_fota.in_progress       = false;
    fota_in_progress               = false;
    commMgr_fota_end(COMM_DEVICE_NRF5340);    // Let commMgr know that we are done with fota
    commMgr_fota_end(COMM_DEVICE_NRF9160);    // Let commMgr know that we are done with fota
    commMgr_fota_end(COMM_DEVICE_DA16200);    // Let commMgr know that we are done with fota
    if (rm_get_active_mqtt_radio() != COMM_DEVICE_NRF9160) {
        modem_power_off();
    } else {
        LOG_DBG("Not powering off modem, active radio is nrf9160");
    }
    commMgr_enable_S_work(true);
    return;    // avoid the cleanup label

reset_timer:
    k_timer_start(&fota_check_timer, K_SECONDS(timer_period), K_NO_WAIT);
    return;
}

/////////////////////////////////////////////////
// fota_update_all_devices()
// External api
int fota_update_all_devices()
{
    // get it off this callback
    if (!fota_in_progress) {
        fota_in_progress = true;
        commMgr_enable_S_work(false);
        nrf9160_fota.complete          = false;
        nrf9160_fota.requested         = false;
        nrf9160_fota.response_received = false;
        nrf9160_fota.in_progress       = false;
        da16200_fota.complete          = false;
        da16200_fota.requested         = false;
        da16200_fota.response_received = false;
        da16200_fota.in_progress       = false;
        nrf5340_fota.complete          = false;
        nrf5340_fota.requested         = false;
        nrf5340_fota.response_received = false;
        nrf5340_fota.in_progress       = false;
        schedule_fota_work(NULL);
    }
    return 0;
}

static int get_current_version(comm_device_type_t device_type, int *major, int *minor, int *patch)
{
    LOG_DBG("Getting current version for %s", comm_dev_str(device_type));
    if (device_type == COMM_DEVICE_NRF9160) {
        // version_response_t*  ver = get_cached_version_info();
        version_response_t *ver;
        ver    = get_cached_version_info();
        *major = ver->major;
        *minor = ver->minor;
        *patch = ver->patch;
        return 0;
    } else if (device_type == COMM_DEVICE_DA16200) {
        // get the current version of the da16200
        *major = da_state.version[0];
        *minor = da_state.version[1];
        *patch = da_state.version[2];
        return 0;
    } else if (device_type == COMM_DEVICE_NRF5340) {
        // get the current version of the nrf5340
        *major = APP_VERSION_MAJOR;
        *minor = APP_VERSION_MINOR;
        *patch = APP_VERSION_PATCH;
        return 0;
    }
    return -1;
}

/////////////////////////////////////////////////
// check_for_updates()
// External api
int check_for_updates(bool do_update, comm_device_type_t device_type)
{
    char  msg_send_buf[512];
    char *machine_id = uicr_serial_number_get();
    int   ret;

    char nonce[9] = "12345678";
    int  major    = 0;
    int  minor    = 0;
    int  patch    = 0;
    if (get_current_version(device_type, &major, &minor, &patch) == 0) {
        LOG_WRN(
            "Checking for updates for %s - current version is: %d.%d.%d", comm_dev_str(device_type), major, minor, patch);
        char *fota_request = json_fota_check(machine_id, major, minor, patch, nonce, device_type);
        int   len          = snprintf(msg_send_buf, 512, fota_request, machine_id, CONFIG_IOT_MQTT_BRAND_ID);

        nrf5340_fota.complete          = false;
        nrf5340_fota.requested         = false;
        nrf5340_fota.response_received = false;
        nrf5340_fota.in_progress       = false;
        update_check_in_progress       = true;
        if (do_update) {
            update_device_type = device_type;
        } else {
            update_device_type = COMM_DEVICE_NONE;
        }
        if (device_type == COMM_DEVICE_NRF9160) {
            nrf9160_fota.complete          = false;
            nrf9160_fota.response_received = false;
            nrf9160_fota.in_progress       = false;
            nrf9160_fota.requested         = true;
            nrf9160_fota.last_state_time   = k_uptime_get();
        } else if (device_type == COMM_DEVICE_DA16200) {
            da16200_fota.complete          = false;
            da16200_fota.response_received = false;
            da16200_fota.in_progress       = false;
            da16200_fota.requested         = true;
            nrf9160_fota.last_state_time   = k_uptime_get();
        } else if (device_type == COMM_DEVICE_NRF5340) {
            nrf5340_fota.complete          = false;
            nrf5340_fota.response_received = false;
            nrf5340_fota.in_progress       = false;
            nrf5340_fota.requested         = true;
            nrf5340_fota.last_state_time   = k_uptime_get();
        }
        if ((ret = commMgr_queue_mqtt_message(msg_send_buf, len, MQTT_MESSAGE_TYPE_FOTA, 0, 1)) < 0) {
            LOG_ERR("Failed to send FOTA check message - %d", ret);
            update_check_in_progress = false;
            return -1;
        }
        return 0;
    } else {
        LOG_ERR("Failed to get current version for %s", comm_dev_str(device_type));
        return -1;
    }
}

static int fota_print_url(char *url, char *device_name)
{
    if (strlen(url) > 64) {
        LOG_DBG("FOTA for %s - URL: %.*s...%.*s", device_name, 32, url, 32, (url + (strlen(url) - 32)));
    } else {
        LOG_DBG("FOTA for %s - URL: %s", device_name, url);
    }
    return 0;
}

////////////////////////////////////////////////////
// handle_fota_message()
//  process incoming FOTA message
//
//  @param m the "M" message from the incoming mqtt message
//
//  @return 0 on success, -1 on failure
int handle_fota_message(cJSON *m)
{

    update_check_in_progress = false;

    cJSON *m_str = cJSON_GetObjectItem(m, "STR");
    if (m_str == NULL) {
        LOG_ERR("Failed to get STR from message");
        return -1;
    }
    if (strcmp(m_str->valuestring, "OK") != 0) {
        LOG_ERR("STR is not OK");
        return -1;
    }

    // We may use these 2 later, leaving them in the code for now
    // cJSON *m_did = cJSON_GetObjectItem(m, "DID");   // DeployId
    // cJSON *m_fv = cJSON_GetObjectItem(m, "FV");   // feature version string  "2.0" or "2.1"
    // LOG_DBG("FOTA FeatureVersion: %s", m_fv->valuestring);
    cJSON *m_al   = cJSON_GetObjectItem(m, "AL");
    cJSON *m_rid  = cJSON_GetObjectItem(m, "RID");    // RequestID
    cJSON *al_val = NULL;

    uint8_t results_count = cJSON_GetArraySize(m_al);
    if (nrf9160_fota.requested && !nrf9160_fota.response_received) {
        nrf9160_fota.response_received = true;
        nrf9160_fota.last_state_time   = k_uptime_get();
        if (results_count == 0) {
            nrf9160_fota.complete = true;
            nrf9160_fota.in_progress = false;
            commMgr_fota_end(COMM_DEVICE_NRF9160);    // Let commMgr know that we are done with fota
            LOG_ERR("No Updates for this device (nrf9160).");
            return 1;
        }
    }
    if (da16200_fota.requested && !da16200_fota.response_received) {
        da16200_fota.response_received = true;
        da16200_fota.last_state_time   = k_uptime_get();
        if (results_count == 0) {
            da16200_fota.complete = true;
            da16200_fota.in_progress = false;
            commMgr_fota_end(COMM_DEVICE_DA16200);    // Let commMgr know that we are done with fota
            LOG_ERR("No Updates for this device (da16200).");
            return 1;
        }
    }
    if (nrf5340_fota.requested && !nrf5340_fota.response_received) {
        nrf5340_fota.response_received = true;
        nrf5340_fota.last_state_time   = k_uptime_get();
        if (results_count == 0) {
            nrf5340_fota.complete = true;
            nrf5340_fota.in_progress = false;
            commMgr_fota_end(COMM_DEVICE_NRF5340);    // Let commMgr know that we are done with fota
            LOG_ERR("No Updates for this device (nrf5340).");
            fota_notify("NO_UPDATES");
            return 1;
        }
    }

    cJSON_ArrayForEach(al_val, m_al)
    {
        // char *al_string = cJSON_Print(al_val);
        // printf("FOTA AL: %s\n", al_string);

        log_panic();
        // LOG_DBG("FOTA AL: %s", al_string);
        cJSON *al_dt = cJSON_GetObjectItem(al_val, "DT");    // update type, 'firmware', 'bootloader' or 'modem'
        // LOG_DBG("FOTA update type: %s", al_dt->valuestring);
        cJSON *al_mtd     = cJSON_GetObjectItem(al_val, "MTD");    // get meta data string
        cJSON *mtd_major  = NULL;
        cJSON *mtd_minor  = NULL;
        cJSON *mtd_patch  = NULL;
        cJSON *mtd_alturl = NULL;
        // cJSON *mtd_hash = NULL;
        if (al_mtd != NULL) {
            mtd_major = cJSON_GetObjectItem(al_mtd, "major");
            mtd_minor = cJSON_GetObjectItem(al_mtd, "minor");
            mtd_patch = cJSON_GetObjectItem(al_mtd, "build");
            // mtd_hash = cJSON_GetObjectItem(al_mtd, "hash");
            mtd_alturl = cJSON_GetObjectItem(al_mtd, "alturl");

            // if (mtd_hash != NULL) {
            //     LOG_DBG("FOTA MTD hash: %s", mtd_hash->valuestring);
            // }
        } else {
            LOG_ERR("Failed to get MTD from message");
            return -1;
        }
        // The following 3 may be used later, leaving them in the code for now

        // cJSON *al_sha = cJSON_GetObjectItem(al_val, "SHA");
        // if (al_sha != NULL) LOG_DBG("SHA: %s", al_sha->valuestring);
        // cJSON *al_sig = cJSON_GetObjectItem(al_val, "SIG");
        // if (al_sig != NULL) LOG_DBG("SIG: %s", al_sig->valuestring);
        // cJSON *al_msig = cJSON_GetObjectItem(al_val, "MSIG");  // get the mfg signature
        // of the update if (al_msig != NULL)  LOG_DBG("MSIG: %s", al_msig->valuestring);
        cJSON *al_ch = cJSON_GetObjectItem(al_val, "CH");    // get the array of URLs to download from
        if (al_ch == NULL) {
            LOG_ERR("Failed to get CH from message");
            return -1;
        }

        cJSON *al_url          = NULL;
        char  *final_url       = NULL;
        char  *machine_id      = uicr_serial_number_get();
        char  *validiation_msg = NULL;
        cJSON_ArrayForEach(al_url, al_ch)
        {
            if (cJSON_IsString(al_url)) {
                if (mtd_alturl != NULL) {
                    LOG_WRN("FOTA Using Alternate URL: %s", mtd_alturl->valuestring);
                    final_url = mtd_alturl->valuestring;
                } else {
                    final_url = al_url->valuestring;
                }
                // match against the filename and type to determine target
                uint8_t ver_array[3] = { 0 };
                ver_array[0]         = atoi(mtd_major->valuestring);
                ver_array[1]         = atoi(mtd_minor->valuestring);
                ver_array[2]         = atoi(mtd_patch->valuestring);
                if (strstr(final_url, FILENAME_MATCH_FOR_9160) && strstr(al_dt->valuestring, FILENAME_MATCH_FOR_9160)) {
                    // download and install for nrf9160
                    fota_print_url(final_url, "nrf9160");
                    if ((update_device_type & COMM_DEVICE_NRF9160)) {
                        if (!is_9160_lte_connected() && !modem_is_powered_on()) {
                            LOG_DBG("FOTA for nrf9160 - LTE not connected, powering on modem");
                            modem_power_on();
                            uint8_t loop_cnt = 0;
                            while (is_9160_lte_connected() == false) {
                                if (loop_cnt > 10) {
                                    LOG_ERR("FOTA for nrf9160 - LTE not connected, skipping");
                                    return -1;
                                }
                                k_sleep(K_SECONDS(2));
                                loop_cnt++;
                                LOG_DBG("FOTA for nrf9160 - LTE not connected, waiting");
                            }
                        }
                        memset(nrf9160_fota.request_id, 0, 64);    // clear the RID for this device
                        memcpy(nrf9160_fota.request_id, m_rid->valuestring, strlen(m_rid->valuestring));
                        validiation_msg = json_fota_feedback_msg(
                            machine_id, 0, al_dt->valuestring, m_rid->valuestring, commMgr_get_unix_time(), "VALIDATION");
                        if (validiation_msg) {
                            commMgr_queue_mqtt_message(
                                validiation_msg, strlen(validiation_msg), MQTT_MESSAGE_TYPE_FOTA_LIFE, 0, 30);
                        }
                        k_sleep(K_MSEC(50));
                        int ret = fota_start_9160_process(ver_array[0],ver_array[1],ver_array[2]);
                        if (ret == 0) {
                            ret = modem_fota_from_https(final_url, strlen(final_url));
                            if (ret != 0) {
                                LOG_ERR("'%s'(%d) from modem_fota_from_https", wstrerr(-ret), ret);
                            }
                        } else {
                            LOG_ERR("'%s'(%d) fota_start_9160_process", wstrerr(-ret), ret);
                        }
                    } else {
                        nrf9160_fota.complete = true;
                        nrf5340_fota.in_progress = false;
                        commMgr_fota_end(COMM_DEVICE_NRF9160);    // Let commMgr know
                                                                  // that we are done
                                                                  // with fota
                    }
                } else if (strstr(final_url, FILENAME_MATCH_FOR_da16200) && strstr(al_dt->valuestring, FILENAME_MATCH_FOR_da16200)) {
                    // download and install for da16200
                    fota_print_url(final_url, "da16200");
                    if (update_device_type & COMM_DEVICE_DA16200) {
                        memset(da16200_fota.request_id, 0, 64);    // clear the RID for this device
                        memcpy(da16200_fota.request_id, m_rid->valuestring, strlen(m_rid->valuestring));
                        validiation_msg = json_fota_feedback_msg(
                            machine_id, 0, al_dt->valuestring, m_rid->valuestring, commMgr_get_unix_time(), "VALIDATION");
                        if (validiation_msg) {
                            commMgr_queue_mqtt_message(
                                validiation_msg, strlen(validiation_msg), MQTT_MESSAGE_TYPE_FOTA_LIFE, 0, 30);
                        }
                        k_sleep(K_MSEC(50));
                        if (commMgr_fota_start(COMM_DEVICE_DA16200) == 0) {
                            da16200_fota.last_state_time   = k_uptime_get();
                            da16200_fota.target_version[0] = ver_array[0];
                            da16200_fota.target_version[1] = ver_array[1];
                            da16200_fota.target_version[2] = ver_array[2];
                            da16200_fota.in_progress       = true;
                            int ret                        = net_start_ota(final_url, da16200_fota.target_version);
                            if (ret != 0) {
                                LOG_ERR("Failed to start FOTA for da16200 - %d", ret);
                                fota_status_update(2, FOTA_DOWNLOAD_ERROR_CAUSE_INTERNAL, COMM_DEVICE_DA16200);
                            }
                        } else {
                            LOG_ERR("Couldn't start fota for da16200 because DA is not ready");
                        }
                    } else {
                        da16200_fota.complete = true;
                        da16200_fota.in_progress = false;
                        LOG_WRN("Couldn't start fota for da16200 because device type is not DA");
                        commMgr_fota_end(COMM_DEVICE_DA16200);    // Let commMgr know
                                                                  // that we are done
                                                                  // with fota
                    }
                } else if (strstr(final_url, FILENAME_MATCH_FOR_5340) && strstr(al_dt->valuestring, FILENAME_MATCH_FOR_5340)) {
                    // download and install for nrf5340
                    fota_print_url(final_url, "nrf5340");
                    if (update_device_type & COMM_DEVICE_NRF5340) {
                        memset(nrf5340_fota.request_id, 0, 64);    // clear the RID for this device
                        memcpy(nrf5340_fota.request_id, m_rid->valuestring, strlen(m_rid->valuestring));
                        validiation_msg = json_fota_feedback_msg(
                            machine_id, 0, al_dt->valuestring, m_rid->valuestring, commMgr_get_unix_time(), "VALIDATION");
                        if (validiation_msg) {
                            commMgr_queue_mqtt_message(
                                validiation_msg, strlen(validiation_msg), MQTT_MESSAGE_TYPE_FOTA_LIFE, 0, 30);
                        }
                        k_sleep(K_MSEC(50));
                        char temp[100] = { 0 };
                        snprintf(temp, sizeof(temp), "5340_%s.%s.%s", mtd_major->valuestring, mtd_minor->valuestring, mtd_patch->valuestring);
                        fota_notify(temp);

                        if (commMgr_fota_start(COMM_DEVICE_NRF5340) == 0) {
                            nrf5340_fota.last_state_time   = k_uptime_get();
                            nrf5340_fota.target_version[0] = ver_array[0];
                            nrf5340_fota.target_version[1] = ver_array[1];
                            nrf5340_fota.target_version[2] = ver_array[2];
                            nrf5340_fota.in_progress       = true;
                            int ret = wifi_http_get(final_url, "nrf5340_fota.bin", true, K_SECONDS(3));
                            if (ret != 0) {
                                LOG_ERR("Failed to start FOTA for nrf5340 - '%s'(%d)", wstrerr(-ret), ret);
                                fota_status_update(2, FOTA_DOWNLOAD_ERROR_CAUSE_INTERNAL, COMM_DEVICE_NRF5340);
                            }
                            fota_start_da_http_monitoring();
                        } else {
                            LOG_ERR("Couldn't start fota for nrf5340 because DA is not ready");
                        }
                    } else {
                        nrf5340_fota.complete = true;
                        nrf5340_fota.in_progress = false;
                        commMgr_fota_end(COMM_DEVICE_NRF5340);    // Let commMgr know
                                                                  // that we are done
                                                                  // with fota
                    }
                } else {
                    fota_print_url(final_url, "UNKNOWN");
                }
                if ((mtd_major != NULL) && (mtd_minor != NULL)
                    && (mtd_patch != NULL)) {    // printing this down here so the output makes more sense.
                    LOG_INF("FOTA file version: %s.%s.%s", mtd_major->valuestring, mtd_minor->valuestring, mtd_patch->valuestring);
                }
            }
        }
        // if you got here and nothing is in_progress or complete, then
        // there was a list of URLs but I guess none of them matched the device/version
    }
    return 0;
}

/////////////////////////////////////////////////
// fota_status_update()
// External api
int fota_status_update(int state, int percentage, comm_device_type_t device_type)
{
    char *validiation_msg = NULL;
    char *machine_id      = uicr_serial_number_get();
    if (state == 1) {
        // FOTA DOWNLOADING
        LOG_WRN("%s - FOTA DL percentage: %d%%", comm_dev_str(device_type), percentage);
        if (percentage == 0) {
            if ((device_type == COMM_DEVICE_NRF9160) && nrf9160_fota.in_progress) {
                validiation_msg = json_fota_feedback_msg(
                    machine_id, 0, FILENAME_MATCH_FOR_9160, nrf9160_fota.request_id, commMgr_get_unix_time(), "DOWNLOAD");
                if (validiation_msg) {
                    commMgr_queue_mqtt_message(
                        validiation_msg, strlen(validiation_msg), MQTT_MESSAGE_TYPE_FOTA_LIFE, 0, 30);
                }
                LOG_WRN("%s - FOTA download started.", comm_dev_str(device_type));
                nrf9160_fota.last_state_time = k_uptime_get();
            } else if ((device_type == COMM_DEVICE_DA16200) && da16200_fota.in_progress) {
                validiation_msg = json_fota_feedback_msg(
                    machine_id, 0, FILENAME_MATCH_FOR_da16200, da16200_fota.request_id, commMgr_get_unix_time(), "DOWNLOAD");
                if (validiation_msg) {
                    commMgr_queue_mqtt_message(
                        validiation_msg, strlen(validiation_msg), MQTT_MESSAGE_TYPE_FOTA_LIFE, 0, 30);
                }
                LOG_WRN("%s - FOTA download started.", comm_dev_str(device_type));
                da16200_fota.last_state_time = k_uptime_get();
            } else if ((device_type == COMM_DEVICE_NRF5340) && nrf5340_fota.in_progress) {
                validiation_msg = json_fota_feedback_msg(
                    machine_id, 0, FILENAME_MATCH_FOR_5340, nrf5340_fota.request_id, commMgr_get_unix_time(), "DOWNLOAD");
                if (validiation_msg) {
                    commMgr_queue_mqtt_message(
                        validiation_msg, strlen(validiation_msg), MQTT_MESSAGE_TYPE_FOTA_LIFE, 0, 30);
                }
                LOG_WRN("%s - FOTA download started.", comm_dev_str(device_type));
                nrf5340_fota.last_state_time = k_uptime_get();
            }
        } else if (percentage == 100) {
            // TODO:
            //    get current version before this step, as the device is prob now
            //    rebooting kick off a timer that will get the version and compare to
            //    see if it was completed, if so send state 3 special case for 5340,
            //    needs to set a flag and store previous version
            if ((device_type == COMM_DEVICE_NRF9160) && nrf9160_fota.in_progress) {
                validiation_msg = json_fota_feedback_msg(
                    machine_id,
                    0,
                    FILENAME_MATCH_FOR_9160,
                    nrf9160_fota.request_id,
                    commMgr_get_unix_time(),
                    "ASSET_VERIFICATION");
                if (validiation_msg) {
                    commMgr_queue_mqtt_message(
                        validiation_msg, strlen(validiation_msg), MQTT_MESSAGE_TYPE_FOTA_LIFE, 0, 30);
                }
                LOG_WRN(
                    "%s - FOTA download complete, rebooting.  Takes a few moments for the first reboot after an update.",
                    comm_dev_str(device_type));
                k_timer_start(&fota_version_check_timer, K_SECONDS(45), K_NO_WAIT);
                nrf9160_fota.last_state_time = k_uptime_get();
            } else if ((device_type == COMM_DEVICE_DA16200) && da16200_fota.in_progress) {
                validiation_msg = json_fota_feedback_msg(
                    machine_id,
                    0,
                    FILENAME_MATCH_FOR_da16200,
                    da16200_fota.request_id,
                    commMgr_get_unix_time(),
                    "ASSET_VERIFICATION");
                if (validiation_msg) {
                    commMgr_queue_mqtt_message(
                        validiation_msg, strlen(validiation_msg), MQTT_MESSAGE_TYPE_FOTA_LIFE, 0, 30);
                }
                LOG_WRN(
                    "%s - FOTA download complete, rebooting.  Takes a few moments for the first reboot after an update.",
                    comm_dev_str(device_type));
                k_timer_start(&fota_version_check_timer, K_SECONDS(30), K_NO_WAIT);
                da16200_fota.last_state_time = k_uptime_get();
            } else if ((device_type == COMM_DEVICE_NRF5340) && nrf5340_fota.in_progress) {
                fota_stop_da_http_monitoring();
                validiation_msg = json_fota_feedback_msg(
                    machine_id,
                    0,
                    FILENAME_MATCH_FOR_5340,
                    nrf5340_fota.request_id,
                    commMgr_get_unix_time(),
                    "ASSET_VERIFICATION");
                if (validiation_msg) {
                    commMgr_queue_mqtt_message(
                        validiation_msg, strlen(validiation_msg), MQTT_MESSAGE_TYPE_FOTA_LIFE, 0, 30);
                }
                LOG_WRN(
                    "%s - FOTA download complete, rebooting.  Takes a few moments for the first reboot after an update.",
                    comm_dev_str(device_type));
                nrf5340_fota.last_state_time = k_uptime_get();
            }
        } else {
            if ((device_type == COMM_DEVICE_NRF9160) && nrf9160_fota.in_progress) {
                nrf9160_fota.last_state_time = k_uptime_get();
            } else if ((device_type == COMM_DEVICE_DA16200) && da16200_fota.in_progress) {
                da16200_fota.last_state_time = k_uptime_get();
            } else if ((device_type == COMM_DEVICE_NRF5340) && nrf5340_fota.in_progress) {
                nrf5340_fota.last_state_time = k_uptime_get();
            }
        }
    }
    if (state == 3) {
        // FOTA COMPLETE
        int ret;
        if ((device_type == COMM_DEVICE_NRF9160) && nrf9160_fota.in_progress) {
            validiation_msg = json_fota_feedback_msg(
                machine_id, 0, FILENAME_MATCH_FOR_9160, nrf9160_fota.request_id, commMgr_get_unix_time(), "DONE");
            if (validiation_msg) {
                ret = commMgr_queue_mqtt_message(
                    validiation_msg, strlen(validiation_msg), MQTT_MESSAGE_TYPE_FOTA_LIFE, 0, 1);
                if (ret < 0) {
                    LOG_ERR("Failed to send FOTA feedback message - %d", ret);
                    return -1;
                }
            }
            LOG_WRN("%s - FOTA complete.", comm_dev_str(device_type));
            nrf9160_fota.complete = true;
            nrf9160_fota.in_progress = false;
            commMgr_fota_end(COMM_DEVICE_NRF9160);    // Let commMgr know that we are done with fota
        } else if ((device_type == COMM_DEVICE_DA16200) && da16200_fota.in_progress) {
            validiation_msg = json_fota_feedback_msg(
                machine_id, 0, FILENAME_MATCH_FOR_da16200, da16200_fota.request_id, commMgr_get_unix_time(), "DONE");
            if (validiation_msg) {
                ret = commMgr_queue_mqtt_message(
                    validiation_msg, strlen(validiation_msg), MQTT_MESSAGE_TYPE_FOTA_LIFE, 0, 1);
                if (ret < 0) {
                    LOG_ERR("Failed to send FOTA feedback message - %d", ret);
                    return -1;
                }
            }
            LOG_WRN("%s - FOTA complete.", comm_dev_str(device_type));
            da16200_fota.complete = true;
            da16200_fota.in_progress = false;
            commMgr_fota_end(COMM_DEVICE_DA16200);    // Let commMgr know that we are done with fota
        } else if ((device_type == COMM_DEVICE_NRF5340) && nrf5340_fota.in_progress) {
            fota_stop_da_http_monitoring();
            validiation_msg = json_fota_feedback_msg(
                machine_id, 0, FILENAME_MATCH_FOR_5340, nrf5340_fota.request_id, commMgr_get_unix_time(), "DONE");
            if (validiation_msg) {
                ret = commMgr_queue_mqtt_message(
                    validiation_msg, strlen(validiation_msg), MQTT_MESSAGE_TYPE_FOTA_LIFE, 0, 1);
                if (ret < 0) {
                    LOG_ERR("Failed to send FOTA feedback message - %d", ret);
                    return -1;
                }
            }
            LOG_WRN("%s - FOTA complete.", comm_dev_str(device_type));
            nrf5340_fota.complete = true;
            nrf5340_fota.in_progress = false;
            commMgr_fota_end(COMM_DEVICE_NRF5340);    // Let commMgr know that we are done with fota
        }
    }
    if (state == 99) {
        // FOTA ABORTED

        if (device_type == COMM_DEVICE_NRF9160 && nrf9160_fota.in_progress) {
            nrf9160_fota.complete = true;
            nrf9160_fota.in_progress = false;
            commMgr_fota_end(COMM_DEVICE_NRF9160);    // Let commMgr know that we are done with fota
            LOG_ERR("%s - FOTA cancelled", comm_dev_str(device_type));
            validiation_msg = json_fota_feedback_msg(
                machine_id, -500, FILENAME_MATCH_FOR_9160, nrf9160_fota.request_id, commMgr_get_unix_time(), "EXECUTE");
            if (validiation_msg) {
                commMgr_queue_mqtt_message(validiation_msg, strlen(validiation_msg), MQTT_MESSAGE_TYPE_FOTA_LIFE, 0, 30);
            }
        } else if (device_type == COMM_DEVICE_DA16200 && da16200_fota.in_progress) {
            da16200_fota.complete = true;
            da16200_fota.in_progress = false;
            commMgr_fota_end(COMM_DEVICE_DA16200);    // Let commMgr know that we are done with fota
            LOG_ERR("%s - FOTA cancelled", comm_dev_str(device_type));
            validiation_msg = json_fota_feedback_msg(
                machine_id, -500, FILENAME_MATCH_FOR_da16200, da16200_fota.request_id, commMgr_get_unix_time(), "EXECUTE");
            if (validiation_msg) {
                commMgr_queue_mqtt_message(validiation_msg, strlen(validiation_msg), MQTT_MESSAGE_TYPE_FOTA_LIFE, 0, 30);
            }
        } else if (device_type == COMM_DEVICE_NRF5340 && nrf5340_fota.in_progress) {
            nrf5340_fota.complete = true;
            nrf5340_fota.in_progress = false;
            fota_stop_da_http_monitoring();
            commMgr_fota_end(COMM_DEVICE_NRF5340);    // Let commMgr know that we are done with fota
            LOG_ERR("%s - FOTA cancelled", comm_dev_str(device_type));
            validiation_msg = json_fota_feedback_msg(
                machine_id, -500, FILENAME_MATCH_FOR_5340, nrf5340_fota.request_id, commMgr_get_unix_time(), "EXECUTE");
            if (validiation_msg) {
                commMgr_queue_mqtt_message(validiation_msg, strlen(validiation_msg), MQTT_MESSAGE_TYPE_FOTA_LIFE, 0, 30);
            }
        }
    }
    if (state == 2) {
        // FOTA ERROR

        if (device_type == COMM_DEVICE_NRF9160 && nrf9160_fota.in_progress) {
            validiation_msg = json_fota_feedback_msg(
                machine_id,
                nestle_fota_error_codes[percentage],
                FILENAME_MATCH_FOR_9160,
                nrf9160_fota.request_id,
                commMgr_get_unix_time(),
                "EXECUTE");
            if (validiation_msg) {
                commMgr_queue_mqtt_message(validiation_msg, strlen(validiation_msg), MQTT_MESSAGE_TYPE_FOTA_LIFE, 0, 30);
            }
            LOG_ERR("%s - FOTA error: %s", comm_dev_str(device_type), nrf_fota_error_strings[percentage]);
            nrf9160_fota.complete = true;
            nrf9160_fota.in_progress = false;
            commMgr_fota_end(COMM_DEVICE_NRF9160);    // Let commMgr know that we are done with fota
        } else if (device_type == COMM_DEVICE_DA16200 && da16200_fota.in_progress) {
            validiation_msg = json_fota_feedback_msg(
                machine_id,
                nestle_fota_error_codes[percentage],
                FILENAME_MATCH_FOR_da16200,
                da16200_fota.request_id,
                commMgr_get_unix_time(),
                "EXECUTE");
            if (validiation_msg) {
                commMgr_queue_mqtt_message(validiation_msg, strlen(validiation_msg), MQTT_MESSAGE_TYPE_FOTA_LIFE, 0, 30);
            }
            LOG_ERR("%s - FOTA error: %s", comm_dev_str(device_type), nrf_fota_error_strings[percentage]);
            da16200_fota.complete = true;
            da16200_fota.in_progress = false;
            commMgr_fota_end(COMM_DEVICE_DA16200);    // Let commMgr know that we are done with fota
        } else if (device_type == COMM_DEVICE_NRF5340 && nrf5340_fota.in_progress) {
            validiation_msg = json_fota_feedback_msg(
                machine_id,
                nestle_fota_error_codes[percentage],
                FILENAME_MATCH_FOR_5340,
                nrf5340_fota.request_id,
                commMgr_get_unix_time(),
                "EXECUTE");
            if (validiation_msg) {
                commMgr_queue_mqtt_message(validiation_msg, strlen(validiation_msg), MQTT_MESSAGE_TYPE_FOTA_LIFE, 0, 30);
            }
            LOG_ERR("%s - FOTA error: %s", comm_dev_str(device_type), nrf_fota_error_strings[percentage]);
            fota_notify("5340_FOTA_ERR");
            nrf5340_fota.complete = true;
            nrf5340_fota.in_progress = false;
            fota_stop_da_http_monitoring();
            commMgr_fota_end(COMM_DEVICE_NRF5340);    // Let commMgr know that we are done with fota
        }
    }
    return 0;
}

/////////////////////////////////////////////////
// cancel_fota_download()
// External api
int cancel_fota_download(comm_device_type_t device_type)
{
    if (device_type == COMM_DEVICE_NRF9160) {
        if (nrf9160_fota.in_progress) {
            uint8_t cmd = COMMAND_SET_FOTA_CANCEL;
            int     ret = modem_send_command(MESSAGE_TYPE_COMMAND, &cmd, 1, false);
            if (ret < 0) {
                LOG_ERR("'%s'(%d) sending cancel FOTA command", wstrerr(-ret), ret);
            }
            fota_status_update(
                99,
                0,
                COMM_DEVICE_NRF9160);    // 9160 will send confirmation it
                                         // was cancelled, that will set off
                                         // appropriate flags/handlers
        }
    } else if (device_type == COMM_DEVICE_DA16200) {
        if (da16200_fota.in_progress) {
            // TODO: need to cancel the DA download
            fota_status_update(99, 0, COMM_DEVICE_DA16200);    // sets state to cancelled
        }
    } else if (device_type == COMM_DEVICE_NRF5340) {
        if (nrf5340_fota.in_progress) {
            // TODO: need to cancel the 5340 download
            fota_status_update(99, 0, COMM_DEVICE_NRF5340);    // sets state to cancelled
        }
    }
    return 0;
}

/////////////////////////////////////////////////
// fota_cancel_fota_updates()
// External api
int fota_cancel_fota_update()
{
    if (fota_in_progress == false) {
        return -1;
    }

    nrf9160_fota.complete          = false;
    nrf9160_fota.requested         = false;
    nrf9160_fota.response_received = false;
    nrf9160_fota.in_progress       = false;
    da16200_fota.complete          = false;
    da16200_fota.requested         = false;
    da16200_fota.response_received = false;
    da16200_fota.in_progress       = false;
    nrf5340_fota.complete          = false;
    nrf5340_fota.requested         = false;
    nrf5340_fota.response_received = false;
    nrf5340_fota.in_progress       = false;
    fota_in_progress               = false;
    commMgr_fota_end(COMM_DEVICE_NRF5340);    // Let commMgr know that we are done with fota
    commMgr_fota_end(COMM_DEVICE_NRF9160);    // Let commMgr know that we are done with fota
    commMgr_fota_end(COMM_DEVICE_DA16200);    // Let commMgr know that we are done with fota
    commMgr_enable_S_work(true);
    return 0;
}

/////////////////////////////////////////////////
int fota_start_9160_process(int major, int minor, int patch)
{
    if (nrf9160_fota.in_progress) {
        return -1;
    }
    int ret = commMgr_fota_start(COMM_DEVICE_NRF9160);
    if (ret < 0) {
        LOG_ERR("Failed to start FOTA for nrf9160 - %d", ret);
        return ret;
    }
    nrf9160_fota.in_progress       = true;
    nrf9160_fota.complete          = false;
    nrf9160_fota.last_state_time   = k_uptime_get();
    nrf9160_fota.target_version[0] = major;
    nrf9160_fota.target_version[1] = minor;
    nrf9160_fota.target_version[2] = patch;
    return 0;
}