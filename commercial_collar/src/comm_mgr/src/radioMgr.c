/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#include "radioMgr.h"
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/zbus/zbus.h>
#include "wifi.h"
#include "wifi_at.h"
#include "net_mgr.h"
#include "modem.h"
#include "d1_zbus.h"
#include "d1_json.h"
#include "modem_interface_types.h"
#include "wi.h"

LOG_MODULE_REGISTER(radio_mgr, CONFIG_RADIO_MGR_LOG_LEVEL);

/* Register subscriber */
void radio_mgr_listener(const struct zbus_channel *chan);
ZBUS_LISTENER_DEFINE(radio_mgr_sub, radio_mgr_listener);

bool g_radio_mgmt           = true;
bool g_use_sleep            = (CONFIG_RADIO_MGR_SLEEP_ENABLED == 1);
bool g_do_one_step          = false;    // used by shell command to execute one step of the radio switch
bool g_bt_connected         = false;
bool g_usb_connected        = false;
bool g_usb_bt_prepped_radio = false;    // If we ever prepped the radio based on USB/BT state
bool g_is_connecting_to_ap  = false;
bool g_use_wifi             = true;

bool                           g_switching_radios       = false;
uint64_t                       g_switching_radios_start = 0;
static comm_device_type_t      g_active_radio           = COMM_DEVICE_NONE;
extern volatile modem_status_t modem_status_shadow;

struct k_work_q radioMgr_work_q;
K_THREAD_STACK_DEFINE(radioMgr_stack_area, 4096);
K_MUTEX_DEFINE(RM_mutex);

void switch_radios_work_handler(struct k_work *item);

typedef enum
{
    SRS_IDLE,
    SRS_STATE_TO_BE_KNOWN,
    SRS_WAKING_DA,
    SRS_SLEEPING_DA,
    SRS_TURNING_ON_BROKER,
    SRS_TURNING_OFF_BROKER,
    SRS_TURNING_ON_BROKER_BOOT,
    SRS_TURNING_OFF_BROKER_BOOT,
    SRS_WAIT_FOR_AP,
    SRS_DISABLING_LTE_MQTT,
    SRS_ENABLING_LTE_MQTT,
    SRS_WAIT_FOR_LTE_READY,
    SRS_WAIT_FOR_LTE_CONNECT,
    SRS_WAIT_FOR_LTE_DISCONNECT,
    SRS_WAIT_FOR_BROKER,
    SRS_STOP_AP_USE
} switch_radios_state_t;

typedef enum
{
    OS_STARTED,
    OS_STILL_GOING,
    OS_FAILED
} op_state_t;

typedef struct switch_radios_info_struct
{
    struct k_work         SR_work;
    switch_radios_state_t currOp;
    uint64_t              currOpStart;
    comm_device_type_t    target_radio;
    int                   op_failures;
    int                   max_retrys;
    int                   timeout;
    bool                  radio_enabled;
    bool                  radio_disabled;
} switch_radios_info_t;

switch_radios_info_t switch_radios_info = { .SR_work        = { .handler = switch_radios_work_handler },
                                            .currOp         = SRS_IDLE,
                                            .currOpStart    = 0,
                                            .target_radio   = COMM_DEVICE_NRF9160,
                                            .op_failures    = 0,
                                            .radio_enabled  = false,
                                            .radio_disabled = false };

void queue_switching_handler(struct k_timer *dummy);
K_TIMER_DEFINE(continue_switching_timer, queue_switching_handler, NULL);

typedef struct connect_to_ap_struct
{
    wifi_saved_ap_t connInfo;
    int             conn_idx;
    bool            disconnectFirst;
} connect_to_ap_t;

connect_to_ap_t g_ap_to_attempt;

typedef struct zbus_work_info
{
    workref_t                 *zbus_work;
    const struct zbus_channel *chan;
    union
    {
        da_event_t            evt;
        modem_status_update_t modem_status;
        bool                  connected;
    } msg;
} zbus_work_info_t;

static void   connecting_to_ap_work_handler(struct k_work *item);
struct k_work connecting_to_ap_work;
void          queue_connect_to_ap_work()
{
    k_work_submit_to_queue(&radioMgr_work_q, &connecting_to_ap_work);
}

static void   usb_bt_connection_work_handler(struct k_work *item);
struct k_work usb_bt_connection_work;

// Awake for the RadioMgr means awake and will stay awake
static inline bool is_da_awake()
{
    return (da_state.is_sleeping == DA_STATE_KNOWN_FALSE);
}

static inline bool is_da_in_right_sleep_mode()
{
    if (g_use_sleep == true) {
        if (da_state.ap_connected == DA_STATE_KNOWN_TRUE) {
            // We should dpm sleep if connected to an AP
            return (da_state.dpm_mode == DA_STATE_KNOWN_TRUE && da_state.is_sleeping == DA_STATE_KNOWN_TRUE);

        } else {
            // If not connected to an AP, we should SLEEP3
            return (da_state.dpm_mode == DA_STATE_KNOWN_TRUE && da_state.is_sleeping == DA_STATE_KNOWN_TRUE);
        }
    } else {
        // If we don't sleep then dpm and sleep should be off
        return (da_state.dpm_mode == DA_STATE_KNOWN_FALSE && da_state.is_sleeping == DA_STATE_KNOWN_FALSE);
    }
}

static inline bool is_lte_connected(uint32_t *status_ptr)
{
    uint32_t status = modem_status_shadow.status_flags;
    if (status_ptr != NULL) {
        status = *status_ptr;
    }
    return status_getBit(status, STATUS_LTE_CONNECTED);
}

static inline bool has_lte_been_connected(uint32_t *status_ptr)
{
    uint32_t status = modem_status_shadow.status_flags;
    if (status_ptr != NULL) {
        status = *status_ptr;
    }
    return status_getBit(status, STATUS_LTE_WORKING);
}

static inline bool is_lte_mqtt_enabled()
{
    return (status_getBit(modem_status_shadow.status_flags, STATUS_MQTT_ENABLED));
}

static inline bool is_lte_mqtt_connected()
{
    return (status_getBit(modem_status_shadow.status_flags, STATUS_MQTT_CONNECTED));
}

////////////////////////////////////////////////////
// queue_switching_handler()
//  Called to check if we can continue switching radios
void queue_switching_handler(struct k_timer *dummy)
{
    k_work_submit_to_queue(&radioMgr_work_q, &switch_radios_info.SR_work);
}

static int lte_set_mqtt_enable(bool on, k_timeout_t timeout)
{
    k_timepoint_t timepoint = sys_timepoint_calc(timeout);
    int           ret;

    if (on == is_lte_mqtt_enabled()) {
        return 0;
    }
    LOG_DBG("Changing LTE MQTT state to %d", on);
    if (on) {
        ret = modem_start_mqtt();
    } else {
        ret = modem_stop_mqtt();
    }
    if (ret != 0) {
        LOG_ERR("Got error %d when set LTE MQTT state to %d", ret, on);
        return -1;
    }
    while (on != is_lte_mqtt_enabled()) {
        k_sleep(K_MSEC(10));
        timeout = sys_timepoint_timeout(timepoint);
        if (K_TIMEOUT_EQ(timeout, K_NO_WAIT) == true) {
            return -ETIMEDOUT;
        }
    }
    return 0;
}

////////////////////////////////////////////////////
// rm_enable()
//  Enable or disable the radio manager state
// machine and reconnect task.
//
//  @param enable true to enable, false to disable
void rm_enable(bool enable)
{
    g_radio_mgmt = enable;
}

////////////////////////////////////////////////////
// rm_is_enabled()
//  Return if the radio manager is enabled
//
bool rm_is_enable()
{
    return g_radio_mgmt;
}

////////////////////////////////////////////////////
// rm_wifi_enable()
//  Enable or disable the radio manager from using
// the DA.  commMgr also checks this value for ssid
// scans
//
//  @param enable true to enable, false to disable
void rm_wifi_enable(bool use_wifi)
{
    if (use_wifi == false && g_use_wifi == true) {
        g_use_wifi = false;
        if (g_active_radio == COMM_DEVICE_DA16200) {
            LOG_WRN("Wifi is active, switching to LTE");
            rm_switch_to(COMM_DEVICE_NRF9160, false, false);
        }
    }

    if (use_wifi == true && g_use_wifi == false) {
        g_use_wifi = true;
    }
}


////////////////////////////////////////////////////
// rm_is_wifi_enabled()
//  Return if the radio manager is enabled to use
// the DA
//
bool rm_is_wifi_enabled()
{
    return g_use_wifi;
}


////////////////////////////////////////////////////
// rm_use_sleep()
//  Enable or disable the radio manager to sleep the
// radios when not in use
//
//  @param enable true to enable, false to disable
void rm_use_sleep(bool enable)
{
    g_use_sleep = enable;
}

////////////////////////////////////////////////////
// rm_uses_sleep()
//  Return if the radio manager is using sleep
//
bool rm_uses_sleep()
{
    return g_use_sleep;
}

////////////////////////////////////////////////////
// rm_connect_to_AP_by_index()
//  Connect to the AP with the given inex into the
//  known SSID list
//
//  @param idx - THe index into the known SSID list
//               to connect to
//
int rm_connect_to_AP_by_index(int idx)
{
    if (g_is_connecting_to_ap) {
        LOG_ERR("Already connecting to an AP");
        return -EAGAIN;
    }
    if (!g_use_wifi) {
        LOG_ERR("Wifi is disabled, can't connect to AP");
        return -ENOTSUP;
    }
    if (idx < 0 || idx >= MAX_SAVED_SSIDS) {
        LOG_ERR("Invalid index %d", idx);
        return -EINVAL;
    }
    // Check to see if they are asking for the currently connected AP with the exact same info
    // as what is in the saved_ssid list.  This should mean that our current connecting was made
    // with those.  If it is different then they are asking us to try a new password
    char *target_ssid = wifi_get_saved_ssid_by_index(idx);
    if (target_ssid == NULL || target_ssid[0] == 0) {
        LOG_ERR("No saved ssid at index %d", idx);
        return -EINVAL;
    }
    g_ap_to_attempt.disconnectFirst = false;
    if (da_state.ap_connected == DA_STATE_KNOWN_TRUE) {
        if (strncmp(da_state.ap_name, target_ssid, 32) == 0) {
            // We are connected to that AP, check to see if we are using it
            if (rm_get_active_mqtt_radio() != COMM_DEVICE_DA16200) {
                // Not using it, so switch to it
                rm_switch_to(COMM_DEVICE_DA16200, false, false);
                return 0;
            } else {
                LOG_DBG("Already connected to and switched to AP at idx %d", idx);
                return -EEXIST;
            }
        } else {
            // We are connected to a different AP, so we need to disconnect first
            g_ap_to_attempt.disconnectFirst = true;
        }
    }
    strncpy(g_ap_to_attempt.connInfo.ssid, target_ssid, 32);
    g_ap_to_attempt.connInfo.ssid[32]    = 0;
    g_is_connecting_to_ap                = true;
    g_ap_to_attempt.connInfo.password[0] = 0;
    g_ap_to_attempt.conn_idx             = idx;

    queue_connect_to_ap_work();

    return 0;
}

////////////////////////////////////////////////////
// rm_connect_to_AP()
//  Connect to the AP with the given ssid and password
//
//  @param ssid the ssid of the AP to connect to
//  @param password the password of the AP to connect to
//  @param sec the security type of the AP
//  @param keyidx the key index of the AP
//  @param enc the encryption type of the AP
//
int rm_connect_to_AP(char *ssid, char *password, uint16_t sec, uint16_t keyidx, uint16_t enc)
{
    if (g_is_connecting_to_ap) {
        LOG_ERR("Already connecting to an AP");
        return -EAGAIN;
    }
    if (!g_use_wifi) {
        LOG_ERR("Wifi is disabled, can't connect to AP2");
        return -ENOTSUP;
    }
    // Check to see if they are asking for the currently connected AP with the exact same info
    // as what is in the saved_ssid list.  This should mean that our current connecting was made
    // with those.  If it is different then they are asking us to try a new passworkd

    // Check to see if these params were used to connect
    int ret = wifi_is_curr_AP(ssid, password, sec, keyidx, enc);
    if (ret == 0) {
        LOG_DBG("Already connected to AP %s with those creds", ssid);
        return -EEXIST;
    }
    if (ret == 1) {    // Same AP, different creds
        g_ap_to_attempt.disconnectFirst = true;
    }
    if (ret == 2) {
        g_ap_to_attempt.disconnectFirst = false;
    }

    g_is_connecting_to_ap = true;

    strncpy(g_ap_to_attempt.connInfo.ssid, ssid, 32);
    g_ap_to_attempt.connInfo.ssid[32] = '\0';
    strncpy(g_ap_to_attempt.connInfo.password, password, 64);
    g_ap_to_attempt.connInfo.password[64] = '\0';
    g_ap_to_attempt.connInfo.sec          = sec;
    g_ap_to_attempt.connInfo.keyidx       = keyidx;
    g_ap_to_attempt.connInfo.enc          = enc;
    g_ap_to_attempt.conn_idx              = -1;    // required to connect by creds

    queue_connect_to_ap_work();
    return 0;
}

static void connecting_to_ap_work_handler(struct k_work *item)
{
    // The DA does not respond to any SPI messagers until it is done connecting
    // so we grab the mutex so that no one can try without generating a error
    // which we should then fix by having that code check to see if we are in
    // the middle of connecting
    if (!g_use_wifi) {    // Could have been turned on after we queued this work
        LOG_ERR("Wifi is disabled, not connecting to AP");
        return;
    }
    int ret = wifi_get_mutex(K_SECONDS(10), __func__);
    if (ret != 0) {
        LOG_ERR("'%s'(%d) getting wifi mutex to connect to AP", wstrerr(-ret), ret);
        g_is_connecting_to_ap = false;
        return;
    }

    if (g_ap_to_attempt.disconnectFirst == true) {
        ret = wifi_disconnect_from_AP(K_SECONDS(2));
        if (ret != 0) {
            LOG_ERR("'%s'(%d) disconnecting from AP", wstrerr(-ret), ret);
            goto ap_conn_failed;
        }
        k_sleep(K_SECONDS(1));    // Give the DA time to disconnect
                                  // EAS XXX figure out how to notify whoever asked us to connect that we can't
    }
    // Before we connect, put us in the DPM mode we need based on the sleep mode
    // This is done by prepare_radio_for_use()
    if (rm_prepare_radio_for_use(COMM_DEVICE_DA16200, false, K_SECONDS(3)) == false) {
        LOG_ERR("Failed to prepare DA for AP connection");
        goto ap_conn_failed;
    }

    if (g_ap_to_attempt.conn_idx == -1) {
        LOG_WRN("Connecting to (%s)", g_ap_to_attempt.connInfo.ssid);
        ret = wifi_initiate_connect_to_ssid(
            g_ap_to_attempt.connInfo.ssid,
            g_ap_to_attempt.connInfo.password,
            g_ap_to_attempt.connInfo.sec,
            g_ap_to_attempt.connInfo.keyidx,
            g_ap_to_attempt.connInfo.enc,
            false,
            K_SECONDS(10));
    } else {
        LOG_WRN("Connecting to AP by index %d (%s)", g_ap_to_attempt.conn_idx, g_ap_to_attempt.connInfo.ssid);
        ret = wifi_initiate_connect_by_index(g_ap_to_attempt.conn_idx, K_SECONDS(10));
        if (ret == -821) {  // We updated the DA and changed the SSID encryption
            wifi_saved_ssids_del_all(K_MSEC(3000));
        }
    }
    if (ret != 0) {
        LOG_ERR("'%s' (%d) connecting to AP", wstrerr(-ret), ret);
        goto ap_conn_done_and_exit;
    }
    k_timeout_t   timeout   = K_SECONDS(25);
    k_timepoint_t timepoint = sys_timepoint_calc(timeout);
    // We have initiated a connection which might take some time or could have already
    // completed.  Wrong creds can take a long time (15 sec),
    // Wait for the da_state to change to the AP we are connecting to
    while (true) {
        if (da_state.ap_connected == DA_STATE_KNOWN_TRUE
            && (strncmp(da_state.ap_name, g_ap_to_attempt.connInfo.ssid, 32) == 0)) {
            break;
        }
        timeout = sys_timepoint_timeout(timepoint);
        if (K_TIMEOUT_EQ(timeout, K_NO_WAIT) == true) {
            LOG_ERR("Timed out waiting for |%s| to connect", g_ap_to_attempt.connInfo.ssid);
            goto ap_conn_done_and_exit;
        }
        k_sleep(K_MSEC(100));
    }
    LOG_DBG("Connect attempt to AP %s succeeded", g_ap_to_attempt.connInfo.ssid);

    // If we were asked to conencect by creds, then we need to save the creds
    // if we were successful
    if (g_ap_to_attempt.conn_idx == -1) {
        int saved_idx = wifi_find_saved_ssid(g_ap_to_attempt.connInfo.ssid);
        if (saved_idx == -1) {
            if (wifi_num_saved_ssids() < MAX_SAVED_SSIDS) {
                LOG_INF("AP %s is not in saved SSID list, adding", g_ap_to_attempt.connInfo.ssid);
                int ret = wifi_saved_ssids_add(
                    -1,
                    g_ap_to_attempt.connInfo.ssid,
                    g_ap_to_attempt.connInfo.password,
                    g_ap_to_attempt.connInfo.sec,
                    g_ap_to_attempt.connInfo.keyidx,
                    g_ap_to_attempt.connInfo.enc,
                    false,
                    true,
                    K_SECONDS(2));
                if (ret != 0) {
                    LOG_ERR("'%s'(%d) saving new AP to saved SSID list", wstrerr(-ret), ret);
                }
            } else {
                LOG_ERR(
                    "We just connected to an new AP but we are out of room to "
                    "save it!");
            }
        } else {
            LOG_INF("AP %s is in saved SSID list at idx %d.", da_state.ap_name, saved_idx);
        }
    }

    da_state.mqtt_enabled          = DA_STATE_UNKNOWN;
    da_state.mqtt_broker_connected = DA_STATE_UNKNOWN;
    rm_switch_to(COMM_DEVICE_DA16200, false, false);
ap_conn_done_and_exit:
    rm_done_with_radio(COMM_DEVICE_DA16200);
ap_conn_failed:
    wifi_release_mutex();
    g_is_connecting_to_ap = false;
}

////////////////////////////////////////////////////
// rm_wifi_is_connecting()
//  Return if the radio manager is using sleep
//
bool rm_wifi_is_connecting()
{
    return (g_is_connecting_to_ap || (g_switching_radios && switch_radios_info.target_radio == COMM_DEVICE_DA16200));
}

char *rm_op_str()
{
    switch (switch_radios_info.currOp) {
    case SRS_IDLE:
        return "Idle";
    case SRS_STATE_TO_BE_KNOWN:
        return "Waiting for da_states to be known";
    case SRS_WAKING_DA:
        return "Waking DA";
    case SRS_SLEEPING_DA:
        return "Sleeping DA";
    case SRS_TURNING_ON_BROKER:
        return "Enabling mqtt";
    case SRS_TURNING_OFF_BROKER:
        return "Disabling mqtt";
    case SRS_TURNING_ON_BROKER_BOOT:
        return "Enabling mqtt boot";
    case SRS_TURNING_OFF_BROKER_BOOT:
        return "Disabling mqtt boot";
    case SRS_WAIT_FOR_AP:
        return "Waiting for AP to connect";
    case SRS_DISABLING_LTE_MQTT:
        return "Disabling MQTT";
    case SRS_ENABLING_LTE_MQTT:
        return "Enabling MQTT";
    case SRS_WAIT_FOR_LTE_READY:
        return "Waiting for LTE to be ready to receive commands";
    case SRS_WAIT_FOR_LTE_CONNECT:
        return "Waiting for LTE to connect";
    case SRS_WAIT_FOR_LTE_DISCONNECT:
        return "Waiting for LTE to disconnect";
    case SRS_WAIT_FOR_BROKER:
        return "Waiting for MQTT broker to connect";
    case SRS_STOP_AP_USE:
        return "Stopping AP Profile from being used";
    default:
        return "Unknown";
    }
}

////////////////////////////////////////////////////
// rm_is_active_radio_mqtt_connected()
//  Return if the active radio is connected to the
// MQTT broker
bool rm_is_active_radio_mqtt_connected()
{
    bool lteok = rm_get_active_mqtt_radio() == COMM_DEVICE_NRF9160 && is_lte_mqtt_connected();
    bool daok =
        rm_get_active_mqtt_radio() == COMM_DEVICE_DA16200 && da_state.mqtt_broker_connected == DA_STATE_KNOWN_TRUE;

    return (lteok || daok);
}

////////////////////////////////////////////////////
// rm_is_switching_radios()
//  Return whether the radio manager is switch
bool rm_is_switching_radios(void)
{
    return g_switching_radios || g_is_connecting_to_ap;
}

////////////////////////////////////////////////////
// rm_get_active_mqtt_radio()
//  Return the radio that is ready to be used for
// MQTT communications to the server.  Since the
// state of the radios can change at any time, any
// message sent could still fail to send.
//
//  @return the radio that is ready to be used for
// MQTT communications to the server
comm_device_type_t rm_get_active_mqtt_radio()
{
    return g_active_radio;
}

////////////////////////////////////////////////////
// rm_ready_for_mqtt()
//  Return true if there is a radio that is ready
// to send MQTT messages.  This checks not only if
// there is an active radio, but whether the radio
// reports it is connected to the MQTT broker at
// the moment.
//
//  @return bool if there is a radio ready to send MQTT
bool rm_ready_for_mqtt()
{
    if (g_active_radio == COMM_DEVICE_NRF9160) {
        return (
            status_getBit(modem_status_shadow.status_flags, STATUS_MQTT_ENABLED)
            && status_getBit(modem_status_shadow.status_flags, STATUS_MQTT_CONNECTED));
    }

    if (g_active_radio == COMM_DEVICE_DA16200) {
        return (da_state.mqtt_broker_connected == 1);
    }
    return false;
}

////////////////////////////////////////////////////////////
// insure_da_is_awake()
//  Make sure the DA is not sleeping.  If we are using
// DPM, then we need to wake the DA and tell it to stay
// awake.  If we are not using DPM, then we need to make
// sure DPM is off.
//   This can be called multiple time so we need to do the
// work in a non-destructive way if the right state is
// already set.
//
//  @param timeout how long to wait for the radio to be ready
//
//  @return 0 on success, -1 on failure
static int insure_da_is_awake()
{
    int ret = 0;

    // Check if we shipped from factory, if not do nothing
    if (uicr_shipping_flag_get() == false) {
        return 0;
    }

    bool was_powered = da_state.powered_on;
    LOG_DBG(
        "powered = %d, dpm = %d, sleep = %d, conn=%d, use_sleep=%d",
        da_state.powered_on,
        da_state.dpm_mode,
        da_state.is_sleeping,
        da_state.ap_connected,
        g_use_sleep);
    if (g_radio_mgmt == false) {
        LOG_WRN("Radio mgmt off, not insuring da is awake");
        return 0;
    }
    // This insures the DA is powered and waits for it to
    // boot.  If it was powered on it does nothing
    // We can also expect tht if it booted, the DA told us its DPM state
    net_mgr_wifi_power(true);

    if (g_use_sleep == true) {
        if (was_powered) {
            // If we didn't just power up, make sure we don't send a wake up
            // pulse too quickly after the last wake up pulse due to DA bugs
            int wnw = wifi_time_to_next_wake();
            if (wnw > 0) {
                // This doesn't happen often in practice.  I had a debug
                // message here to see and it is infrequent.   As such it
                // make sense to wait the time needed for the DA to settle
                // and continue
                LOG_DBG("Too soon to wake DA from sleep, need %d more ms, waiting", wnw);
                k_sleep(K_MSEC(wnw));    // This works because its the DA that needs settling
            }
        }
        // Regardless of whether we just powered the DA, we want to set the
        // correct dpm mode.
        ret = wifi_set_sleep_mode(WIFI_SLEEP_DPM_AWAKE, 0);
    } else {
        // If we are NOT using sleep then we want make sure
        // DPM is off and sleep if off.
        // This call should do nothing if things are correct
        ret = wifi_set_sleep_mode(WIFI_SLEEP_NONE, 0);
    }
    return ret;
}

static int16_t prep_ref_cnt[2] = { 0, 0 };
void           rm_got_UC_from_AP()
{
    // Bump the "radio prepped" count for the DA
    prep_ref_cnt[0]++;
}
////////////////////////////////////////////////////////////
// rm_prepare_radio_for_use()
//   Make sure the radio is ready to use.
//
// For the DA:
//   If it was sleeping, wake it. If MQTT is needed, check
// if the broker is connected. This is done in a way that
// respects if sleep is enabled so that if we choose not to
// use sleep mode, this will keep the DA awake.
//
// For LTE:
//   Wait for the MQTT to be connected before returning. In
// stable LTE conditions, this should be immediate, but
// in unstable conditions, waiting for it makes the next
// message send more likely to succeed.
//
// This prep work should be done once be group of messages
// sent at time since waking and sleeping or waiting can
// use power and time. Most message sends are one offs so
// to allow lower level message send functions to use this
// for individual message and higher level message grouping
// code to use it, we keep a reference count of the number
// of times this is called and only put the radio back to
// sleep when the last call is done.
//
// The ref counts are per radio, as such, a call to
// rm_prepare_radio_for_use() must be matched one for one
// with a call to rm_done_with_radio() for each radio
//
//  @param device the device to prepare
//  @param need_mqtt if true, then we need to make sure
// MQTT is connected
//  @param timeout how long to wait for the radio to be ready
//
//  @return  bool if the wifi is ready to use
bool rm_prepare_radio_for_use(comm_device_type_t device, bool need_mqtt, k_timeout_t timeout)
{
    int           ret;
    int           radio_idx = (device == COMM_DEVICE_DA16200) ? 0 : 1;
    k_timepoint_t timepoint = sys_timepoint_calc(timeout);
    prep_ref_cnt[radio_idx]++;
    LOG_DBG("Prep ref with %s, ref count = %d", comm_dev_str(device), prep_ref_cnt[radio_idx]);
    // Its ok to do the following multiple times

    if (device == COMM_DEVICE_DA16200) {
        if ((ret = insure_da_is_awake()) != 0) {
            // We didn't actually prep the radio so decrement the ref count
            LOG_ERR("'%s'(%d) insure_da_is_awake", wstrerr(-ret), ret);
            prep_ref_cnt[radio_idx]--;
            return false;
        }
        // Like LTE, DA also can disconnect temporarily from the broker, so loop check
        if (need_mqtt) {
            while (da_state.mqtt_broker_connected != DA_STATE_KNOWN_TRUE) {
                k_sleep(K_MSEC(50));
                timeout = sys_timepoint_timeout(timepoint);
                ret     = wifi_get_mqtt_state(timeout);
                if (K_TIMEOUT_EQ(timeout, K_NO_WAIT) == true) {
                    LOG_ERR("MQTT is not connected on Wifi, but is needed");
                    // We didn't actually prep the radio so decrement the ref
                    // count
                    prep_ref_cnt[radio_idx]--;
                    return false;
                }
            }
        }
    } else if (device == COMM_DEVICE_NRF9160) {
        if (need_mqtt) {
            k_timepoint_t timepoint = sys_timepoint_calc(timeout);
            while (is_lte_mqtt_connected() == false) {
                k_sleep(K_MSEC(100));
                timeout = sys_timepoint_timeout(timepoint);
                if (K_TIMEOUT_EQ(timeout, K_NO_WAIT) == true) {
                    // LOG_ERR("MQTT is not connected on LTE, but is needed");
                    //  We didn't actually prep the radio so decrement the ref
                    //  count
                    prep_ref_cnt[radio_idx]--;
                    return false;
                }
            }
        }
    }
    return true;
}

////////////////////////////////////////////////////////////
// return_da_to_sleep()
//  Put the DA back to sleep if needed.
//
//  @return 0 on success, -1 on failure
static int return_da_to_sleep()
{
    int ret = 0;

    // Check if we shipped from factory, if not leave the DA awake
    if (uicr_shipping_flag_get() == false) {
        return 0;
    }

    if (g_radio_mgmt == false) {
        LOG_WRN("Radio mgmt off, not changing da state");
        return 0;
    }
    if (rm_wifi_is_connecting()) {
        LOG_DBG("DA is connecting to an AP, so we don't sleep it");
        return 0;
    }

    // Removed check on ap connected in this if. Old logic. Should not be handled by
    // rm_wifi_is_connecting() above
    if (rm_get_active_mqtt_radio() == COMM_DEVICE_DA16200) {
        if (g_use_sleep == true) {
            if ((da_state.dpm_mode == DA_STATE_UNKNOWN) || (da_state.is_sleeping == DA_STATE_UNKNOWN)) {
                LOG_ERR(
                    "DA DPM state or sleep state is unknown, managing them now "
                    "won't work");
                return -EBADE;
            }

            if (da_state.ap_connected == DA_STATE_KNOWN_TRUE) {
                LOG_DBG("Setting DPM sleep mode");
                ret = wifi_set_sleep_mode(WIFI_SLEEP_DPM_ASLEEP, 0);
                if (ret == 0) {
                    // We have told the DA to sleep and didn't get an
                    // error.  It may not sleep right away because its
                    // doing things, but if we didn't get an error, we
                    // need to trust it will eventually sleep
                    return 0;
                }    // else fall through to return ret
            } else {
                // We aren't connecting, DA is active, but not connected???
                // make sure that the active radio isn't the DA anymore
                LOG_WRN(
                    "Wifi is active radio, but we are not connected to an AP!, "
                    "going to LTE");
                rm_switch_to(COMM_DEVICE_NRF9160, true, false);
                if (g_usb_bt_prepped_radio == false) {
                    net_mgr_wifi_power(false);
                }
            }
        } else {
            LOG_DBG("Setting DA to no sleep");
            ret = wifi_set_sleep_mode(WIFI_SLEEP_NONE, 0);
        }
    } else {
        // Not active radio or switching to the DA, so turn it off
        if (g_usb_bt_prepped_radio == false) {
            net_mgr_wifi_power(false);
        }
    }
    return ret;
}

////////////////////////////////////////////////////
// rm_done_with_radio()
// Called when we are done with the radio. If its
// the DA, we wantto put it back to sleep (if dpm
// is enabled), if its the 9160, we do nothing
//
//  @return 0 on success, -1 on failure
int rm_done_with_radio(comm_device_type_t device)
{
    int ret = 0;

    int radio_idx = (device == COMM_DEVICE_DA16200) ? 0 : 1;
    if (prep_ref_cnt[radio_idx] <= 0) {
        LOG_ERR("called too many times for %s", comm_dev_str(device));
        return -EMLINK;
    }
    prep_ref_cnt[radio_idx]--;
    LOG_DBG("Done with %s, ref count = %d", comm_dev_str(device), prep_ref_cnt[radio_idx]);
    if (prep_ref_cnt[radio_idx] > 0) {
        return 0;
    }

    if (device == COMM_DEVICE_DA16200) {
        ret = return_da_to_sleep();
        if (ret != 0) {
            LOG_ERR("'%s'(%d) Trying to sleep DA", wstrerr(-ret), ret);
        }
    }
    return ret;
}

////////////////////////////////////////////////////
// rm_switch_to()
//  Switch to the radio specified.  If the radio is
// already active, then nothing is done.  If the radio
// is not active, then the radio is switched to. if
// the radio being switch to is different then the
// newly requested radio, then we start trying to
// switch to the new radio.
//
// @param radio the radio to switch toS
// @param clear_existing_radio set active radio to None if true
// @param force_switch if true, then set all vars as if
//        the switch succeeded (dev only)
//
//
//  @return 0 on success, -1 on failure
int rm_switch_to(comm_device_type_t radio, bool clear_existing_radio, bool force_switch)
{
    if (g_switching_radios && switch_radios_info.target_radio == radio) {
        LOG_DBG("Already switching to %s", comm_dev_str(radio));
        return 0;
    }

    if (!g_use_wifi && radio == COMM_DEVICE_DA16200) {
        LOG_WRN("Wifi is disabled, not switching to da");
        return 0;
    }

    if (radio != COMM_DEVICE_NRF9160 && radio != COMM_DEVICE_DA16200) {
        LOG_ERR("Can't switch to radio type %d", radio);
        return -1;
    }

    switch_radios_info.radio_disabled = false;
    switch_radios_info.radio_enabled  = false;
    if (force_switch) {
        g_active_radio           = radio;
        g_switching_radios       = false;
        g_switching_radios_start = 0;
        return 0;
    }

    if (radio == COMM_DEVICE_NRF9160) {
        LOG_DBG("Switching to LTE from %s", comm_dev_str(g_active_radio));
        switch_radios_info.target_radio = COMM_DEVICE_NRF9160;
        g_switching_radios_start        = k_uptime_get();
    } else if (radio == COMM_DEVICE_DA16200) {
        LOG_DBG("Switching to Wifi from %s", comm_dev_str(g_active_radio));
        switch_radios_info.target_radio = COMM_DEVICE_DA16200;
        g_switching_radios_start        = k_uptime_get();
    }

    if (g_switching_radios == false) {
        // We weren't trying to switch, but now we are
        // The DA can be in sleep mode if things are rebooted in unusual ways
        // While this is probably only a "developer" problem, we can trivial
        // check the DA's dpm state now to make sure the shadow vars are
        // correct
        g_switching_radios = true;
        // Switching radios may need to wait until the radios
        // complete some operations that may take unknown amounts
        // of time, so we periodically check the status of the switch
        // and kick off the next step when the previous step is done
        queue_switching_handler(NULL);
    }
    return 0;
}

////////////////////////////////////////////////////
// failOp()
//  Mark and operation as failed.
//
//  @param info the switch radio info
void failOp(switch_radios_info_t *info)
{
    // To fail this op, just set the start time
    // to have expired for next check
    info->currOpStart -= info->timeout;
}

////////////////////////////////////////////////////
// startOp()
// A convience function to make code readable
//
// If the op asked for is different then the current
// op, then we initialize the new op info and
// return time left.
//
// If its the same op as started and there is time
// left for his op to complete, we update the info
// and return the time left.
//
// If the op has expired and we have retrys left, we
// update the info and return the time left.
//
// If the op has expired and we have no retrys left,
// we return that the op has failed.
//
//  @param info the switch radio info
//  @param timeout the time the operation should take
//  @param op the operation that is being checked
//  @param timeremaining the time left for the operation
//  @param retrys the number of retrys allowed
op_state_t startOp(switch_radios_info_t *info, int timeout, switch_radios_state_t op, int *timeremaining, int retrys)
{
    uint64_t now = k_uptime_get();

    if (info->currOp != op) {    // Starting a new op
        info->currOp      = op;
        info->currOpStart = now;
        info->op_failures = 0;
        info->max_retrys  = retrys;
        info->timeout     = timeout;
        *timeremaining    = timeout;
        return OS_STARTED;
    } else {
        // Continuing the op
        uint64_t time_spent = now - info->currOpStart;
        if (time_spent > info->timeout) {
            // We have timed out
            info->op_failures++;
            if (info->op_failures > info->max_retrys) {
                *timeremaining = -1;
                return OS_FAILED;
            } else {
                info->currOpStart = now;
                *timeremaining    = timeout;
                return OS_STARTED;
            }
        } else {
            *timeremaining = timeout - time_spent;
            return OS_STILL_GOING;
        }
    }
}

////////////////////////////////////////////////////
// disable_wifi()
//   Called to make sure the DA is not trying to
// use MQTT so that it doesn't fight with LTE
//
// @return 0 if wifi is disabled
//         the number of milliseconds before checking
//         the if the state is correct otherwise
int disable_wifi(switch_radios_info_t *info)
{
    int        timeleft, ret, err;
    op_state_t opstate;

    if ((da_state.mqtt_enabled == DA_STATE_KNOWN_FALSE) && (da_state.mqtt_broker_connected == DA_STATE_KNOWN_FALSE)
        && is_da_in_right_sleep_mode()) {
        return 0;
    }

    wifi_check_sleeping(true);

    // To make any changes or queries we need the DA awake
    if (da_state.is_sleeping != DA_STATE_KNOWN_FALSE) {
        opstate = startOp(info, 500, SRS_WAKING_DA, &timeleft, 3);
        if (opstate == OS_STARTED) {
            LOG_DBG("Waking DA to disconnect");
            ret = insure_da_is_awake();
            if (ret != 0) {
                LOG_ERR("'%s'(%d) Trying wake up DA to disconnect, will retry", wstrerr(-ret), ret);
                failOp(info);    // don't wait, mark this try as failed
                return 200;
            }
            // if we didn't get an error waking the DA, assume its awake and
            // move on to the next step
        }
        if (opstate == OS_STILL_GOING) {
            return 100;
        }
        if (opstate == OS_FAILED) {
            LOG_ERR("DA never woke to disconnect. reseting DA");
            wifi_reset();
            return 700;
        }
    }

    if (da_state.mqtt_enabled != DA_STATE_KNOWN_FALSE) {
        ret = wifi_set_mqtt_state(0, K_SECONDS(2));
        if (ret < 0) {
            LOG_ERR("'%s'(%d) disabling wifi MQTT", wstrerr(-ret), ret);
            // This is not critical if we later disconnect from Wifi
            // report it for diagnostics
        }
    }
    if (da_state.ap_connected) {
        ret = wifi_disconnect_from_AP(K_SECONDS(2));
        if (ret < 0) {
            LOG_ERR("'%s'(%d) disconnecting wifi", wstrerr(-ret), ret);
        }
        // EAS TODO - There is no reason that this should loop forever since that would mean
        // that the DA is just not doing what we want but is responding to commands
        // so there isn't anything practical to do here.  We COULD count the number of times
        // this happens and if it happens too often, reset the DA
        return 500;
    }

    if (!is_da_in_right_sleep_mode()) {
        opstate = startOp(info, 1200, SRS_SLEEPING_DA, &timeleft, 4);
        if (opstate == OS_STARTED) {
            LOG_DBG("Putting DA to sleep");
            err = return_da_to_sleep();
            if (err < 0) {
                LOG_ERR("'%s'(%d) sleeping DA, will retry", wstrerr(-err), err);
                failOp(info);    // mark this try as failed
                return 700;
            }
            // We have let the DA sleep, but it could be a while since it has to
            // connect to mqtt before sleeping. So if we don't get an error we
            // assume it will eventually sleep, so return 0 to indicate we are done
            return 0;
        }
        if (opstate == OS_STILL_GOING) {
            return 600;
        }
        if (opstate == OS_FAILED) {
            LOG_ERR("We could not sleep DA going to LTE. turning DA off");
            net_mgr_wifi_power(false);
            return 0;    // We are done disabling wifi
        }
    }
    return 0;
}

////////////////////////////////////////////////////
// enable_wifi()
//   Called to make sure the DA is trying to
// use MQTT
//
// This function should be able to be called earlier
// then it expects and should not assume the state of
// the DA is in any particular state.
//
// @return 0 if wifi is enabled
//         the number of milliseconds before checking
//         the if the state is correct otherwise
int enable_wifi(switch_radios_info_t *info)
{
    int        timeleft, ret, err;
    op_state_t opstate;

    net_mgr_wifi_power(true);
    wifi_check_sleeping(true);

    // The state we want the DA in when it is not the active radio is:
    // 1) mqtt_on_boot is set
    // 2) DA is connected to an AP
    // 3) DA is connected to mqtt broker AND
    // 4) the DA is either in dpm sleep or non-dpm sleep based the sleep flag
    if ((da_state.mqtt_enabled == DA_STATE_KNOWN_TRUE) && (da_state.mqtt_on_boot == DA_STATE_KNOWN_TRUE)
        && (da_state.mqtt_broker_connected == DA_STATE_KNOWN_TRUE) && is_da_in_right_sleep_mode()) {
        return 0;
    }
    LOG_DBG(
        "DA state: dpm=%d, sleep=%d, mqtt=%d, mqtt_enabled=%d, mqtt_on_boot=%d, "
        "ap_connected=%d",
        da_state.dpm_mode,
        da_state.is_sleeping,
        da_state.mqtt_broker_connected,
        da_state.mqtt_enabled,
        da_state.mqtt_on_boot,
        da_state.ap_connected);

    // We disable LTE mqtt if it's the active radio while we switch to Wifi.
    // If we succeed in switching this is ok
    // If we fail we will fall back to LTE which will re-enable it
    if (g_active_radio == COMM_DEVICE_NRF9160 && is_lte_mqtt_enabled()) {
        opstate = startOp(info, 1500, SRS_DISABLING_LTE_MQTT, &timeleft, 3);
        if (opstate == OS_STARTED) {
            LOG_DBG("disabling LTE MQTT");
            err = lte_set_mqtt_enable(false, K_MSEC(1000));
            if (err < 0) {
                LOG_ERR("'%s'(%d) disabling MQTT, will retry", wstrerr(-err), err);
            }
            return 500;
        }
        if (opstate == OS_STILL_GOING) {
            return 500;
        }
        if (opstate == OS_FAILED) {
            LOG_DBG("MQTT is taking too long to disable, fallback to LTE");
            rm_switch_to(COMM_DEVICE_NRF9160, false, false);
            return 200;
        }
    }

    // To make any changes or queries we need the DA awake
    if (da_state.is_sleeping != DA_STATE_KNOWN_FALSE) {
        opstate = startOp(info, 500, SRS_WAKING_DA, &timeleft, 3);
        if (opstate == OS_STARTED) {
            LOG_DBG("Waking DA");
            ret = insure_da_is_awake();
            if (ret != 0) {
                LOG_ERR("'%s'(%d) Trying wake up DA, will retry", wstrerr(-ret), ret);
                failOp(info);    // don't wait, mark this try as failed
                return 200;
            }
            // if we didn't get an error waking the DA, assume its awake and
            // move on to the next step
        }
        if (opstate == OS_STILL_GOING) {
            return 100;
        }
        if (opstate == OS_FAILED) {
            LOG_ERR("DA never woke to enable. Fallback to LTE");
            rm_switch_to(COMM_DEVICE_NRF9160, false, false);
            return 200;
        }
    }

    // We need the state of the DA to be known.  Its unknown for a time when
    // it boots, so we need to wait for it to become know for a time
    if (da_state.mqtt_on_boot == DA_STATE_UNKNOWN || da_state.mqtt_broker_connected == DA_STATE_UNKNOWN) {
        opstate = startOp(info, 1000, SRS_STATE_TO_BE_KNOWN, &timeleft, 3);
        if (opstate == OS_STARTED) {
            // None of these states should be unknown under normal conditions
            // so if they are now, warn about it and issue queries to get the state
            if (da_state.mqtt_broker_connected == DA_STATE_UNKNOWN) {
                LOG_DBG("DA mqtt state is unknown, querying for state");
                ret = wifi_get_mqtt_state(K_MSEC(100));
                if (ret < 0) {
                    LOG_WRN("'%s'(%d) getting mqtt state", wstrerr(-ret), ret);
                } else {
                    // >= 0 means the state is now ret
                }
            }
            if (da_state.mqtt_on_boot == DA_STATE_UNKNOWN) {
                LOG_WRN("DA mqtt on boot state is unknown, querying for state");
                ret = wifi_get_mqtt_boot_state(K_MSEC(150));
                if (ret < 0) {
                    LOG_WRN("'%s'(%d) getting mqtt on boot state", wstrerr(-ret), ret);
                } else {
                    // >= 0 means the state is now ret
                }
            }
            return 100;
        }
        if (opstate == OS_STILL_GOING) {
            return 100;    //
        }
        if (opstate == OS_FAILED) {
            LOG_ERR("DA state stayed unknown. Fallback to LTE");
            rm_switch_to(COMM_DEVICE_NRF9160, false, false);
            return 200;
        }
    }

    // We need to be connected to an AP.  We will wait a little to allow
    // for it maybe coming up soon
    if (da_state.ap_connected != DA_STATE_KNOWN_TRUE) {
        opstate = startOp(info, 1000, SRS_WAIT_FOR_AP, &timeleft, 3);
        if (opstate == OS_STARTED) {
            LOG_DBG("Waiting for AP to connect");
            // Nothing to do but wait and check back
            return 1000;
        }
        if (opstate == OS_STILL_GOING) {
            return timeleft;    //
        }
        if (opstate == OS_FAILED) {
            LOG_ERR("DA never connected to an AP. Fallback to LTE");
            rm_switch_to(COMM_DEVICE_NRF9160, false, false);
            return 200;
        }
    }

    // We know at this point that the DA is not enabled

    if (da_state.mqtt_on_boot != DA_STATE_KNOWN_TRUE) {
        opstate = startOp(info, 1500, SRS_TURNING_ON_BROKER_BOOT, &timeleft, 2);
        if (opstate == OS_STARTED) {
            LOG_DBG("Enabling MQTT on boot");
            err = wifi_set_mqtt_boot_state(1, K_MSEC(500));
            if (err < 0) {
                LOG_ERR("'%s'(%d) enabling MQTT on boot, will retry", wstrerr(-err), err);
                failOp(info);
            }
            return 100;
        }
        if (opstate == OS_STILL_GOING) {
            return 100;
        }
        if (opstate == OS_FAILED) {
            LOG_ERR("We could not turn on DA mqtt on boot. Fallback to LTE");
            rm_switch_to(COMM_DEVICE_NRF9160, false, false);
            return 200;
        }
    }

    if (da_state.mqtt_enabled != DA_STATE_KNOWN_TRUE) {
        opstate = startOp(info, 1500, SRS_TURNING_ON_BROKER, &timeleft, 2);
        if (opstate == OS_STARTED) {
            LOG_DBG("Enabling MQTT");
            err = wifi_set_mqtt_state(1, K_MSEC(500));
            if (err < 0) {
                LOG_ERR("'%s'(%d) enabling MQTT, will retry", wstrerr(-err), err);
                failOp(info);
            }
            return 100;
        }
        if (opstate == OS_STILL_GOING) {
            return 100;
        }
        if (opstate == OS_FAILED) {
            LOG_ERR("We could not turn on DA mqtt. Fallback to LTE");
            rm_switch_to(COMM_DEVICE_NRF9160, false, false);
            return 200;
        }
    }

    if (da_state.mqtt_broker_connected != DA_STATE_KNOWN_TRUE) {
        opstate = startOp(info, 30000, SRS_WAIT_FOR_BROKER, &timeleft, 1);
        if (opstate == OS_STARTED) {
            LOG_DBG("Waiting for MQTT to connect");
            return 3000;
        }
        if (opstate == OS_STILL_GOING) {
            return 3000;
        }
        if (opstate == OS_FAILED) {
            LOG_ERR("MQTT never connected. Fallback to LTE");
            rm_switch_to(COMM_DEVICE_NRF9160, false, false);
            return 200;
        }
    }

    if (!is_da_in_right_sleep_mode()) {
        opstate = startOp(info, 1200, SRS_SLEEPING_DA, &timeleft, 4);
        if (opstate == OS_STARTED) {
            LOG_DBG("Putting DA to sleep");
            err = return_da_to_sleep();
            if (err < 0) {
                LOG_ERR("'%s'(%d) sleeping DA, will retry", wstrerr(-err), err);
                failOp(info);    // mark this try as failed
                return 700;
            }
            // We have let the DA sleep, but it could be a while since it has to
            // connect to mqtt before sleeping. So if we don't get an error we
            // assume it will eventually sleep, so return 0 to indicate we are done
            return 0;
        }
        if (opstate == OS_STILL_GOING) {
            return 600;
        }
        if (opstate == OS_FAILED) {
            LOG_ERR("We could not sleep DA. Fallback to LTE");
            rm_switch_to(COMM_DEVICE_NRF9160, false, false);
            return 200;
        }
    }
    // The DA is enabled
    return 0;
}

////////////////////////////////////////////////////
// disable_lte()
//   Called to make sure the 9160 is not trying to
// use MQTT so that it doesn't fight with the DA
//
// This function should be able to be called earlier
// then it expects and should not assume the state of
// the DA is in any particular state.
//
// @return 0 if LTE is disabled
//         the number of milliseconds before checking
//         the if the state is correct otherwise
static int disable_lte(switch_radios_info_t *info)
{
    LOG_ERR("Disabling LTE");
    modem_power_off();    // Make sure that the modem is powered off
    return 0;
}

////////////////////////////////////////////////////
// enable_lte()
//   Called to make sure the 9160 is using MQTT
//
// This function should be able to be called earlier
// then it expects and should not assume the state of
// the 9160 is in any particular state.
//
// @return 0 if LTE is enabled
//         the number of milliseconds before checking
//         the if the state is correct otherwise
static int enable_lte(switch_radios_info_t *info)
{
    int        timeleft, err;
    op_state_t opstate;

    // This fuction is only called if we want to swtich to LTE.  Because the only
    // requirement for LTE to be "active" is that the 9160 is on, its possible that
    // the Wifi has its MQTT enabled and in erroneous cases, might be connected.
    // So before we make sure the 9160 is on, we need to make sure Wifi MQTT is not

    // We disable WIFI mqtt if it's the active radio while we switch to LTE.
    // If we succeed in switching this is ok
    // If we fail we will fall back to NONE which will cause a SSID scan and re-enable it
    if (g_active_radio == COMM_DEVICE_DA16200 && da_state.mqtt_broker_connected == DA_STATE_KNOWN_TRUE) {
        opstate = startOp(info, 1500, SRS_TURNING_OFF_BROKER, &timeleft, 3);
        if (opstate == OS_STARTED) {
            LOG_DBG("disabling Wifi MQTT");
            if (rm_prepare_radio_for_use(COMM_DEVICE_DA16200, false, K_SECONDS(3)) == false) {
                LOG_ERR("Failed to prepare DA for MQTT disable");
                return 500;
            }
            err = wifi_set_mqtt_state(0, K_MSEC(500));
            if (err < 0) {
                LOG_ERR("'%s'(%d) disabling Wifi MQTT, will retry", wstrerr(-err), err);
            }
            rm_done_with_radio(COMM_DEVICE_DA16200);
            return 500;
        }
        if (opstate == OS_STILL_GOING) {
            return 500;
        }
        if (opstate == OS_FAILED) {
            LOG_DBG("Wifi MQTT is taking too long to disable, restarting the DA");
            wifi_reset();
            return 1000;
        }
    }

    // The state we want the 9160 in when it is the active radio is:
    // 1) modem is powered on
    // 2) LTE is connected
    // 3) MQTT is enabled
    if (modem_is_powered_on() && is_lte_mqtt_enabled() && has_lte_been_connected(NULL)) {
        return 0;    // lte is enabled
    }

    // The modem may be off or just been turned on so power it on and for it to report its
    // powered on
    if (!modem_is_powered_on()) {
        opstate = startOp(info, 4000, SRS_WAIT_FOR_LTE_READY, &timeleft, 3);
        if (opstate == OS_STARTED) {
            int ret = modem_power_on();    // Make sure that the modem is powered on
            if (ret != 0) {
                LOG_ERR("'%s'(%d) powering on modem, voltage must be too low", wstrerr(-ret), ret);
                failOp(info);    // mark this try as failed
                return 500;
            }
            LOG_DBG("Waiting for LTE to be ready for commands");
            return 500;
        }
        if (opstate == OS_STILL_GOING) {
            return 500;
        }
        if (opstate == OS_FAILED) {
            LOG_ERR(
                "LTE never became ready for commands after 4 x 3 seconds!  "
                "Something is very wrong! Hard resetting the nrf9160");
            int ret = pmic_power_off_modem(true);
            if (ret != 0) {
                LOG_ERR("'%s'(%d) powering off modem ", wstrerr(-ret), ret);
            }
            k_sleep(K_SECONDS(3));
            ret = pmic_power_on_modem();
            if (ret != 0) {
                LOG_ERR("'%s'(%d) powering on modem", wstrerr(-ret), ret);
            }
            return 1000;
        }
    }

    // We know that the LTE is ready for commands and that MQTT is not enabled
    if (!is_lte_mqtt_enabled()) {
        opstate = startOp(info, 1500, SRS_ENABLING_LTE_MQTT, &timeleft, 3);
        if (opstate == OS_STARTED) {
            LOG_DBG("enabling MQTT");
            err = lte_set_mqtt_enable(true, K_MSEC(1000));
            if (err < 0) {
                LOG_ERR("'%s'(%d) enabling MQTT, will retry", wstrerr(-err), err);
            }
            return 500;
        }
        if (opstate == OS_STILL_GOING) {
            return 500;
        }
        if (opstate == OS_FAILED) {
            LOG_ERR("MQTT is taking too long to enable, Hard resetting the nrf9160");
            int ret = pmic_power_off_modem(true);
            if (ret != 0) {
                LOG_ERR("'%s'(%d) powering off modem ", wstrerr(-ret), ret);
            }
            k_sleep(K_SECONDS(3));
            ret = pmic_power_on_modem();
            if (ret != 0) {
                LOG_ERR("'%s'(%d) powering on modem", wstrerr(-ret), ret);
            }
            return 200;
        }
    }

    // We know that MQTT is enabled, since there is not fallback to LTE
    // there no reason to wait for mqtt to connect.  So done
    return 0;
}

////////////////////////////////////////////////////
// switch_radios_work_handler()
//  Called when we need to stop one radio and maybe
// start the other radio.
//
// For the moment, we leave the 9160 on and try to
// leave the DA in DPM sleep, so switching between
// the two should be just to verify that they are
// connected to a network and turn on the mqtt
//
//  @return void
void switch_radios_work_handler(struct k_work *item)
{
    int                   time_to_wait = 1000, ret;
    switch_radios_info_t *info         = CONTAINER_OF(item, switch_radios_info_t, SR_work);
    if (uicr_shipping_flag_get() == false) {
        // Don't print or do anything
        return;
    }
    ret = k_mutex_lock(&RM_mutex, K_NO_WAIT);
    if (ret != 0) {
        return;
    }

    if (g_radio_mgmt == false && g_do_one_step != true) {
        goto unlock_and_return;
    }

    if (k_uptime_get() < 8000) {
        // We need to wait a little bit before we start switching radios
        time_to_wait = 5000;
        goto unlock_and_return;
    }

    // This function is called multiple times to set the state of the radios
    // trying to get the target radio state in the right state.

    // The code after this should check the shadow states to see what needs
    // to be done next and shouldn't assume that the radio we are targetting
    // is the same as the last time we got called
    // This allows the target to change based on outside conditions in mid switch
    // Allowable targets are 9160, 16200 and NONE

    // If we are are switching to Wifi, enable it
    if (info->target_radio == COMM_DEVICE_DA16200 && info->radio_enabled == false) {
        time_to_wait = enable_wifi(info);    // Returns 0 if already on
        if (time_to_wait > 0) {
            goto unlock_and_return;
        }
    }

    // If we are are switching to LTE, make sure its enabled
    if (info->target_radio == COMM_DEVICE_NRF9160 && info->radio_enabled == false) {
        time_to_wait = enable_lte(info);    // Returns 0 if already on
        if (time_to_wait > 0) {
            goto unlock_and_return;
        }
    }
    info->radio_enabled = true;

    // If we are aren't switching to Wifi, make sure its MQTT is off
    if (info->target_radio != COMM_DEVICE_DA16200 && info->radio_disabled == false) {
        time_to_wait = disable_wifi(info);    // Returns 0 if already off
        if (time_to_wait > 0) {
            goto unlock_and_return;
        }
    }

    // If we are aren't switching to LTE, make sure its MQTT is off
    if (info->target_radio != COMM_DEVICE_NRF9160 && info->radio_disabled == false) {
        time_to_wait = disable_lte(info);    // Returns 0 if already off
        if (time_to_wait > 0) {
            goto unlock_and_return;
        }
    }
    info->radio_disabled = true;

    // At this point, the right radio is on and the other radio is off
    // Mark the correct radio as active so that MQTT messages stop going
    // through the old active radio and start going through the new one

    // If Wifi is supposed to be active, activate it
    if (info->target_radio == COMM_DEVICE_DA16200) {
        g_active_radio            = COMM_DEVICE_DA16200;
        time_to_wait              = 0;
        switch_radios_info.currOp = SRS_IDLE;
        g_switching_radios        = false;
        LOG_WRN("Switched to DA16200");
        commMgr_switched_to_wifi();
        goto unlock_and_return;
    }

    // If LTE is supposed to be active, activate it
    if (info->target_radio == COMM_DEVICE_NRF9160) {
        g_active_radio            = COMM_DEVICE_NRF9160;
        time_to_wait              = 0;
        switch_radios_info.currOp = SRS_IDLE;
        g_switching_radios        = false;
        LOG_WRN("Switched to NRF9160");
        commMgr_switched_to_lte();
        // We are confirmed to be on LTE, make sure the DA is off
        // in the case we switched from power to battery this is missed
        return_da_to_sleep();
        goto unlock_and_return;
    }

    // EAS XXX At this point we know that the state of the radios is correct
    // We normally stop the timer so that this function stops being called
    // However an option is to keep call this periodically to make sure the
    // radios stay in the correct state. We can enable this not stopping the
    // timer and setting time_to_wait to a non-zero value

unlock_and_return:
    if (g_do_one_step == false && time_to_wait > 0) {
        k_timer_start(&continue_switching_timer, K_MSEC(time_to_wait), K_NO_WAIT);
    } else {
        if (g_do_one_step == true) {
            LOG_DBG("One step switch, wants to be called again in %d ms", time_to_wait);
            g_do_one_step = false;
        } else {
            k_timer_stop(&continue_switching_timer);
        }
    }
    k_mutex_unlock(&RM_mutex);
}

////////////////////////////////////////////////////
// rm_da_work_handler()
//  Called when the DA state changes, and we need to
// switch radios
//
//  @return void
static void rm_da_work_handler(struct k_work *item)
{
    workref_t        *wr        = CONTAINER_OF(item, workref_t, work);
    zbus_work_info_t *work_info = (zbus_work_info_t *)wr->reference;
    int               ret       = 0;

    if (uicr_shipping_flag_get() == false) {
        // Don't print or do anything
        goto free_and_exit;
    }
    if (g_radio_mgmt == false) {
        LOG_DBG("Radio mgmr off, Skipping DA status update");
        goto free_and_exit;
    }

    if (work_info->msg.evt.events & DA_EVENT_TYPE_AP_CONNECT) {
        if (da_state.ap_connected == DA_STATE_KNOWN_FALSE) {
            // AP_CONNECT + not connected = disconnected or failed to connect

            // If active was DA, or we were switching to the DA
            // then we need to switch to the LTE
            if ((g_active_radio == COMM_DEVICE_DA16200)
                || (g_switching_radios && switch_radios_info.target_radio == COMM_DEVICE_DA16200)) {
                LOG_DBG("AP failed to connect or disconnected, switch to LTE");
                ret = rm_switch_to(COMM_DEVICE_NRF9160, true, false);    // active was da, but it is gone as of now
                if (ret != 0) {
                    LOG_ERR("Failed to switch to %s: '%s'(%d)", comm_dev_str(COMM_DEVICE_NRF9160), wstrerr(-ret), ret);
                }
            }
            return_da_to_sleep();
        } else {
            // We just connected to an AP
            // We have changed the logic to not have to react to a connection
            // that happened automatically.  We only initiate connections, so
            // no reason to do anything here
        }
    }

    if (work_info->msg.evt.events & DA_EVENT_TYPE_MQTT_ENABLED) {
        if (da_state.mqtt_enabled == DA_STATE_KNOWN_FALSE) {
            if (g_active_radio == COMM_DEVICE_DA16200) {
                LOG_DBG(
                    "Wifi was the active radio when mqtt was disabled, active "
                    "is now None. Switch to LTE");
                rm_switch_to(COMM_DEVICE_NRF9160, false, false);
            }
        }
    }

    if (work_info->msg.evt.events & DA_EVENT_TYPE_MQTT_BROKER_CONNECT) {
        if (da_state.mqtt_broker_connected == DA_STATE_KNOWN_TRUE) {
            if (g_active_radio == COMM_DEVICE_NRF9160 && rm_wifi_is_connecting() == false) {
                LOG_ERR(
                    "LTE was the active radio when WIFI mqtt broker connected, "
                    "rebooting DA.");
                wifi_reset();
            }
        }
    }
free_and_exit:
    k_free(work_info);
    wr_put(wr);
}

static void rm_9160_work_handler(struct k_work *item)
{
    workref_t        *wr        = CONTAINER_OF(item, workref_t, work);
    zbus_work_info_t *work_info = (zbus_work_info_t *)wr->reference;
    if (g_radio_mgmt == false) {
        LOG_DBG("Radio mgmr off, Skipping 9160 status update");
        goto free_and_exit_9160;
    }

    // unlike the Wifi, if the LTE is disconnected or the MQTT is disconnected, we don't
    // automatically switch to the wifi.  Instead we keep trying to use LTE, however
    // error prone, until we are told not to or a Wifi AP is detected and connected to.

    // One exception is when there is no active radio and the nrf9160 connects to a cell tower
    if (work_info->msg.modem_status.change_bits & STATUS_LTE_CONNECTED
        || work_info->msg.modem_status.change_bits & STATUS_MQTT_CONNECTED) {
        LOG_DBG("LTE connected or MQTT connected");
    }

    if (work_info->msg.modem_status.change_bits & STATUS_MQTT_ENABLED) {
        // MQTT connected status changed
        LOG_WRN(
            "9160 MQTT enabled: %s",
            status_getBit(work_info->msg.modem_status.status.status_flags, UPDATE_STATUS_MQTT_ENABLED) ? "yes" : "no");
        if (!status_getBit(work_info->msg.modem_status.status.status_flags, STATUS_MQTT_ENABLED)) {
            if (rm_get_active_mqtt_radio() == COMM_DEVICE_NRF9160 && !rm_is_switching_radios()) {
                LOG_WRN(
                    "LTE MQTT disabled, LTE is active radio, turning MQTT "
                    "enable back on");
                rm_switch_to(
                    COMM_DEVICE_NRF9160,
                    false,
                    false);    // This will cause the RM to recheck and
                               // re-enable the MQTT
            }
        }
    }

free_and_exit_9160:
    k_free(work_info);
    wr_put(wr);
}

static void usb_bt_connection_work_handler(struct k_work *item)
{
    workref_t        *wr                 = CONTAINER_OF(item, workref_t, work);
    zbus_work_info_t *work_info          = (zbus_work_info_t *)wr->reference;
    static bool       last_usb_connected = false;    // The last known state of the USB connection
    static bool       last_bt_connected  = false;    // The last known state of the BT connection

    // If there was a change in the USB or BT state
    if (last_usb_connected != g_usb_connected || last_bt_connected != g_bt_connected) {
        LOG_DBG("USB connected is now: %d", g_usb_connected);
        LOG_DBG("BT connected is now: %d", g_bt_connected);
        if ((g_usb_connected || g_bt_connected)
            && !g_usb_bt_prepped_radio)    // Either BT or USB is connected and we haven't prepped
        {
            LOG_WRN("USB or BT connected, prepping radio");
            // If we do this at boot it can be several seconds before wifi if free
            int ret = wifi_get_mutex(K_SECONDS(5), __func__);
            if (ret != 0) {
                LOG_ERR("'%s'(%d) getting wifi mutex for usb_bt connection work", wstrerr(-ret), ret);
                wr_put(wr);
                return;
            }
            g_use_sleep = false;
            ret         = wifi_disconnect_from_AP(K_SECONDS(1));
            if (ret != 0) {
                LOG_ERR("'%s'(%d) disconnecting from AP going to battery", wstrerr(-ret), ret);
            }
            da_state.ap_connected  = DA_STATE_KNOWN_FALSE;
            da_state.ap_name[0]    = '\0';
            da_state.ip_address[0] = '\0';
            insure_da_is_awake();
            g_usb_bt_prepped_radio = true;
            wifi_release_mutex();
        }
        if ((!g_usb_connected && !g_bt_connected)
            && g_usb_bt_prepped_radio)    // Neither BT or USB is connected, release the radio if
                                          // we prepped it
        {
            LOG_WRN("USB and BT disconnected, releasing radio");
            // If we do this at boot it can be several seconds before the wifi mutex is
            // free
            int ret = wifi_get_mutex(K_SECONDS(5), "usb_bt_connection_work_handler2");
            if (ret != 0) {
                wr_put(wr);
                LOG_ERR("'%s'(%d) getting wifi mutex for usb_bt connection work", wstrerr(-ret), ret);
                return;
            }
            // This will cause the DA to reboot.  The DA crashes if it is connected to
            // an AP if we change to DPM mode.  So we are going to disconnect it first.
            // We do this regardless of whether we think the DA is connected to an AP
            // because the DA might connected to an old AP if we failed to tell it to
            // forget it
            g_use_sleep = true;
            ret         = wifi_disconnect_from_AP(K_SECONDS(1));
            if (ret != 0) {
                LOG_ERR("'%s'(%d) disconnecting from AP going to battery", wstrerr(-ret), ret);
            }
            da_state.ap_connected  = DA_STATE_KNOWN_FALSE;
            da_state.ap_name[0]    = '\0';
            da_state.ip_address[0] = '\0';
            return_da_to_sleep();
            g_usb_bt_prepped_radio = false;
            wifi_release_mutex();
        }
        last_usb_connected = g_usb_connected;
        last_bt_connected  = g_bt_connected;
    }
    k_free(work_info);
    wr_put(wr);
}

////////////////////////////////////////////////////
// radioMgr_init()
//
//  @return void
void radioMgr_init()
{
    k_work_queue_init(&radioMgr_work_q);
    struct k_work_queue_config radioMgr_work_q_cfg = {
        .name     = "radioMgr_work_q",
        .no_yield = 0,
    };

    k_work_queue_start(
        &radioMgr_work_q, radioMgr_stack_area, K_THREAD_STACK_SIZEOF(radioMgr_stack_area), 5, &radioMgr_work_q_cfg);
    k_work_init(&switch_radios_info.SR_work, switch_radios_work_handler);
    k_work_init(&connecting_to_ap_work, connecting_to_ap_work_handler);
}

void radio_mgr_listener(const struct zbus_channel *chan)
{
    zbus_work_info_t *work_info = k_malloc(sizeof(zbus_work_info_t));
    if (work_info == NULL) {
        LOG_ERR("Failed to allocate memory for zbus_work_info_t");
        return;
    }
    work_info->zbus_work = wr_get(work_info, __LINE__);
    if (work_info->zbus_work == NULL) {
        LOG_ERR("Out of workrefs");
        k_free(work_info);
        return;
    }
    work_info->chan = chan;

    if (&LTE_STATUS_UPDATE == chan) {
        const modem_status_update_t *cstatus = zbus_chan_const_msg(chan);    // Direct message access
        memcpy(&work_info->msg.modem_status, cstatus, sizeof(modem_status_update_t));
        k_work_init(
            &(work_info->zbus_work->work),
            rm_9160_work_handler);    // radioMgr_9160_status_update(&status_update);
    }

    if (&BT_CONN_STATE_UPDATE == chan) {
        const bool *cconnected = zbus_chan_const_msg(chan);    // Direct message access
        g_bt_connected         = *cconnected;
        k_work_init(&(work_info->zbus_work->work), usb_bt_connection_work_handler);
    }

    if (&USB_POWER_STATE_UPDATE == chan) {
        const bool *cconnected = zbus_chan_const_msg(chan);    // Direct message access
        g_usb_connected        = *cconnected;
        k_work_init(&(work_info->zbus_work->work), usb_bt_connection_work_handler);
    }

    if (&da_state_chan == chan) {
        const da_event_t *cevt = zbus_chan_const_msg(chan);    // Direct message access
        memcpy(&work_info->msg.evt, cevt, sizeof(da_event_t));
        k_work_init(&(work_info->zbus_work->work), rm_da_work_handler);
    }

    int ret = k_work_submit_to_queue(&radioMgr_work_q, &(work_info->zbus_work->work));
    if (ret < 0) {
        LOG_ERR("Failed to submit work to radioMgr_work_q: %d", ret);
        wr_put(work_info->zbus_work);
        k_free(work_info);
    }
}

K_THREAD_DEFINE(radio_mgr_task_id, 4092, radioMgr_init, NULL, NULL, NULL, CONFIG_MAIN_THREAD_PRIORITY - 1, 0, 0);

#define ENABLE_PARAMS "[1|on|0|off]"
void do_enable(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 1 && argc != 2) {
        shell_error(sh, "Usage: %s " ENABLE_PARAMS, argv[0]);
        return;
    }
    if (strcmp("off", argv[1]) == 0 || strcmp("0", argv[1]) == 0) {
        g_radio_mgmt = false;
    } else {
        g_radio_mgmt = true;
    }
}

#define CHANGE_RADIO_PARAMS "<0|wifi, 1|lte> [1|on, force, set vars don't do cmds]"
void do_change_radio(const struct shell *sh, size_t argc, char **argv)
{
    bool force = false;
    if (argc != 2 && argc != 3) {
        shell_error(sh, "Usage: %s " CHANGE_RADIO_PARAMS, argv[0]);
        return;
    }
    if (argc == 3 && (strcmp("1", argv[2]) == 0 || strcmp("on", argv[2]) == 0)) {
        force = true;
    }

    switch (argv[1][0]) {
    case '0':
    case 'w':
    case 'W':
        shell_print(sh, "Switching to wifi");
        rm_switch_to(COMM_DEVICE_DA16200, false, force);
        break;
    case '1':
    case 'l':
    case 'L':
        shell_print(sh, "Switching to lte");
        rm_switch_to(COMM_DEVICE_NRF9160, false, force);
        break;
    default:
        shell_error(sh, "Invalid radio");
        return;
    }
}

#define STEP_PARAMS ""
void do_step(const struct shell *sh, size_t argc, char **argv)
{
    if (g_radio_mgmt) {
        shell_error(sh, "RadioMgr is running so we can't step the switch state machine");
        return;
    }
    g_do_one_step = true;
    queue_switching_handler(NULL);    // If it does a step it will set g_do_one_step to false
}

#define STATUS_PARAMS ""
void do_status(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Radio Manager %s", g_radio_mgmt ? "Enabled" : "Disabled");
    shell_print(sh, "using sleep: %s", g_use_sleep ? "yes" : "no");
    shell_print(sh, "using wifi: %s", g_use_wifi ? "yes" : "no");
    shell_print(sh, "Active Radio: %s", comm_dev_str(rm_get_active_mqtt_radio()));
    shell_print(sh, "MQTT Connected: %d", rm_ready_for_mqtt());
    shell_print(sh, "In the middle of switching radios: %d", g_switching_radios);
    shell_print(sh, "Current Switching Operation: %s", rm_op_str());
    shell_print(
        sh,
        "da_state: In DPM Mode: %s - Is Sleeping: %s - AP connected: %s",
        da_state.dpm_mode ? "true" : "false",
        da_state.is_sleeping ? "true" : "false",
        da_state.ap_connected ? "true" : "false");
}

#define USE_SLEEP_PARAMS "<on|1|off|0>"
void do_use_sleep(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 2) {
        shell_error(sh, "Usage: %s " USE_SLEEP_PARAMS, argv[0]);
        return;
    }
    if (strcmp("on", argv[1]) == 0 || strcmp("1", argv[1]) == 0) {
        g_use_sleep = true;
    } else {
        g_use_sleep = false;
    }
}


#define USE_WIFI_PARAMS "<on|1|off|0>"
void do_use_wifi(const struct shell *sh, size_t argc, char **argv)
{
    int err = 0;
    if (argc != 2) {
        shell_error(sh, "Usage: %s " USE_WIFI_PARAMS, argv[0]);
        return;
    }
    bool use_wifi = shell_strtobool(argv[1], 10, &err);
    rm_wifi_enable(use_wifi);
}


SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_radioMgr,
    SHELL_CMD(enable, NULL, "Enable or disable radio management. " ENABLE_PARAMS, do_enable),
    SHELL_CMD(change_radio, NULL, "change the active radios. " CHANGE_RADIO_PARAMS, do_change_radio),
    SHELL_CMD(one_step, NULL, "Do one call to the RM switch state machine. " STEP_PARAMS, do_step),
    SHELL_CMD(status, NULL, "Show radio manager status. " STATUS_PARAMS, do_status),
    SHELL_CMD(use_sleep, NULL, "Set if the radio mgr puts the da to sleep when not in use. " USE_SLEEP_PARAMS, do_use_sleep),
    SHELL_CMD(
        use_wifi,
        NULL,
        "Set if the radio mgr will switch to wifi or use the DA for ssid scans. " USE_WIFI_PARAMS,
        do_use_wifi),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(radioMgr, &sub_radioMgr, "Commands to control the Radio Manager", NULL);
