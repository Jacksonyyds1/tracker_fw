/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#pragma once
#include "commMgr.h"
#include "modem_interface_types.h"
#include "d1_json.h"
#include "net_mgr.h"

// copied from net/fota_download.h which is NOT trivial to #include, so I didn't
enum fota_download_error_cause
{
    /** No error, used when event ID is not FOTA_DOWNLOAD_EVT_ERROR. */
    FOTA_DOWNLOAD_ERROR_CAUSE_NO_ERROR,
    /** Downloading the update failed. The download may be retried. */
    FOTA_DOWNLOAD_ERROR_CAUSE_DOWNLOAD_FAILED,
    /** The update is invalid and was rejected. Retry will not help. */
    FOTA_DOWNLOAD_ERROR_CAUSE_INVALID_UPDATE,
    /** Actual firmware type does not match expected. Retry will not help. */
    FOTA_DOWNLOAD_ERROR_CAUSE_TYPE_MISMATCH,
    /** Generic error on device side. */
    FOTA_DOWNLOAD_ERROR_CAUSE_INTERNAL,
    FOTA_DOWNLOAD_ERROR_MAX_ERROR,
};

int  check_for_updates(bool do_update, comm_device_type_t device_type);
int  handle_fota_message(cJSON *m);
int  fota_status_update(int state, int percentage, comm_device_type_t device_type);
int  cancel_fota_download(comm_device_type_t device_type);
int  fota_update_all_devices();
int  fota_handle_da_event(da_event_t status);
void fota_check_for_fota_in_progress();
int  fota_set_in_progress_timer(int dur, bool save);

/////////////////////////////////////////////////
// fota_cancel_fota_updates()
// Stop any ongoing FOTA updates
int fota_cancel_fota_update();
int fota_start_9160_process(int major, int minor, int patch);
