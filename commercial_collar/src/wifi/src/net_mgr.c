/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
/****************************************************************
 *  net_mgr.c
 *  SPDX-License-Identifier: LicenseRef-Proprietary
 *
 * The wifi code is layered as follows:
 *     wifi_spi.c or wifi_uart.c - Talked to hardware to transfer data to and from the DA1200
 *     wifi.c - HW independent layer to send and receive messages to and from the DA1200
 *     wifi_at.c - AT command layer to send and receive AT commands to and from the DA1200
 *     net_mgr.c - The network (wifi and lte) api layer, manages the state of the da and
 *                 lte chip and network communication state machine and publishes zbus
 *                 message when revevent stuff happens
 *     wifi_shell.c - A collection of shell commands to test and debug during development
 */
#include "wifi.h"
#include "wifi_at.h"
#include "net_mgr.h"
#include <zephyr/zbus/zbus.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include "pmic.h"
#include "d1_zbus.h"
#include "d1_time.h"
#include "fota.h"
#include "commMgr.h"
#include "radioMgr.h"
#include <ctype.h>
#include <cJSON_os.h>
#include <strings.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>

LOG_MODULE_REGISTER(net_mgr, CONFIG_NET_MGR_LOG_LEVEL);

static void net_monitor_DA(wifi_msg_t *msg, void *user_data);
int         insure_uicr_backup();
int         cmpMACs(char *mac1, char *mac2);

void wifi_at_http_write(wifi_msg_t *msg);
void wifi_at_http_status(wifi_msg_t *msg);

K_THREAD_STACK_DEFINE(net_stack, 4096);
void watcher_listener(const struct zbus_channel *chan);
ZBUS_LISTENER_DEFINE(watch_l, watcher_listener);

ZBUS_CHAN_DEFINE(
    da_state_chan,                                        /* Name */
    da_event_t,                                           /* Message type */
    NULL,                                                 /* Validator */
    NULL,                                                 /* User Data */
    ZBUS_OBSERVERS(watch_l, comm_mgr_sub, radio_mgr_sub), /* observers */
    ZBUS_MSG_INIT(0)                                      /* Initial value {0} */
);

// g_rebooting_da start as true and gets reset to true each time we power the DA
// on from a known off state.  This is used to know if the INIT:DONE messages we
// just got it from a power on or response to a wakeup pulse
volatile bool   g_rebooting_da = true;
bool            g_show_events  = false;
char            ota_last_start_result[10];
wifi_saved_ap_t g_last_conn_attempt;
char            g_last_ap_name[33];

static struct k_work_q net_mgr_work_q;

static void net_do_da_init_work_fn();
K_WORK_DEFINE(net_do_da_init_work, net_do_da_init_work_fn);
static void queue_da_init_work()
{
    int ret = k_work_submit_to_queue(&net_mgr_work_q, &net_do_da_init_work);
    if (ret <= 0) {
        LOG_ERR("Failed to queue init work: %d", ret);
    }
}

static void net_process_WAKEUP_fn();
K_WORK_DEFINE(net_process_WAKEUP_work, net_process_WAKEUP_fn);
static void queue_WAKEUP_work()
{
    int ret = k_work_submit_to_queue(&net_mgr_work_q, &net_process_WAKEUP_work);
    if (ret <= 0) {
        LOG_ERR("Failed to queue wakeup work: %d", ret);
    }
}

static void net_reset_da_fn();
K_WORK_DEFINE(net_reset_da_work, net_reset_da_fn);
static void queue_reset_da_work()
{
    int ret = k_work_submit_to_queue(&net_mgr_work_q, &net_reset_da_work);
    if (ret <= 0) {
        LOG_ERR("Failed to queue reset work: %d", ret);
    }
}

static void net_disconn_work_fn();
K_WORK_DEFINE(net_disconn_work, net_disconn_work_fn);
static void queue_disconn_work()
{
    int ret = k_work_submit_to_queue(&net_mgr_work_q, &net_disconn_work);
    if (ret <= 0) {
        LOG_ERR("Failed to queue disconn work: %d", ret);
    }
}

static void net_get_time_work_fn();
K_WORK_DEFINE(net_get_time_work, net_get_time_work_fn);
static void queue_get_time_work()
{
    if (!is_5340_time_set()) {
        int ret = k_work_submit_to_queue(&net_mgr_work_q, &net_get_time_work);
        if (ret <= 0) {
            LOG_ERR("Failed to queue time work: %d", ret);
        }
    }
}

static void watch_work_fn(struct k_work *work);
static void queue_watch_work(sa_event_watch_item_t *item)
{
    int ret = k_work_submit_to_queue(&net_mgr_work_q, &item->work);
    if (ret <= 0) {
        LOG_ERR("Failed to queue watch work: %d", ret);
    }
}

#define URL_BUF_LEN (MAX_URL_LEN + 40)
typedef struct ota_info_t
{
    struct k_work work;
    char          url_buf[URL_BUF_LEN];
    uint8_t       expected_version[3];
    uint8_t       cancel;
    uint8_t       last;
    uint8_t       reboot_cnt;
    bool          download_complete;
    char          errtxt[50];
    int           state;    // The OTA is small state machine
} ota_info_t;
ota_info_t  my_ota_info;
static void net_ota_fn();
static void queue_ota_work()
{
    int ret = k_work_submit_to_queue(&net_mgr_work_q, &my_ota_info.work);
    if (ret <= 0) {
        LOG_ERR("Failed to queue OTA work: %d", ret);
    }
}

void ota_work_timer_handler(struct k_timer *dummy);
K_TIMER_DEFINE(ota_work_timer, ota_work_timer_handler, NULL);

// This holds the state of the DA and is used to decide what actions
// can be taken by 5340 code without having to query the DA.
da_state_t da_state = { .initialized           = DA_STATE_KNOWN_FALSE,
                        .rtc_wake_time         = 0,
                        .ap_connected          = DA_STATE_KNOWN_FALSE,
                        .ap_name               = "",
                        .ap_disconnect_reason  = "",
                        .ip_address            = "",
                        .dpm_mode              = DA_STATE_UNKNOWN,
                        .is_sleeping           = DA_STATE_UNKNOWN,
                        .mqtt_enabled          = DA_STATE_UNKNOWN,
                        .mqtt_broker_connected = DA_STATE_UNKNOWN,
                        .mqtt_last_msg_time    = 0,
                        .mqtt_certs_installed  = DA_STATE_UNKNOWN,
                        .ntp_server_set        = DA_STATE_UNKNOWN,
                        .dhcp_client_name_set  = DA_STATE_UNKNOWN,
                        .dhcp_client_name      = "",
                        .mqtt_sub_topic_count  = 0,
                        .mqtt_sub_topics       = { "", "", "", "", "", "", "", "", "" },
                        .mqtt_client_id        = "",
                        .uicr_bu_status        = DA_BU_UNKNOWN,
                        .uicr_bu               = { 0, 0, 0, 0, 0, 0, 0, 0 },
                        .mac_set               = DA_STATE_UNKNOWN,
                        .xtal_set              = DA_STATE_UNKNOWN,
                        .onboarded             = DA_STATE_UNKNOWN,
                        .mqtt_on_boot          = DA_STATE_UNKNOWN,
                        .ota_progress          = DA_STATE_UNKNOWN,
                        .reboot_cnt            = 0,
                        .version               = { 0, 0, 0 },
                        .ap_profile_disabled   = DA_STATE_UNKNOWN,
                        .powered_on            = DA_STATE_KNOWN_TRUE,
                        .rssi                  = RSSI_NOT_CONNECTED };

#if IS_ENABLED(CONFIG_SAFE_LOG)
char *safe_log_str(char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(log_safe_str, CONFIG_MAX_TELEMETRY_LOG_MSG_SIZE, fmt, args);
    for (int ig = 0; ig < CONFIG_MAX_TELEMETRY_LOG_MSG_SIZE; ig++) {
        if ((log_safe_str[ig] < ' ' || log_safe_str[ig] == 0x80)
            && !(
                log_safe_str[ig] == '\r' || log_safe_str[ig] == '\n' || log_safe_str[ig] == 0
                || log_safe_str[ig] == '\x1b')) {
            log_safe_str[ig] = '_';
        }
    }
    va_end(args);
    return log_safe_str;
}
#endif

void send_zbus_tri_event(uint32_t event, das_tri_state_t new, das_tri_state_t *var)
{
    das_tri_state_t old = *var;
    // If the state didn't change, don't send an event
    if (old == new) {
        return;
    }
    *var = new;    // change the state first

    // Send the event about the change
    da_event_t evt = { .events = event, .timestamp = k_uptime_get() };
    evt.old.tri    = old;
    evt.new.tri    = new;
    int zret       = zbus_chan_pub(&da_state_chan, &evt, K_MSEC(100));
    if (zret != 0) {
        LOG_ERR("'%s'(%d) when publishing tri evt: %d from, %d to %d", wstrerr(-zret), zret, event, old, new);
    }
}

void send_zbus_int_event(uint32_t event, int new, int *var, bool force)
{
    int old = *var;
    // If the state didn't change, don't send an event
    if (!force && old == new) {
        return;
    }

    *var = new;    // change the state first

    // Send the event about the change
    da_event_t evt = { .events = event, .timestamp = k_uptime_get() };
    evt.old.theInt = old;
    evt.new.theInt = new;
    int zret       = zbus_chan_pub(&da_state_chan, &evt, K_MSEC(100));
    if (zret != 0) {
        LOG_ERR("'%s'(%d) when publishing int evt: %d from, %d to %d", wstrerr(-zret), zret, event, old, new);
    }
}

void send_zbus_bu_event(das_bu_status_t new)
{
    da_event_t evt = { .events = DA_EVENT_TYPE_UICR_BU_STATUS, .timestamp = k_uptime_get() };
    // If the state didn't change, don't send an event
    if (da_state.uicr_bu_status == new) {
        return;
    }

    evt.old.uicr_bu_status  = da_state.uicr_bu_status;
    da_state.uicr_bu_status = new;    // change the state first

    // Send the event about the change
    evt.new.uicr_bu_status = new;
    int zret               = zbus_chan_pub(&da_state_chan, &evt, K_MSEC(100));
    if (zret != 0) {
        LOG_ERR("'%s'(%d) when publishing backup evt from, %d to %d", wstrerr(-zret), zret, evt.old.uicr_bu_status, new);
    }
}

void send_zbus_ota_event(uint32_t event, da_ota_progress_t new, da_ota_progress_t *var)
{
    da_ota_progress_t old = *var;
    // If the state didn't change, don't send an event
    if (old == new) {
        return;
    }

    *var = new;    // change the state first

    // Send the event about the change
    da_event_t evt       = { .events = event, .timestamp = k_uptime_get() };
    evt.old.ota_progress = old;
    evt.new.ota_progress = new;
    int zret             = zbus_chan_pub(&da_state_chan, &evt, K_MSEC(100));
    if (zret != 0) {
        LOG_ERR("'%s'(%d) when publishing ota evt: %d from, %d to %d", wstrerr(-zret), zret, event, old, new);
    }
}

void send_zbus_version_event(da_version_t ver)
{
    bool differs = (da_state.version[0] != ver[0] || da_state.version[1] != ver[1] || da_state.version[2] != ver[2]);
    if (!differs) {
        return;
    }

    // Send the event about the change
    da_event_t evt = { .events = DA_EVENT_TYPE_VERSION, .timestamp = k_uptime_get() };
    evt.old.ver[0] = da_state.version[0];
    evt.old.ver[1] = da_state.version[1];
    evt.old.ver[2] = da_state.version[2];

    evt.new.ver[0] = shadow_doc.wifiVer[0] = da_state.version[0] = ver[0];
    evt.new.ver[1] = shadow_doc.wifiVer[1] = da_state.version[1] = ver[1];
    evt.new.ver[2] = shadow_doc.wifiVer[2] = da_state.version[2] = ver[2];
    int zret                                                     = zbus_chan_pub(&da_state_chan, &evt, K_MSEC(100));
    if (zret != 0) {
        LOG_ERR(
            "'%s'(%d) when publishing version evt from, %d.%d.%d to %d.%d.%d",
            wstrerr(-zret),
            zret,
            evt.old.ver[0],
            evt.old.ver[1],
            evt.old.ver[2],
            evt.new.ver[0],
            evt.new.ver[1],
            evt.new.ver[2]);
    }
}

void send_zbus_timestamp_event(uint32_t event, uint64_t new, uint64_t *var)
{
    // Need to insure that the uint64_t's are aligned properly and there is not guarentee
    // that a point to one is
    uint64_t old;
    memcpy(&old, var, sizeof(uint64_t));

    if (old == new) {
        return;
    }

    memcpy(var, &new, sizeof(uint64_t));    // change the state first

    // Send the event about the change
    da_event_t evt    = { .events = event, .timestamp = k_uptime_get() };
    evt.old.timestamp = old;
    evt.new.timestamp = new;
    int zret          = zbus_chan_pub(&da_state_chan, &evt, K_MSEC(100));
    if (zret != 0) {
        LOG_ERR("'%s'(%d) when publishing timestamp evt: %d from, %lld to %lld", wstrerr(-zret), zret, event, old, new);
    }
}

void send_zbus_string_event(uint32_t event, char *new, char *var, int maxsize)
{
    strncpy(var, new, maxsize);    // change the state first

    // Send the event about the change
    da_event_t evt  = { .events = event, .timestamp = k_uptime_get() };
    int        zret = zbus_chan_pub(&da_state_chan, &evt, K_MSEC(100));
    if (zret != 0) {
        LOG_ERR("'%s'(%d) when publishing string evt %d: to %s", wstrerr(-zret), zret, event, new);
    }
}

////////////////////////////////////////////////////////////////////
// net_mgr_init()
//
// Initialize the net application layer
//
// @return - 0 on success, -1 on error
int net_mgr_init()
{
    // Once we are communicating to the DA or the 9160 via SPI, they
    // notifies us when their state changes such as when they reboots,
    // or goes to sleep, wakes up, connects to an AP, etc.
    // We want to monitor the message we get and keep track of the
    // state of the DA so we can easily check if we can do a
    // function or need to do a function again
    wifi_add_tx_rx_cb(net_monitor_DA, NULL);

    k_work_init(&(my_ota_info.work), net_ota_fn);
    // Start a work thread to process net_mgr work
    struct k_work_queue_config cfg;
    cfg.no_yield = 1;
    cfg.name     = "net_mgr";
    k_work_queue_start(
        &net_mgr_work_q, net_stack, K_THREAD_STACK_SIZEOF(net_stack), CONFIG_SYSTEM_WORKQUEUE_PRIORITY, &cfg);

    send_zbus_tri_event(DA_EVENT_TYPE_WIFI_INIT, DA_STATE_KNOWN_TRUE, &(da_state.initialized));

    return 0;
}

///////////////////////////////////////////////////
// tristate_str()
// Get the name of the comm device type
// device: comm device type
// returns: name of the comm device type
const char *tristate_str(das_tri_state_t val)
{
    switch (val) {
    case DA_STATE_UNKNOWN:
        return "?";
    case DA_STATE_KNOWN_FALSE:
        return "F";
    case DA_STATE_KNOWN_TRUE:
        return "T";
    default:
        return "X";
    }
}

void net_mgr_wifi_power(bool powered)
{
    bool is_powered = wifi_get_power_key();
    if (powered && !is_powered) {
        LOG_WRN("Powering ON the wifi chip");
        wifi_set_power_key(1);
        g_rebooting_da = true;
        send_zbus_tri_event(DA_EVENT_TYPE_POWERED_ON, DA_STATE_KNOWN_TRUE, &(da_state.powered_on));
        k_sleep(K_MSEC(1500));    // Give it time to boot
    }
    if (!powered && is_powered) {
        LOG_ERR("Powering OFF the wifi chip");
        wifi_set_power_key(0);
        g_DA_needs_one_time_config = true;
        send_zbus_tri_event(DA_EVENT_TYPE_POWERED_ON, DA_STATE_KNOWN_FALSE, &(da_state.powered_on));

        if (da_state.ap_connected == DA_STATE_KNOWN_TRUE) {
            strncpy(g_last_ap_name, da_state.ap_name, 32);
            g_last_ap_name[32] = 0;
        }
        da_state.ap_name[0]    = 0;
        da_state.ip_address[0] = 0;
        send_zbus_tri_event(DA_EVENT_TYPE_AP_CONNECT, DA_STATE_KNOWN_FALSE, &(da_state.ap_connected));
        send_zbus_tri_event(DA_EVENT_TYPE_AP_SAFE, DA_STATE_UNKNOWN, &(da_state.ap_safe));
        send_zbus_tri_event(DA_EVENT_TYPE_IS_SLEEPING, DA_STATE_KNOWN_TRUE, &(da_state.is_sleeping));
        send_zbus_tri_event(DA_EVENT_TYPE_MQTT_ENABLED, DA_STATE_KNOWN_FALSE, &(da_state.mqtt_enabled));
        send_zbus_tri_event(DA_EVENT_TYPE_MQTT_BROKER_CONNECT, DA_STATE_KNOWN_FALSE, &(da_state.mqtt_broker_connected));
        da_state.dhcp_client_name[0] = 0;
        send_zbus_tri_event(DA_EVENT_TYPE_DHCP_CLIENT_NAME_SET, DA_STATE_UNKNOWN, &(da_state.dhcp_client_name_set));
        for (int i = 0; i < CONFIG_IOT_MAX_TOPIC_NUM; i++) {
            da_state.mqtt_sub_topics[i][0] = 0;
        }
        send_zbus_int_event(DA_EVENT_TYPE_MQTT_SUB_TOP_CHANGED, 0, &(da_state.mqtt_sub_topic_count), false);
        send_zbus_tri_event(DA_EVENT_TYPE_MAC_SET, DA_STATE_UNKNOWN, &(da_state.mac_set));
        send_zbus_tri_event(DA_EVENT_TYPE_XTAL_SET, DA_STATE_UNKNOWN, &(da_state.xtal_set));
        send_zbus_ota_event(DA_EVENT_TYPE_OTA_PROGRESS, DA_OTA_PROGRESS_NO_OTA, &(da_state.ota_progress));
        send_zbus_tri_event(DA_EVENT_TYPE_AP_PROFILE_USE, DA_STATE_UNKNOWN, &(da_state.ap_profile_disabled));
    }
}


//////////////////////////////////////////////////////////
// net_handle_DA_Init()
//
// This is called when the DA sends us a \r\n+INIT:
// I extended this message in the DONE case to
//+INIT:DONE,0,DPM=1
void net_handle_DA_Init(wifi_msg_t *msg, char *msgPtr)
{
    char *subtype = msgPtr + 8;

    if (!uicr_in_factory_flag_get() && !uicr_shipping_flag_get()) {
        queue_da_init_work();
        return;
    }

    if (strncmp(subtype, "DONE", 4) == 0) {
        das_tri_state_t tri      = DA_STATE_UNKNOWN;
        char           *dpmstate = strstr(subtype, ",DPM=");
        if (dpmstate != NULL) {
            // We were sent a DPM state, set the initial DPM state so we don't
            // send it a AT+DPM=x which will reboot the DA even if it is already
            // in that state.
            if (strncmp(dpmstate, ",DPM=0", 6) == 0) {
                tri = DA_STATE_KNOWN_FALSE;
            } else if (strncmp(dpmstate, ",DPM=1", 6) == 0) {
                tri = DA_STATE_KNOWN_TRUE;
            }
            send_zbus_tri_event(DA_EVENT_TYPE_DPM_MODE, tri, &(da_state.dpm_mode));
        }

        if (my_ota_info.state != 4 && g_rebooting_da == false) {
            // The DA may send us a +INIT:DONE other then on boot, so if this isn't the
            // "power on" boot stop here
            send_zbus_tri_event(DA_EVENT_TYPE_IS_SLEEPING, DA_STATE_KNOWN_FALSE, &(da_state.is_sleeping));
            return;
        }
        // The powered on code waits for this to go false and expects
        // that the DPM state will be set properly via the
        // INIT:DONE,DPM= message parsed above, don't change
        // g_rebooting_da until that parsing and da_state.dpm_mode is set
        g_rebooting_da = false;

        // Kick off the work to configure the DA the first time it boots
        queue_da_init_work();

        tri = DA_STATE_UNKNOWN;
        send_zbus_tri_event(DA_EVENT_TYPE_DA_RESTARTED, DA_STATE_KNOWN_TRUE, &(tri));

        send_zbus_int_event(DA_EVENT_TYPE_REBOOT_CNT, da_state.reboot_cnt + 1, &(da_state.reboot_cnt), false);

        // A restart means we are no longer connected
        if (da_state.ap_connected == DA_STATE_KNOWN_TRUE) {
            strncpy(g_last_ap_name, da_state.ap_name, 32);
            g_last_ap_name[32] = 0;
        }
        da_state.ap_name[0]    = 0;
        da_state.ip_address[0] = 0;
        send_zbus_tri_event(DA_EVENT_TYPE_AP_CONNECT, DA_STATE_KNOWN_FALSE, &(da_state.ap_connected));
        send_zbus_tri_event(DA_EVENT_TYPE_AP_SAFE, DA_STATE_UNKNOWN, &(da_state.ap_safe));

        // The DA isn't sleeping at the moment
        send_zbus_tri_event(DA_EVENT_TYPE_IS_SLEEPING, DA_STATE_KNOWN_FALSE, &(da_state.is_sleeping));

        // The MQTT broker is not connected at the moment
        send_zbus_tri_event(DA_EVENT_TYPE_MQTT_BROKER_CONNECT, DA_STATE_KNOWN_FALSE, &(da_state.mqtt_broker_connected));

    } else if (strncmp(subtype, "WAKEUP,", 7) == 0) {
        char *waketype = subtype += 7;

        queue_WAKEUP_work();
        send_zbus_tri_event(DA_EVENT_TYPE_DPM_MODE, DA_STATE_KNOWN_TRUE, &(da_state.dpm_mode));

        // Upon getting a WAKEUP we are not sleeping anymore, but there
        // is no message sent when it goes back to sleep. So we don't
        // set the non-sleeping state here.  We assume it will go back
        // to sleep unless some code stops it and that code should set
        // the sleeping state
        if ((strncmp(waketype, "DEAUTH", 6) == 0) || (strncmp(waketype, "NOBCN", 5) == 0)) {
            if (da_state.ap_connected == DA_STATE_KNOWN_TRUE) {
                strncpy(g_last_ap_name, da_state.ap_name, 32);
                g_last_ap_name[32] = 0;
            }
            da_state.ap_name[0]    = 0;
            da_state.ip_address[0] = 0;
            send_zbus_tri_event(DA_EVENT_TYPE_AP_CONNECT, DA_STATE_KNOWN_FALSE, &(da_state.ap_connected));
            send_zbus_tri_event(DA_EVENT_TYPE_AP_SAFE, DA_STATE_UNKNOWN, &(da_state.ap_safe));
            send_zbus_tri_event(
                DA_EVENT_TYPE_MQTT_BROKER_CONNECT, DA_STATE_KNOWN_FALSE, &(da_state.mqtt_broker_connected));
        }
    } else {
        LOG_ERR("Unknown DA Init subtype: %s", subtype);
    }
}

//////////////////////////////////////////////////////////
// net_process_WAKEUP_fn()
//	This is called when the DA tells us it woke up .
// We let the DA know that we woke up with a MCUWUDONE msg.
//
// It is possible that we don't get to this function fast
// enough and the DA goes back to sleep.  If that happens
// we need to report that something is delaying the work q.
static void net_process_WAKEUP_fn()
{
    if (wifi_get_mutex(K_MSEC(3000), __func__) != 0) {
        return;
    }

    das_tri_state_t old  = da_state.is_sleeping;
    da_state.is_sleeping = DA_STATE_KNOWN_FALSE;    // We will notify soon, but need this to be off now
    int ret              = wifi_send_ok_err_atcmd("AT+MCUWUDONE", NULL, K_MSEC(80));
    if (ret < 0) {
        LOG_ERR(
            "'%s'(%d) doing MCUWUDONE after a +INIT:WAKEUP,UC. we may not have responded fast enough, msg lost",
            wstrerr(-ret),
            ret);
        wifi_release_mutex();
        return;
    }
    ret = wifi_send_ok_err_atcmd("AT+CLRDPMSLPEXT", NULL, K_MSEC(50));
    if (ret < 0) {
        LOG_ERR(
            "'%s'(%d) doing CLRDPMSLPEXT after a +INIT:WAKEUP,UC. we may not have responded fast enough, msg lost",
            wstrerr(-ret),
            ret);
        wifi_release_mutex();
        return;
    }
    da_state.is_sleeping = old;
    send_zbus_tri_event(DA_EVENT_TYPE_IS_SLEEPING, DA_STATE_KNOWN_FALSE, &(da_state.is_sleeping));
    wifi_release_mutex();
    rm_got_UC_from_AP();
    k_sleep(K_MSEC(500));    // Give time for the message to come in and be processed
                             // if the result of the procesing is that the rm_prepare_radio_for_use()
                             // is called then the DA will stay awake, so we only need to keep this
                             // awake for a short time
    rm_done_with_radio(COMM_DEVICE_DA16200);
    // At this point the DA should send us the packet and then
    // go back to sleep.
}

//////////////////////////////////////////////////////////
// net_handle_DA_APConn()
//
// This is called when the DA tells us it connected to an
// AP via a \r\n+WFJAP:1
void net_handle_DA_APConn(wifi_msg_t *msg, char *msgPtr)
{
    // If the DA is in DPM, it will sleep a few seconds after
    // connecting to an AP.  Since the DA doesn't say when it
    // goes to sleep we kick off a work function to wait a few
    // seconds and then check the DPM state.

    //"\r\n+WFJAP:1,'ProtoSorcery',10.1.91.148\r\n"
    char *apname = strchr(msgPtr, '\'');
    if (apname == NULL) {
        LOG_ERR("Can't find first AP name quote in +WFJAP:1 message");
        return;
    }
    apname++;
    int apnamelen = strcspn(apname, "\'");

    char *ip = apname + apnamelen + 2;    // Skip the quote and comma
    if ((ip - (char *)(msg->data)) >= msg->data_len) {
        LOG_ERR("Can't find IP in +WFJAP:1 message");
        return;
    }
    int iplen = strspn(ip, ".1234567890");
    if (iplen < 8 || iplen > 15) {
        LOG_ERR("IP address size is incorrect in +WFJAP:1 message");
        return;
    }

    if (strncmp(apname, da_state.ap_name, apnamelen) != 0) {
        strncpy(da_state.ap_name, apname, MIN(32, apnamelen));
        da_state.ap_name[MIN(32, apnamelen)] = 0;
        LOG_DBG("AP name changed to %s", da_state.ap_name);
    }

    if (strncmp(ip, da_state.ip_address, iplen) != 0) {
        strncpy(da_state.ip_address, ip, MIN(20, iplen));
        da_state.ip_address[MIN(20, iplen)] = 0;
        LOG_DBG("IP address changed to %s", da_state.ip_address);
    }

    send_zbus_tri_event(DA_EVENT_TYPE_AP_CONNECT, DA_STATE_KNOWN_TRUE, &(da_state.ap_connected));

    int apidx = wifi_find_saved_ssid(da_state.ap_name);
    if (apidx >= 0) {
        bool is_safe = wifi_saved_ssid_safe(apidx);
        send_zbus_tri_event(DA_EVENT_TYPE_AP_SAFE, is_safe, &(da_state.ap_safe));
    } else {
        // we can't know if the AP is safe, so mark it as unsafe and let
        // the code that did the connect decide if it is safe
        send_zbus_tri_event(DA_EVENT_TYPE_AP_SAFE, DA_STATE_KNOWN_FALSE, &(da_state.ap_safe));
    }
}

//////////////////////////////////////////////////////////
// net_start_ota()
//	Start an OTA download and install
//
// @param url - the url to download the OTA from
// @return - 0 on success, -1 on error
int net_start_ota(char *url, uint8_t expected_version[3])
{
    LOG_DBG("Starting OTA download from %s", url);
    if (strlen(url) > MAX_URL_LEN) {
        LOG_ERR("OTA URL too long");
        return -1;
    }
    if (my_ota_info.state != 0) {
        LOG_ERR("OTA already in progress: %d", my_ota_info.state);
        return -2;
    }
    snprintf(my_ota_info.url_buf, URL_BUF_LEN, "AT+NWOTADWSTART=rtos,%s", url);
    my_ota_info.state               = 1;
    my_ota_info.expected_version[0] = expected_version[0];
    my_ota_info.expected_version[1] = expected_version[1];
    my_ota_info.expected_version[2] = expected_version[2];
    k_timer_start(&ota_work_timer, K_NO_WAIT, K_SECONDS(1));
    return 0;
}
//////////////////////////////////////////////////////////
// ota_work_timer_handler()
//	When an OTA is in progress this function is called
// periodically to check the progress of the OTA and
// perform the next step of the OTA
void ota_work_timer_handler(struct k_timer *dummy)
{
    queue_ota_work();
}

//////////////////////////////////////////////////////////
// net_stop_ota()
//	Stop an OTA download
//
// @return - 0 on success, -1 on error
int net_stop_ota()
{
    if (my_ota_info.state == 0) {
        LOG_ERR("no OTA in progress: %d", my_ota_info.state);
        return -1;
    }
    my_ota_info.cancel = 1;
    return 0;
}

static char *ota_resp_code(int code)
{
    switch (code) {
    case 0x00:
        return "Return success.";
    case 0x01:
        return "Return fail.";
    case 0x02:
        return "SFLASH address is wrong.";
    case 0x03:
        return "FW type is unknown.";
    case 0x04:
        return "Server URL is unknown.";
    case 0x05:
        return "FW size is too big.";
    case 0x06:
        return "CRC is not correct.";
    case 0x07:
        return "FW version is unknown.";
    case 0x08:
        return "FW version is incompatible.";
    case 0x09:
        return "FW not found on the server.";
    case 0x0A:
        return "Failed to connect to the server.";
    case 0x0B:
        return "All new FWs have not been downloaded.";
    case 0x0C:
        return "Failed to allocate memory.";
    case 0xA1:
        return "BLE FW version is unknown.";
    }
    return "Unknown error code";
}

static void ota_publish(int fstat, int fper, da_ota_progress_t dpro)
{
    int           zret;
    fota_status_t fevt = { .status = fstat, .percentage = fper, .device_type = COMM_DEVICE_DA16200 };

    send_zbus_ota_event(DA_EVENT_TYPE_OTA_PROGRESS, dpro, &(da_state.ota_progress));

    zret = zbus_chan_pub(&FOTA_STATE_UPDATE, &fevt, K_MSEC(100));
    if (zret != 0) {
        LOG_ERR("Error publishing OTA state change to commMgr: %d", zret);
    }
}

//////////////////////////////////////////////////////////
// net_ota_fn()
// This is called every second when an OTA is in progress.
// It runs a state machine to start, monitor a FW download
// and then install the FW.
static void net_ota_fn(struct k_work *item)
{
    ota_info_t       *ota_info = CONTAINER_OF(item, ota_info_t, work);
    char              progstr[5];
    wifi_wait_array_t wait_msgs;

    // State 0 - Start the download
    if (ota_info->state == 1) {
        if (ota_info->cancel == 1) {
            LOG_INF("OTA cancelled before it got started");
            ota_info->state  = 0;
            ota_info->cancel = 0;
            k_timer_stop(&ota_work_timer);
            return;
        }
        LOG_DBG("Starting OTA");
        ota_publish(1, 0, 0);    // Publish download started

        memset(ota_last_start_result, 0, 10);
        if (wifi_send_ok_err_atcmd(ota_info->url_buf, ota_info->errtxt, K_SECONDS(2)) != 0) {
            // Publish an error
            LOG_ERR("Error starting OTA: %s", ota_info->errtxt);
            ota_publish(2, FOTA_DOWNLOAD_ERROR_CAUSE_DOWNLOAD_FAILED, DA_OTA_ERR_STARTING_DL);
            ota_info->state = 0;
            k_timer_stop(&ota_work_timer);
            return;
        }
        LOG_DBG("OTA Start command accepted");
        ota_info->last  = 0;
        ota_info->state = 2;
        return;
    }

    // State 2 - Wait for the download to complete
    if (ota_info->state == 2) {
        if (ota_last_start_result[0] != 0) {
            if (strncmp(ota_last_start_result, "0x00", 4) == 0) {
                LOG_DBG("OTA file finished downloaded");
                ota_info->download_complete = true;
                ota_info->state             = 3;
                return;
            } else {
                int code = strtoul(ota_last_start_result, NULL, 16);
                LOG_ERR("Error doing OTA: %s (%s)", ota_resp_code(code), ota_last_start_result);
                ota_publish(2, FOTA_DOWNLOAD_ERROR_CAUSE_DOWNLOAD_FAILED, DA_OTA_ERR_STARTING_DL);
                ota_info->state = 0;
                k_timer_stop(&ota_work_timer);
                return;
            }
        }
        if (ota_info->cancel == 1) {
            if (wifi_send_ok_err_atcmd("AT+NWOTADWSTOP", ota_info->errtxt, K_SECONDS(2)) != 0) {
                LOG_ERR("Error stopping OTA: %s", ota_info->errtxt);
                ota_publish(2, FOTA_DOWNLOAD_ERROR_CAUSE_INTERNAL, DA_OTA_ERROR_STOPPING);
                // While we didn't stop the download, we won't be installing it, so its
                // practically stopped
            }
            LOG_DBG("OTA cancelled during download");
            ota_info->state  = 0;
            ota_info->cancel = 0;
            k_timer_stop(&ota_work_timer);
            return;
        }

        wait_msgs.num_msgs = 0;    // Initialize the structure
        wifi_add_wait_msg(&wait_msgs, "\r\nERROR%19s", true, 1, ota_info->errtxt);
        wifi_add_wait_msg(&wait_msgs, "\r\n+NWOTADWPROG:%3s", true, 1, progstr);
        int32_t amount              = 0;
        ota_info->download_complete = false;
        uint64_t now                = k_uptime_get();
        int      ret                = wifi_send_and_wait_for("AT+NWOTADWPROG=rtos", &wait_msgs, K_SECONDS(15));
        uint64_t delta              = k_uptime_get() - now;
        if (delta > 5000) {
            LOG_WRN("Chunk took %llu ms to download", delta);
        }
        if (ret < 0) {
            LOG_ERR("Timed out getting OTA progress");
            ota_publish(2, FOTA_DOWNLOAD_ERROR_CAUSE_DOWNLOAD_FAILED, DA_OTA_TIMEOUT_GETTING_PROGRESS);
            ota_info->state = 0;
            k_timer_stop(&ota_work_timer);
            return;
        }
        if (ret == 0) {    // ERROR
            LOG_ERR("Error out getting OTA progress: %s", ota_info->errtxt);
            ota_publish(2, FOTA_DOWNLOAD_ERROR_CAUSE_DOWNLOAD_FAILED, DA_OTA_TIMEOUT_GETTING_PROGRESS);
            ota_info->state = 0;
            k_timer_stop(&ota_work_timer);
            return;
        }
        if (ret == 1) {    // PROG
            amount = strtoul(progstr, NULL, 10);
            if (errno == ERANGE) {
                LOG_ERR("Error parsing progress string: %s", progstr);
                ota_publish(2, FOTA_DOWNLOAD_ERROR_CAUSE_INTERNAL, DA_OTA_ERROR_PARSING_PROGRESS);
                ota_info->state = 0;
                k_timer_stop(&ota_work_timer);
                return;
            }
            if (amount >= 100) {
                LOG_DBG("OTA file 100%% downloaded");
                ota_info->download_complete = true;
                ota_info->state             = 3;
                return;
            }
            if (amount > ota_info->last + 2) {
                LOG_DBG("OTA file %d%% downloaded", amount);
                ota_publish(1, amount, amount);
            }
            ota_info->last = amount;
            return;
        }
    }

    if (ota_info->state == 3) {
        if (ota_info->cancel == 1) {
            LOG_DBG("OTA cancelled after download");
            ota_info->state  = 0;
            ota_info->cancel = 0;
            k_timer_stop(&ota_work_timer);
            return;
        }

        if (ota_info->download_complete == false) {
            LOG_ERR("OTA DL Ended before complete");
            ota_publish(2, FOTA_DOWNLOAD_ERROR_CAUSE_DOWNLOAD_FAILED, DA_OTA_PROGRESS_STALLED);
            ota_info->state = 0;
            k_timer_stop(&ota_work_timer);
            return;
        }

        LOG_INF("OTA Sending DA a Renew");
        // Because the DA boot immediately after issuing this command, we expect to get an TIMEOUT
        int ret = wifi_send_ok_err_atcmd("AT+NWOTARENEW", ota_info->errtxt, K_MSEC(1000));
        if (ret != 0 && ret != -EAGAIN) {
            LOG_ERR("'%s'(%d) when renewing", wstrerr(-ret), ret);
            ota_publish(2, FOTA_DOWNLOAD_ERROR_CAUSE_INVALID_UPDATE, DA_OTA_ERROR_RENEWING);
            ota_info->state = 0;
            k_timer_stop(&ota_work_timer);
            return;
        }
        ota_publish(1, DA_OTA_DOWNLOAD_COMPLETE, DA_OTA_PROGRESS_REBOOTING);
        ota_info->reboot_cnt = da_state.reboot_cnt;
        ota_info->state      = 4;
    }

    if (ota_info->state == 4) {
        if (da_state.reboot_cnt > ota_info->reboot_cnt) {
            if (ota_info->expected_version[0] != da_state.version[0]
                || ota_info->expected_version[1] != da_state.version[1]
                || ota_info->expected_version[2] != da_state.version[2]) {
                LOG_ERR("OTA failed, version didn't change");
                ota_publish(2, FOTA_DOWNLOAD_ERROR_CAUSE_INVALID_UPDATE, DA_OTA_VERSION_MISMATCH);
                ota_info->state = 0;
                k_timer_stop(&ota_work_timer);
                return;
            }
            LOG_INF("OTA succeeded, version is what is expected after DA restart");
            ota_publish(3, FOTA_DOWNLOAD_ERROR_CAUSE_NO_ERROR, DA_OTA_PROGRESS_NO_OTA);
            ota_info->state = 0;
            k_timer_stop(&ota_work_timer);
            return;
        }
        return;
    }
}

static void net_disconn_work_fn()
{
    if (rm_is_switching_radios()) {
        return;
    }
    das_tri_state_t osleep = da_state.is_sleeping;
    da_state.is_sleeping   = DA_STATE_KNOWN_FALSE;
    int ret                = wifi_send_ok_err_atcmd("AT+WFDIS=1", NULL, K_SECONDS(5));
    da_state.is_sleeping   = osleep;
    if (ret < 0) {
        if (wifi_get_power_key() == true) {
            LOG_ERR("'%s'(%d) disabling the AP profile on disconnect", wstrerr(-ret), ret);
        }
    }
}

static void net_get_time_work_fn()
{
    if (is_5340_time_set()) {
        return;
    }

    int ret = wifi_send_ok_err_atcmd("AT+TIME=?", NULL, K_MSEC(1000));
    if (ret != 0) {
        LOG_ERR("'%s'(%d) getting time", wstrerr(-ret), ret);
    }
}

//////////////////////////////////////////////////////////
// net_handle_DA_APDiscon()
//
// This is called when the DA tells us it disconnected
// from an AP via a
//  \r\n+WFDAP				We intentionally disconnected
//	\r\n+WFJAP:0,NOT_FOUND	We were disconnected by the AP
//	\r\n+WFJAP:0,TIMEOUT	We failed to connect to an AP
//
// These message are delivered to use via a callback so we
// know that the DA is awake at this moment.
void net_handle_DA_APDiscon(wifi_msg_t *msg, char *msgPtr)
{
    char *sub = NULL;
    // If we get a disconnect, unstage the last attempted ap info
    // because it didn't work, and disable the AP profile
    // so it doesn't use it on its next boot
    net_unstage_ssid_for_saving();

    // Remember why we were disconnected
    sub = strstr(msg->data, "\r\n+WFDAP:0");
    if (sub != NULL || (sub = strstr(msg->data, "\r\n+WFJAP:0")) != NULL) {
        char *reason = sub + 10;
        if (strlen(sub + 10) < 1) {
            reason = "DA gave no reason";
        }
        send_zbus_string_event(DA_EVENT_TYPE_DISCONNECT_REASON, sub + 10, da_state.ap_disconnect_reason, 40);
    }

    queue_disconn_work();    // queue work that is too slow for the wifi callback we are in

    send_zbus_tri_event(DA_EVENT_TYPE_IS_SLEEPING, DA_STATE_KNOWN_FALSE, &(da_state.is_sleeping));
    if (da_state.ap_connected) {
        LOG_DBG("DA got disconnected from |%s|.  Reason: %s", da_state.ap_name, msgPtr);
        strncpy(g_last_ap_name, da_state.ap_name, 32);
        g_last_ap_name[32] = 0;
    }
    da_state.ap_name[0]    = 0;
    da_state.ip_address[0] = 0;
    send_zbus_tri_event(DA_EVENT_TYPE_AP_CONNECT, DA_STATE_KNOWN_FALSE, &(da_state.ap_connected));
    send_zbus_tri_event(DA_EVENT_TYPE_AP_SAFE, DA_STATE_KNOWN_FALSE, &(da_state.ap_safe));
    send_zbus_int_event(DA_EVENT_TYPE_RSSI, RSSI_NOT_CONNECTED, &(da_state.rssi), false);
}

//////////////////////////////////////////////////////////
// net_handle_DA_SubTopic()
//
// This is called when the DA tells us the mqtt sub topics
// have been set via a
// \r\n+NWMQTS:1,”da16k_sub”,"da16k_sub2"
//
void net_handle_DA_SubTopic(wifi_msg_t *msg, char *msgPtr)
{
    char *sub;
    char *numTopics      = msgPtr + 10;
    int   num_topics     = atoi(numTopics);    // Max DA_NUMBER_OF_SUB_TOPICS
    bool  topics_changed = false;

    if (num_topics > CONFIG_IOT_MAX_TOPIC_NUM) {
        LOG_ERR("DA sent more MQTT Sub topics then expected");
        return;
    }
    sub = numTopics + 2;    // Skip the number and the comma

    if (da_state.mqtt_sub_topic_count != num_topics) {
        topics_changed = true;
    }

    for (int t = 0; t < CONFIG_IOT_MAX_TOPIC_NUM; t++) {
        if (t >= num_topics) {
            if (da_state.mqtt_sub_topics[t][0] != 0) {
                topics_changed                 = true;
                da_state.mqtt_sub_topics[t][0] = 0;
            }
            continue;
        }

        if ((sub = strchr(sub, '"')) == NULL) {
            LOG_ERR("Can't find first quote of topic %d in +NWMQTS: message", t);
            return;
        }
        sub++;    // skip the quote
        int sublen = strcspn(sub, "\"");
        if (strncmp(sub, da_state.mqtt_sub_topics[t], sublen) != 0) {
            strncpy(da_state.mqtt_sub_topics[t], sub, MIN(sublen, CONFIG_IOT_MAX_TOPIC_LENGTH + 1));
            da_state.mqtt_sub_topics[t][MIN(sublen, CONFIG_IOT_MAX_TOPIC_LENGTH + 1)] = 0;
            topics_changed                                                            = true;
        }
        sub += sublen + 1;
    }

    if (topics_changed) {
        // We know somethign changed so make sure that the event is sent
        // even if the number of topics didn't change
        if (da_state.mqtt_sub_topic_count == num_topics) {
            da_state.mqtt_sub_topic_count = num_topics + 1;
        }
        send_zbus_int_event(DA_EVENT_TYPE_MQTT_SUB_TOP_CHANGED, num_topics, &(da_state.mqtt_sub_topic_count), true);
    }
}

//////////////////////////////////////////////////////////
// net_handle_DA_MQTT_Msg()
//
// This is called when the DA tells us it got a MQTT message
// \r\n+NWMQMSG:Hello world!!!!,da16k_sub,15
//
void net_handle_DA_MQTT_Msg(wifi_msg_t *msg, char *msgPtr)
{
    mqtt_payload_t payload;
    int            i      = 0;
    char          *lenstr = NULL, *topicstr = NULL;

    // Scan from the end to find the embedded length and topic
    for (i = msg->data_len; i > 0; i--) {
        if (msg->data[i] == ',') {
            if (lenstr == NULL) {
                lenstr = msg->data + i + 1;
            } else {
                topicstr = msg->data + i + 1;
                break;
            }
        }
    }
    if (i < 0) {
        char tmp[36];
        strncpy(tmp, msg->data + msg->data_len - 37, 36);
        tmp[36] = 0;
        LOG_ERR("Can't find len and topic, end is %s", tmp);
        return;
    }

    int msglen = atoi(lenstr);
    if (msglen > msg->data_len) {
        LOG_ERR("MQTT RX message length is longer then the message");
        return;
    }

    int topiclen = lenstr - topicstr - 1;
    if (strncmp(topicstr, "messages/", 9) != 0) {
        LOG_ERR("MQTT RX message topic doesn't start with 'messages/'");
        return;
    }
    // topic is "messages/xx/yy/..." where yy is message type
    char *typestr = strchr(topicstr + 9, '/');
    if (typestr == NULL) {
        LOG_ERR("Can't find message type in +NWMQMSG: %s", topicstr);
        return;
    }
    typestr++;    // Skip the '/'

    // If this is a FOTA message, publish it to the FOTA handler
    payload.topic            = topicstr;
    payload.topic_length     = topiclen;
    payload.payload          = msgPtr + sizeof("\r\n+NWMQMSG:") - 1;
    payload.payload_length   = msglen;
    payload.radio            = COMM_DEVICE_DA16200;
    payload.qos              = 0;
    payload.send_immidiately = false;
    // Make a copy of the message since the copy we have is on the stack
    wifi_msg_t *msgcpy = k_calloc(1, sizeof(wifi_msg_t));
    if (msgcpy == NULL) {
        LOG_ERR("Error allocating memory for MQTT message");
        return;
    }
    memcpy(msgcpy, msg, sizeof(wifi_msg_t));
    payload.user_data = (void *)msgcpy;
    // Put the comma's back
    LOG_DBG("Incrementing packet ref count of %p from %d", msg->data, msg->ref_count);
    wifi_inc_ref_count(msg);
    zbus_chan_pub(&MQTT_CLOUD_TO_DEV_MESSAGE, &payload, K_MSEC(100));
}

//////////////////////////////////////////////////////////
// net_handle_DA_DPM()
//
// This is called when the DA sends us a +DPM msg
//
void net_handle_DA_DPM(wifi_msg_t *msg, char *msgPtr)
{
    if (strstr(msg->data, "\r\n+DPM:1") != NULL) {
        send_zbus_tri_event(DA_EVENT_TYPE_DPM_MODE, DA_STATE_KNOWN_TRUE, &(da_state.dpm_mode));
    } else if (strstr(msg->data, "\r\n+DPM:0") != NULL) {
        send_zbus_tri_event(DA_EVENT_TYPE_DPM_MODE, DA_STATE_KNOWN_FALSE, &(da_state.dpm_mode));
    } else if (strstr(msg->data, "\r\n+DPM_ABNORM_SLEEP") != NULL) {
        LOG_DBG("Got Abnormal Sleep");
        send_zbus_tri_event(DA_EVENT_TYPE_DPM_MODE, DA_STATE_KNOWN_TRUE, &(da_state.dpm_mode));
        send_zbus_tri_event(DA_EVENT_TYPE_IS_SLEEPING, DA_STATE_KNOWN_TRUE, &(da_state.is_sleeping));
    }
}

//////////////////////////////////////////////////////////
// net_monitor_DA
//		This is a callback function called by wifi.c when
// a packet is received from the DA.
//
// This manages the shadown state variables and kicks off
// work items based on what happened
static void net_monitor_DA(wifi_msg_t *msg, void *user_data)
{
    char *sub;
    if (msg->incoming == 1) {
        if ((sub = strstr(msg->data, "\r\n+INIT:")) != NULL) {
            //LOG_DBG("Got %20s message", msg->data);
            // We want to call net_handle_DA_Init() if we have shipped (1) or
            // if the uicr is invalid and neds restoring (<0)
            if (uicr_shipping_flag_get() || !uicr_in_factory_flag_get()) {
                net_handle_DA_Init(msg, sub);
            }
        } else if ((sub = strstr(msg->data, "\r\n+WFJAP:")) != NULL) {
            if ((sub = strstr(msg->data, "\r\n+WFJAP:1")) != NULL) {
                net_handle_DA_APConn(msg, sub);
            }
            if ((sub = strstr(msg->data, "\r\n+WFJAP:0")) != NULL) {
                net_handle_DA_APDiscon(msg, sub);
            }
        } else if ((sub = strstr(msg->data, "\r\n+WFDAP")) != NULL) {
            net_handle_DA_APDiscon(msg, sub);
        } else if ((sub = strstr(msg->data, "\r\n+DPM")) != NULL) {
            net_handle_DA_DPM(msg, sub);
        } else if ((sub = strstr(msg->data, "\r\n+TIME")) != NULL) {
            d1_set_5340_time(sub, false);
        } else if ((sub = strstr(msg->data, "\r\n+RSSI:")) != NULL) {
            int rssi = RSSI_NOT_CONNECTED;
            if (sub[8] == '-' || isdigit(sub[8])) {
                rssi = strtol(sub + 8, NULL, 10);
            }
            send_zbus_int_event(DA_EVENT_TYPE_RSSI, rssi, &(da_state.rssi), false);
        } else if ((sub = strstr(msg->data, "\r\n+NWMQCL:1")) != NULL) {
            queue_get_time_work();
            send_zbus_tri_event(DA_EVENT_TYPE_MQTT_ENABLED, DA_STATE_KNOWN_TRUE, &(da_state.mqtt_enabled));
            send_zbus_tri_event(
                DA_EVENT_TYPE_MQTT_BROKER_CONNECT, DA_STATE_KNOWN_TRUE, &(da_state.mqtt_broker_connected));
        } else if ((sub = strstr(msg->data, "\r\n+NWMQCL:0")) != NULL) {
            // Broker not connected, doesn't mean not enabled
            send_zbus_tri_event(
                DA_EVENT_TYPE_MQTT_BROKER_CONNECT, DA_STATE_KNOWN_FALSE, &(da_state.mqtt_broker_connected));
        } else if ((sub = strstr(msg->data, "\r\n+NWMQMSGSND")) != NULL) {
            send_zbus_timestamp_event(DA_EVENT_TYPE_MQTT_MSG_SENT, k_uptime_get(), &(da_state.mqtt_last_msg_time));
            queue_get_time_work();
        } else if ((sub = strstr(msg->data, "\r\n+NWMQTS:")) != NULL) {
            net_handle_DA_SubTopic(msg, sub);
        } else if ((sub = strstr(msg->data, "\r\n+NWCCRT:")) != NULL) {
            int flags = atoi(sub + 10);
            if (flags & 0x07) {
                send_zbus_tri_event(DA_EVENT_TYPE_MQTT_CERTS, DA_STATE_KNOWN_TRUE, &(da_state.mqtt_certs_installed));
            } else {
                send_zbus_tri_event(DA_EVENT_TYPE_MQTT_CERTS, DA_STATE_KNOWN_FALSE, &(da_state.mqtt_certs_installed));
            }
        } else if ((sub = strstr(msg->data, "\r\n+NWMQMSG:")) != NULL) {
            net_handle_DA_MQTT_Msg(msg, sub);
            queue_get_time_work();
        } else if ((sub = strstr(msg->data, "\r\n+NWMQAUTO:")) != NULL) {
            int state = atoi(sub + 12);
            send_zbus_tri_event(DA_EVENT_TYPE_BOOT_MQTT_STATE, state == 1, &(da_state.mqtt_on_boot));
        } else if ((sub = strstr(msg->data, "\r\n+WFDIS:")) != NULL) {
            int nv = (sub[9] == '1');
            send_zbus_tri_event(DA_EVENT_TYPE_AP_PROFILE_USE, nv, &(da_state.ap_profile_disabled));
        } else if ((sub = strstr(msg->data, "\r\n+NWHTCSTATUS:")) != NULL) {
            wifi_at_http_status(msg);
        } else if ((sub = strstr(msg->data, "\r\n+NWHTCDATA:")) != NULL) {
            wifi_at_http_write(msg);
        } else if ((sub = strstr(msg->data, "\r\n+NWOTADWSTART:")) != NULL) {
            strncpy(ota_last_start_result, sub + strlen("\r\n+NWOTADWSTART:"), 4);
            ota_last_start_result[4] = 0;
        } else if ((sub = strstr(msg->data, "\r\n+SSIDLIST:")) != NULL) {
            char *list = k_calloc(msg->data_len + 2, 1);
            if (list != NULL) {
                memcpy(list, msg->data, msg->data_len);
                sub = strstr(list, "\r\n+SSIDLIST:");
                char *pos;
                char *line = strtok_r(sub, "\n", &pos);
                if (line != NULL) {
                    while (line != NULL && (line - list) < msg->data_len) {
                        wifi_add_SSID_to_cached_list(line);
                        line = strtok_r(NULL, "\n", &pos);
                    }
                }
                k_free(list);
            }
        } else if ((sub = strstr(msg->data, "\r\n+VER:FRTOS-GEN01-01-TDEVER_")) != NULL) {
            //\r\n+VER:FRTOS-GEN01-01-TDEVER_ABC-YYMMDD
            uint8_t ver[3] = { 0, 0, 0 };
            int     off    = sizeof("\r\n+VER:FRTOS-GEN01-01-TDEVER_") - 1;
            ver[0]         = sub[off + 0] - '0';    // EAS TODO allow for 2 digit version numbers
            ver[1]         = sub[off + 1] - '0';    // EAS TODO allow for 2 digit version numbers
            ver[2]         = sub[off + 2] - '0';    // EAS TODO allow for 2 digit version numbers
            send_zbus_version_event(ver);
        }
    } else {
        // Outgoing messages
        strncpy(da_state.last_cmd, msg->data, LAST_CMD_LEN - 1);
        da_state.last_cmd[LAST_CMD_LEN - 1] = 0;
        // NOTE: Do not publish from here because outgoing messages
        // may be sent from withing a observer
    }
}

//////////////////////////////////////////////////////////
// net_set_saved_bool()
//
// Set a bool into the DA NVRAM
//
// @param addr - the address in the DA NVRAM
// @param stored - a pointer to the current value
// @param event_flag - the event flag to set if the value changes
// @param val - the value to set
//
// @return - 0 on success <0 on error
//////////////////////////////////////////////////////////
int net_set_saved_bool(uint32_t addr, das_tri_state_t *localvar, uint32_t event_flag, uint8_t val)
{
    char vals[4];
    int  ret;

    snprintf(vals, 4, "%02X", val);
    if ((ret = wifi_put_nvram(addr, vals, K_MSEC(1000))) == 0) {
        send_zbus_tri_event(event_flag, val, localvar);
    }
    return ret;
}

//////////////////////////////////////////////////////////
// net_get_saved_bool()
//
// Get a value of a bool from the DA NVRAM and compare it
// to the expected value and set an event flag it changed
// and set the new value it it change
//
// @param addr - the address in the DA NVRAM
// @param stored - a pointer to the current value
// @param event_flag - the event flag to set if the value changes
// @param default_val - the value to set if the value has never been set
//
// @return - the value read from the DA NVRAM
//            < 0 on error
//////////////////////////////////////////////////////////
int net_get_saved_bool(uint32_t addr, das_tri_state_t *localvar, uint32_t event_flag, uint8_t default_val)
{
    uint8_t tmp;
    char    val[4];
    int     ret;

    // Steve encounted a pmic reboot in which 100ms timed out.
    // The calls before and after worked, so 100 must be too short
    if ((ret = wifi_get_nvram(addr, &tmp, 1, K_MSEC(200))) == 0) {
        // The value has never been set set it
        if (tmp != 1 && tmp != 0) {
            tmp = default_val;
            snprintf(val, 4, "%02X", default_val);
            ret = wifi_put_nvram(addr, val, K_MSEC(200));
            if (ret == 0) {
                send_zbus_tri_event(event_flag, tmp, localvar);
            }
        }
        *localvar = tmp;
    } else {
        return ret;
    }
    return tmp;
}

//////////////////////////////////////////////////////////
//	net_do_da_init_work_fn()
//
// This work function is called when the DA sends us an
// INIT:DONE message which happens when it boots or wakes
// up from RTC or DPM sleep.
//
// It reads the stored values from the DA if needed
static void net_do_da_init_work_fn()
{
    int      ret;
    uint64_t now = k_uptime_get();
    char     errtxt[100];
    char    *machine_id = NULL;

    LOG_DBG("DA Booted checking state");
    if (wifi_get_mutex(K_MSEC(3000), __func__) != 0) {
        LOG_ERR("Failed to get wifi Mutex for stored values read");
        return;
    }

    int64_t dpm_delta = (now - g_last_dpm_change);
    // LOG_DBG("DA DPM delta %lld", dpm_delta);
    if (!uicr_in_factory_flag_get() && dpm_delta < 3000 && dpm_delta > 0) {
        // The DA was just told to switch DPM modes which causes a reboot
        // We only need to verify DPM mode state, but the rest of INIT
        // work can be ignored since nothing should have changed
        if (g_awake_on_boot) {
            ret = wifi_send_ok_err_atcmd("AT+CLRDPMSLPEXT", errtxt, K_MSEC(150));
            if (ret != 0) {
                LOG_ERR("'%s'(%d) setting DPM to be awake at boot (%s)", wstrerr(-ret), ret, errtxt);
            }
        }
        goto gsv_release_exit;
    }

    bool uicr_valid = false;
    if (da_state.uicr_bu_status == -1) {
        uicr_valid = (insure_uicr_backup() == 0);
    } else {
        uicr_valid = (da_state.uicr_bu_status == 1);
    }
    // if the retored shipping flag is not shipped, don't do anything else
    if (!uicr_shipping_flag_get()) {
        goto gsv_release_exit;
    }
    if (uicr_valid) {
        // We can trust the UICR, set up the DA's MAC address,
        // XTAL setting, and machine id
        if (da_state.mac_set == -1) {
            int   which;
            char *mac = wifi_get_mac(&which, K_MSEC(500));
            if (mac == NULL) {
                send_zbus_tri_event(DA_EVENT_TYPE_MAC_SET, DA_STATE_KNOWN_FALSE, &(da_state.mac_set));
                LOG_ERR("Error getting MAC");
            } else {
                // The DA MAC will be in the format XX:YY:ZZ:AA:BB:CC
                // The UICR MAC will be in the format XXYYZZAABBCC
                char *uicr_mac = uicr_wifi_mac_address_get();
                if (cmpMACs(mac, uicr_mac) != 0) {
                    // The MAC is not set to the UICR value, set it
                    if ((ret = wifi_set_mac(uicr_mac, K_MSEC(500))) == 0) {
                        send_zbus_tri_event(DA_EVENT_TYPE_MAC_SET, DA_STATE_KNOWN_TRUE, &(da_state.mac_set));
                        // We need to restart the DA to use the MAC
                        LOG_ERR("Restarting DA to set MAC");
                        wifi_send_ok_err_atcmd("AT+RESTART", NULL, K_MSEC(100));
                        // We need to wait for the DA to restart
                        goto gsv_release_exit;
                    } else {
                        send_zbus_tri_event(DA_EVENT_TYPE_MAC_SET, DA_STATE_KNOWN_FALSE, &(da_state.mac_set));
                        LOG_ERR("'%s'(%d) setting MAC", wstrerr(-ret), ret);
                    }
                } else {
                    // The MAC is set to the UICR value, we are good
                    send_zbus_tri_event(DA_EVENT_TYPE_MAC_SET, DA_STATE_KNOWN_TRUE, &(da_state.mac_set));
                }
            }
        }

        if (da_state.xtal_set == -1) {
            int uicr_xtal = uicr_wifi_tuning_value_get();
            int curr_xtal = wifi_get_xtal(K_MSEC(200));
            if (curr_xtal >= 0 && uicr_xtal != curr_xtal) {
                errtxt[0] = 0;
                if ((ret = wifi_set_xtal(uicr_xtal, errtxt, K_MSEC(200))) != 0) {
                    LOG_ERR("'%s'(%d) setting XTAL %s", wstrerr(-ret), ret, errtxt);
                }
                send_zbus_tri_event(DA_EVENT_TYPE_XTAL_SET, ret == 0, &(da_state.xtal_set));
            } else {
                LOG_ERR("Error getting XTAL");
            }
        }

        // If the DA DHCP client name has not been set or we don't know if it is
        if (da_state.dhcp_client_name_set != DA_STATE_KNOWN_TRUE) {
            char name[70];
            if (da_state.uicr_bu_status == DA_BU_EXISTS) {
                machine_id = uicr_serial_number_get();
                snprintf(da_state.dhcp_client_name, DHCP_CLIENT_NAME_LEN, "Petivity-Tracker-%.14s", machine_id);
            } else {
                snprintf(da_state.dhcp_client_name, DHCP_CLIENT_NAME_LEN, "Petivity-Tracker-%.14s", "UNKNOWN");
            }
            snprintf(name, 70, "AT+NWDHCHN=%s", da_state.dhcp_client_name);
            errtxt[0] = 0;
            if ((ret = wifi_send_ok_err_atcmd(name, errtxt, K_MSEC(200))) != 0) {
                LOG_ERR("'%s'(%d) setting DHCP client name %s", wstrerr(-ret), ret, errtxt);
            }
            send_zbus_tri_event(DA_EVENT_TYPE_DHCP_CLIENT_NAME_SET, ret == 0, &(da_state.dhcp_client_name_set));
        }

        // The DA doesn't work unless there is a default publish topic which is weird
        // cause we alwasy send the explicit topic in the message.  Set it now anyway
        if ((ret = wifi_send_ok_err_atcmd("AT+NWMQTP=messages/0/0/0/0", errtxt, K_MSEC(200))) != 0) {
            LOG_ERR("'%s'(%d) setting default publish topic %s", wstrerr(-ret), ret, errtxt);
        }
    }

    if ((ret = wifi_send_ok_err_atcmd("AT+DPMTIMWU=30", errtxt, K_MSEC(250)))
        != 0) {    // I have seen this take 80ms on SPI and fail
        LOG_ERR("'%s'(%d) setting DPM Wake Up Time %s", wstrerr(-ret), ret, errtxt);
    }

    if ((ret = wifi_send_ok_err_atcmd("AT+DPMKA=30000", errtxt, K_MSEC(150)))
        != 0) {    // I have seen this take 80ms on SPI and fail
        LOG_ERR("'%s'(%d) setting DPM Keep Alive %s", wstrerr(-ret), ret, errtxt);
    }

    if ((ret = wifi_send_ok_err_atcmd("AT+NWMQAUTO=?", errtxt, K_MSEC(150))) != 0) {
        LOG_ERR("'%s'(%d) trying to get MQTT on boot state %s", wstrerr(-ret), ret, errtxt);
    }

    if ((ret = wifi_send_ok_err_atcmd("AT+DPM=?", errtxt, K_MSEC(150))) != 0) {
        LOG_ERR("'%s'(%d) getting DPM status %s", wstrerr(-ret), ret, errtxt);
    }

    if ((ret = wifi_send_ok_err_atcmd("AT+VER", errtxt, K_MSEC(150))) != 0) {
        LOG_ERR("'%s'(%d) getting version %s", wstrerr(-ret), ret, errtxt);
    }

    if ((ret = wifi_send_ok_err_atcmd("AT+WFDIS=?", errtxt, K_MSEC(150))) != 0) {
        LOG_ERR("'%s'(%d) getting ap profile use %s", wstrerr(-ret), ret, errtxt);
    }

    // Read in the state of the DA from NVRAM
    ret = net_get_saved_bool(DA_NV_ONBOARDED_ADDR, &(da_state.onboarded), DA_EVENT_TYPE_ONBOARDED, 0);
    if (ret < 0) {
        LOG_ERR("'%s'(%d) getting onboarded state", wstrerr(-ret), ret);
        goto gsv_release_exit;
    }

    if (g_DA_needs_one_time_config == true) {
        machine_id = uicr_serial_number_get();
        if (strncmp(machine_id, "DT", 2) != 0) {
            LOG_ERR("The Serial Number is not valid, please correct that (%s)", machine_id);
        } else {
            if ((ret = wifi_set_mqtt_state(0, K_MSEC(300))) != 0) {
                LOG_ERR("'%s'(%d) stopping MQTT to configure it", wstrerr(-ret), ret);
                goto gsv_release_exit;
            }

            if (da_state.mqtt_client_id[0] == 0) {
                // Make sure the brand is no more then 3 characters
                if (CONFIG_IOT_MQTT_BRAND_ID > 999 || CONFIG_IOT_MQTT_BRAND_ID < -99) {
                    LOG_ERR("The MQTT Brand ID not valid!!!");
                }
                snprintf(da_state.mqtt_client_id, 17, "%d_%s", CONFIG_IOT_MQTT_BRAND_ID, machine_id);
            }
            // If we are booting for the first time
            // Then make sure the DA doesn't use any previous AP profile to connect
            // to an AP.
            //
            // We do need this functionality during normal operation when we disconnect
            // from the AP because we may not want to reconnect to the same AP and
            // having it auto connect to the last AP can make our state machine more
            // complicated and error prone.
            if ((ret = wifi_send_ok_err_atcmd("AT+WFDIS=1", NULL, K_MSEC(300))) < 0) {
                LOG_ERR("'%s'(%d) disabling the AP profile on boot", wstrerr(-ret), ret);
            }

            static shadow_zone_t fromDA[NUM_ZONES];    // static to keep off stack
            ret = wifi_get_ap_list(fromDA, K_MSEC(3000));
            // Special case when we move from un-encrypted to encrypted, leaves the SSID store unusable
            if (ret == -821) {
                LOG_WRN("Decrypt error reading SSIDs, delete and retry!!");
                ret = wifi_saved_ssids_del_all(K_MSEC(3000));
                if (ret != 0) {
                    LOG_ERR("'%s'(%d) returned from wifi_saved_ssids_del_all", wstrerr(-ret), ret);
                }
                ret = wifi_get_ap_list(fromDA, K_MSEC(3000));
                // Error status checked after the loop
            }
            if (ret != 0) {
                LOG_ERR("'%s'(%d) getting AP list from DA", wstrerr(-ret), ret);
            } else {
                // copy to the shadow doc and if there are differences, queue
                // shadow update to let backend know what is current
                bool changed = false;
                for (int i = 0; i < NUM_ZONES; i++) {
                    if (strncmp(fromDA[i].ssid, shadow_doc.zones[i].ssid, 32) != 0) {
                        changed = true;
                        strncpy(shadow_doc.zones[i].ssid, fromDA[i].ssid, 32);
                        shadow_doc.zones[i].ssid[32] = 0;
                        shadow_doc.zones[i].safe     = fromDA[i].safe;
                    }
                }
                if (changed) {
                    commMgr_queue_shadow(NULL);
                }
            }

            int   ddb_len = sizeof(CONFIG_IOT_BROKER_HOST_NAME) + 20;
            char *ddb_buf = (char *)k_malloc(ddb_len);
            if (ddb_buf != NULL) {
                snprintf(ddb_buf, ddb_len, "AT+NWMQCID=%s", da_state.mqtt_client_id);
                if ((ret = wifi_send_ok_err_atcmd(ddb_buf, errtxt, K_MSEC(1000))) < 0) {
                    LOG_ERR("'%s'(%d) Setting the client id (%s) %s", wstrerr(-ret), ret, ddb_buf, errtxt);
                }

                if ((ret = wifi_send_ok_err_atcmd("AT+NWMQCS=1", errtxt, K_MSEC(1000))) < 0) {
                    LOG_ERR("'%s'(%d) Setting the clean session flag %s", wstrerr(-ret), ret, errtxt);
                }

                if ((ret = wifi_send_ok_err_atcmd("AT+NWMQTLS=1", errtxt, K_MSEC(300))) < 0) {
                    LOG_ERR("'%s'(%d) enabling MQTT TLS %s", wstrerr(-ret), ret, errtxt);
                }

                if ((ret = wifi_send_ok_err_atcmd("AT+WFCC=US", errtxt, K_MSEC(300))) < 0) {
                    LOG_ERR("'%s'(%d) setting CC to US %s", wstrerr(-ret), ret, errtxt);
                }

                snprintf(ddb_buf, ddb_len, "AT+NWMQBR=%s,8883", CONFIG_IOT_BROKER_HOST_NAME);
                if ((ret = wifi_send_ok_err_atcmd(ddb_buf, errtxt, K_MSEC(300))) < 0) {
                    LOG_ERR("'%s'(%d) Setting the MQTT broker %s", wstrerr(-ret), ret, errtxt);
                }
                k_free(ddb_buf);
            } else {
                LOG_ERR("k_malloc err, not setting client_id, and mqtt params");
            }

            int topics[15];
            topics[0]      = MQTT_MESSAGE_TYPE_ONBOARDING;
            int num_topics = 1;
            if (da_state.onboarded == 1) {
                topics[1]  = MQTT_MESSAGE_TYPE_FOTA;
                topics[2]  = MQTT_MESSAGE_TYPE_REMOTE_FUNCTION;
                topics[3]  = MQTT_MESSAGE_TYPE_CONN_TEST;
                topics[4]  = MQTT_MESSAGE_TYPE_SHADOW_PROXY;
                topics[5]  = MQTT_MESSAGE_TYPE_SRF_NONCE;
                topics[6]  = MQTT_MESSAGE_TYPE_SRF_FUNC;
                topics[7]  = MQTT_MESSAGE_TYPE_CONFIG_HUB;
                num_topics = 8;
            }
            if ((ret = wifi_set_mqtt_sub_topics_by_type(topics, num_topics, K_MSEC(500))) != 0) {
                LOG_ERR("'%s'(%d) setting MQTT sub topics", wstrerr(-ret), ret);
            }

            if (da_state.mqtt_certs_installed == -1) {
                // Sending this query will cause the DA to send back its status which will
                // be processed by net_monitor_DA() above
                wifi_send_ok_err_atcmd("AT+NWCCRT", NULL, K_MSEC(300));
            }

            if (da_state.ntp_server_set == -1 || da_state.ntp_server_set == 0) {
                ret = wifi_set_ntp_server();
                if (ret < 0) {
                    LOG_ERR("'%s'(%d) setting ntp server", wstrerr(-ret), ret);
                }
                // if we just set the ntp server, we need to give the DA time it and
                // set its time before we potentially put it back to sleep
                k_sleep(K_MSEC(2000));
            }

            g_DA_needs_one_time_config = false;
        }
    }

gsv_release_exit:
    LOG_DBG("DA Booted done");
    wifi_release_mutex();
}

//////////////////////////////////////////////////////////
// report_evt()
//
// Helper function to report the state of a das_tri_state_t
// based flag variable in the da_state_t structure.
// If the the event that just changed is the event in
// question, report the state and return true
bool report_evt(const da_event_t *evt, uint32_t event_flag, const das_tri_state_t *state, char *state_str)
{
    bool ret = false;
    if (evt->events & event_flag) {
        if (*state == DA_STATE_KNOWN_TRUE) {
            LOG_DBG("da_state: TRUE: %s", state_str);
            ret = true;
        } else if (*state == DA_STATE_UNKNOWN) {
            LOG_DBG("da_state: UNKNOWN: %s", state_str);
        } else {
            LOG_DBG("da_state: FALSE: %s", state_str);
        }
    }
    return ret;
}

//////////////////////////////////////////////////////////
// report_bu()
//
// Helper function to report the state of a int8_t based
// flag variable in the da_state_t structure.
// If the the event that just changed is the event in
// question, report the state and return true
bool report_bu(const da_event_t *evt, uint32_t event_flag, const das_bu_status_t *state, char *state_str)
{
    bool ret = false;
    if (evt->events & event_flag) {
        if (*state == DA_BU_EXISTS) {
            LOG_DBG("da_state: Backed up: %s", state_str);
            ret = true;
        } else if (*state == DA_BU_UNKNOWN) {
            LOG_DBG("da_state: Back up unknown: %s", state_str);
        } else if (*state == DA_BU_MISMATCH) {
            LOG_DBG("da_state: Back up mismatch: %s", state_str);
        } else {
            LOG_DBG("da_state: No Backup: %s", state_str);
        }
    }
    return ret;
}

static void net_reset_da_fn()
{
    wifi_reset();    // If the 5340 ever restarts but the DA didn't then we may
                     // not be able to talk to it due to dpm mode.  There is a
                     // window after the DA boots where we can talk to it before
                     // it sleeps.  We reboot the DA now so that the 5340 can
                     // talk to it and get in sync with shadow
}

//////////////////////////////////////////////////////////
// watcher_listener
//
// Called by ZBus when the net state changes.  This is
// a listern which means it should be treated like an ISR
// and do as little as possible.  It should queue work
// items to do the real work.
//////////////////////////////////////////////////////////
void watcher_listener(const struct zbus_channel *chan)
{
    if (chan != &da_state_chan) {
        LOG_ERR("zbus subscriber received from unknown channel");
        return;
    }
    sa_event_watch_item_t *item = k_malloc(sizeof(sa_event_watch_item_t));
    if (item == NULL) {
        LOG_ERR("Error allocating memory for event");
        return;
    }
    k_work_init(&item->work, watch_work_fn);
    const da_event_t *evt = zbus_chan_const_msg(chan);    // Direct message access
    memcpy(&(item->evt), evt, sizeof(da_event_t));
    queue_watch_work(item);
}

static void watch_work_fn(struct k_work *work)
{
    sa_event_watch_item_t *item = CONTAINER_OF(work, sa_event_watch_item_t, work);
    // Debug flag, print out the events
    if (g_show_events) {
        LOG_ERR("zbus subscriber received evt 0x%X, published@ %lld", item->evt.events, item->evt.timestamp);
    }

    if (item->evt.events & DA_EVENT_TYPE_IS_SLEEPING && da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        // We want to enforce a minimum time between sleep and the next wake.
        // Since there are lots of places in the code that sleep, but they all should
        // set the is_sleeping flag, we can use the transitions to set a timer to prevent
        // waking up too soon.
        g_last_sleep_time = k_uptime_get();
    }
    // Each time the DA restarts, we read in the nvram variable we stored there.
    // They shouldn't change if we don't change them, but check anyway
    // We can't do this in this thread because it takes too long and we
    // don't want to miss message, so we trigger a work item
    if (item->evt.events & DA_EVENT_TYPE_DA_RESTARTED) {
        LOG_DBG("da_state: DA restarted");
    }

    if (item->evt.events & DA_EVENT_TYPE_WIFI_INIT) {
        LOG_DBG("da_state: DA %s initialized", da_state.initialized ? "is" : "not");
        if (da_state.initialized == DA_STATE_KNOWN_TRUE) {
            queue_reset_da_work();
        }
    }
    report_evt(&(item->evt), DA_EVENT_TYPE_AP_CONNECT, &(da_state.ap_connected), "connected to an AP");
    report_evt(&(item->evt), DA_EVENT_TYPE_AP_SAFE, &(da_state.ap_safe), "AP is a safe zone");
    report_evt(&(item->evt), DA_EVENT_TYPE_AP_PROFILE_USE, &(da_state.ap_profile_disabled), "AP profile disabled");
    report_evt(&(item->evt), DA_EVENT_TYPE_DPM_MODE, &(da_state.dpm_mode), "in DPM mode");

    report_evt(&(item->evt), DA_EVENT_TYPE_IS_SLEEPING, &(da_state.is_sleeping), "sleeping");
    report_evt(&(item->evt), DA_EVENT_TYPE_MQTT_ENABLED, &(da_state.mqtt_enabled), "MQTT enabled");
    report_evt(
        &(item->evt), DA_EVENT_TYPE_MQTT_BROKER_CONNECT, &(da_state.mqtt_broker_connected), "connected to MQTT broker");
    report_evt(
        &(item->evt), DA_EVENT_TYPE_BOOT_MQTT_STATE, &(da_state.mqtt_on_boot), "MQTT client will auto start on DA boot");

    if (item->evt.events & DA_EVENT_TYPE_MQTT_MSG_SENT) {
        LOG_DBG("da_state: Last MQTT message sent at %lld", da_state.mqtt_last_msg_time);
    }
    report_evt(&(item->evt), DA_EVENT_TYPE_MQTT_CERTS, &(da_state.mqtt_certs_installed), "MQTT certs installed");
    report_evt(&(item->evt), DA_EVENT_TYPE_NTP_SERVER_SET, &(da_state.ntp_server_set), "NTP server set");
    report_evt(
        &(item->evt), DA_EVENT_TYPE_DHCP_CLIENT_NAME_SET, &(da_state.dhcp_client_name_set), "DHCP client host name is set");

    if (item->evt.events & DA_EVENT_TYPE_MQTT_SUB_TOP_CHANGED) {
        for (int t = 0; t < da_state.mqtt_sub_topic_count; t++) {
            if (da_state.mqtt_sub_topics[t][0] != 0) {
                LOG_DBG("da_state: MQTT topic %d:%s", t, da_state.mqtt_sub_topics[t]);
            }
        }
    }
    if (item->evt.events & DA_EVENT_TYPE_LAST_CMD) {
        LOG_DBG("da_state: Last command:%s", da_state.last_cmd);
    }
    report_bu(&(item->evt), DA_EVENT_TYPE_UICR_BU_STATUS, &(da_state.uicr_bu_status), "UICR backup exists");
    report_evt(&(item->evt), DA_EVENT_TYPE_MAC_SET, &(da_state.mac_set), "MAC addr is set");
    report_evt(&(item->evt), DA_EVENT_TYPE_XTAL_SET, &(da_state.xtal_set), "DA XTAL is set");
    report_evt(&(item->evt), DA_EVENT_TYPE_ONBOARDED, &(da_state.onboarded), "has been onboarded");
    if (item->evt.events & DA_EVENT_TYPE_HTTP_COMPLETE) {
        LOG_DBG("Http download of %ld bytes completed with code %d", http_amt_written, httpresultcode);
    }
    if (item->evt.events & DA_EVENT_TYPE_OTA_PROGRESS) {
        LOG_DBG("OTA: %s", net_ota_progress_str(da_state.ota_progress));
    }
    if (item->evt.events & DA_EVENT_TYPE_REBOOT_CNT) {
        LOG_DBG("DA reboot count: %d", da_state.reboot_cnt);
    }
    if (item->evt.events & DA_EVENT_TYPE_VERSION) {
        LOG_DBG("DA version number: %d.%d.%d", da_state.version[0], da_state.version[1], da_state.version[2]);
    }

    k_free(item);
}

//////////////////////////////////////////////////////////
// write_uicr_backup()
//
// Write the UICR backup to the DA NVRAM
//
// @return - 0 on success, -1 on error
int write_uicr_backup()
{
    char uicr_bu_str[DA_UICR_BACKUP_SIZE * 2 + 1];
    for (int k = 0; k < DA_UICR_BACKUP_SIZE; k++) {
        snprintf(&uicr_bu_str[k * 2], 3, "%02x", da_state.uicr_bu[k]);
    }
    int ret = wifi_put_nvram(DA_UICR_BACKUP_ADDR, uicr_bu_str, K_MSEC(500));
    if (ret != 0) {
        LOG_ERR("Error writing UICR backup");
        // Even though we know we don't have a backup
        // we aren't going to change the status so that
        // next time the work function is called we try
        // to back up again
        return ret;
    }

    strncpy(uicr_bu_str, "EA", 3);
    ret = wifi_put_nvram(DA_UICR_BACKUP_FLAG, uicr_bu_str, K_MSEC(500));
    if (ret != 0) {
        LOG_ERR("Error writing UICR backup flag");
        // Even though we know we don't have a backup
        // we aren't going to change the status so that
        // next time the work function is called we try
        // to back up again
        return ret;
    }
    return 0;
}

//////////////////////////////////////////////////////////
//	insure_uicr_backup()
//
//  This function tries to make sure that the UICR is
//  backed up to DA NVram.
//
//  @param evt - a pointer to an event structure in case
//               we change the state and need to publish
//               the change
//  @return - 0 on success, -1 on error, 1 if a developer
//			  needs to intervene
int insure_uicr_backup()
{
    char            has_backed_up = 0;    // Magic value of "EA" = has been
    int             ret;
    das_bu_status_t new_state = DA_BU_UNKNOWN;

    // Check to see if there is a backup
    ret = wifi_get_nvram(DA_UICR_BACKUP_FLAG, &has_backed_up, 1, K_MSEC(500));
    if (ret < 0) {
        // Can't know if there is
        LOG_ERR("Error reading UICR backup flag");
        return ret;
    }

    if (has_backed_up == 0xea) {
        // Get the backup
        ret = wifi_get_nvram(DA_UICR_BACKUP_ADDR, da_state.uicr_bu, DA_UICR_BACKUP_SIZE, K_MSEC(500));
        if (ret < 0) {
            // Can't know if there is a backup
            LOG_ERR("'%s'(%d) reading UICR backup", wstrerr(-ret), ret);
            return ret;
        }
        // This next code will only be needed for developers cause only they can clear UICR
        // and run latest code which will write a new UICR version.  Since the diff between
        // 3 and 4 is only the ship mode flag, we just set it to 1 and change the version in
        // the backup to 4, if they match then we are done, if not then there is some other
        // problem that the dev needs to fix
        if (((uint32_t *)da_state.uicr_bu)[0] == 0xBEEF0003 && uicr_version_get() == 0xBEEF0004) {
            uicr_storage_struct_t *uicr_bu = (uicr_storage_struct_t *)da_state.uicr_bu;
            uicr_bu->uicr_version_number   = MY_UICR_SCHEMA_VERSION;
            uicr_bu->shipping_flag         = MAGIC_SHIPPED_SIGNATURE;
            ret                            = write_uicr_backup();
            if (ret < 0) {
                return ret;
            }
        }
        // Check the signature
        if (((uint32_t *)da_state.uicr_bu)[0] != uicr_version_get()) {
            // No match, the dev needs to intervene
            LOG_ERR("ERROR!!  UICR backup does not match schema version!");
            new_state = DA_BU_MISMATCH;
            ret       = 1;
            goto pub_and_exit;
        }

        // Check the backup and correct UICR if we can
        ret = uicr_backup_cmp_restore((uint32_t *)da_state.uicr_bu);
        if (ret < 0) {
            // No match, the dev needs to intervene
            LOG_ERR("ERROR!!  UICR backup does not match and can't be restored!");
            new_state = DA_BU_MISMATCH;
            ret       = 1;
            goto pub_and_exit;
        }
        if (ret == 1) {
            // The UICR was corrected and things get values at boot time so we need to reboot
            LOG_ERR("UICR was corrected, rebooting");
            LOG_PANIC();
            pmic_reboot("UICR update");
        }

        // Everything matches, so we are good
        new_state = DA_BU_EXISTS;
        ret       = 1;
    } else {
        // There isn't a backup, make one
        if (uicr_verify() != 0) {
            if (uicr_shipping_flag_get()) {
                LOG_ERR("UICR is not valid, can't back it up");
            }
            // Even though we know we don't have a backup
            // we aren't going to change the status so that
            // next time the work function is called we try
            // to back up again
            return -1;
        }
        uicr_export((uint32_t *)(da_state.uicr_bu));
        ret = write_uicr_backup();
        if (ret < 0) {
            LOG_ERR("Error writing UICR backup");
            return ret;
        }

        // Whew, we are backed up
        ret       = 0;
        new_state = DA_BU_EXISTS;
    }

pub_and_exit:
    send_zbus_bu_event(new_state);
    return ret;
}

//////////////////////////////////////////////////////////
// The DA MAC will be in the format XX:YY:ZZ:AA:BB:CC
// The UICR MAC will be in the format XXYYZZAABBCC
// compare and return 0 if equal
//////////////////////////////////////////////////////////
int cmpMACs(char *mac1, char *mac2)
{
    int i1 = 0, i2 = 0;
    int l1 = strlen(mac1);
    int l2 = strlen(mac2);

    while (i1 < l1 && i2 < l2) {
        if (mac1[i1] == ':') {
            i1++;
        }
        if (mac2[i2] == ':') {
            i2++;
        }
        if (strncasecmp(mac1 + i1, mac2 + i2, 1) != 0) {
            return -1;
        }
        i1++;
        i2++;
    }
    return 0;
}

////////////////////////////////////////////////////////////
// net_ota_progress_str()
//
// Return a string that describes the OTA progress
//
// @param progress - the progress to describe
//
// @return - a string that describes the progress
//
const char *net_ota_progress_str(da_ota_progress_t progress)
{
    static char str[100];
    switch (progress) {
    case DA_OTA_PROGRESS_NO_OTA:
        return ("OTA is not in progress");
        break;
    case DA_OTA_CANT_GET_MUTEX:
        return ("Can't get mutex for OTA");
        break;
    case DA_OTA_ERR_STARTING_DL:
        return ("Error starting OTA download");
        break;
    case DA_OTA_TIMEOUT_GETTING_PROGRESS:
        return ("Timeout getting OTA progress");
        break;
    case DA_OTA_ERROR_GETTING_PROGRESS:
        return ("Error getting OTA progress");
        break;
    case DA_OTA_ERROR_STOPPING:
        return ("Error stopping OTA");
        break;
    case DA_OTA_PROGRESS_STALLED:
        return ("OTA stalled");
        break;
    case DA_OTA_ERROR_RENEWING:
        return ("Error renewing OTA");
        break;
    case DA_OTA_DOWNLOAD_COMPLETE:
        return ("OTA download complete");
        break;
    case DA_OTA_PROGRESS_REBOOTING:
        return ("Rebooting for OTA");
        break;
    case DA_OTA_ASYNC_START_ERROR:
        return ("Async OTA error");
        break;
    default:
        snprintf(str, 100, "OTA: %d%% downloaded", (int)progress);
        return str;
        break;
    }
}

////////////////////////////////////////////////////////////
// net_stage_ssid_for_saving()
//
// Stage a ssid for saving later when confirmed connected
//
// @param ssid - the ssid name
// @param pass - the password
// @param sec - the security type
// @param keyidx - the key index
// @param enc - the encryption type
// @param hidden - true if the ssid is hidden
//
void net_stage_ssid_for_saving(char *ssid, char *pass, uint16_t sec, uint16_t keyidx, uint16_t enc, bool hidden)
{
    strncpy(g_last_conn_attempt.ssid, ssid, 32);
    g_last_conn_attempt.ssid[32] = 0;
    strncpy(g_last_conn_attempt.password, pass, 64);
    g_last_conn_attempt.password[64]       = 0;
    g_last_conn_attempt.sec                = sec;
    g_last_conn_attempt.keyidx             = keyidx;
    g_last_conn_attempt.enc                = enc;
    g_last_conn_attempt.hidden             = hidden;
    g_last_conn_attempt.last_time_accessed = commMgr_get_unix_time();
}

////////////////////////////////////////////////////////////
// net_unstage_ssid_for_saving()
//
// Unstage a ssid for saving later when confirmed connected
//
//
void net_unstage_ssid_for_saving()
{
    g_last_conn_attempt.ssid[0]     = 0;
    g_last_conn_attempt.password[0] = 0;
}

///////////////////////////////////////////////////////
// tde0002
// “tde 0002 \r\n”
// Get the Wifi version number
// FRTOS-GEN01-01-TDEVER_wxy-231212     official release
// FRTOS-GEN01-01-23b34fr2a!-231212     git hash from development release
// FRTOS-GEN01-01-UNTRACKED!-231212     from a build with untracked files
//
char *tde0002()
{
    static char ver[60];
    wifi_msg_t  msg;

    strncpy(ver, "tde 0002.000", 59);
    ver[59] = 0;

    wifi_flush_msgs();

    int ret = wifi_send_timeout("AT+VER", K_NO_WAIT);
    if (ret != 0) {
        LOG_ERR("Failed to send at+ver %d", ret);
        return ver;
    }

    ret = wifi_recv(&msg, K_MSEC(1000));
    if (ret != 0) {
        LOG_ERR("Didn't receive a response %d", ret);
        return ver;
    }

    char *str = strstr(msg.data, "TDEVER_");
    if (str != NULL) {
        str += 7;
        memcpy(ver + 9, str, 3);
    }
    wifi_msg_free(&msg);    // free doesn't use len, so it fine to modify it
    return ver;
}

/////////////////////////////////////////////////////////
// tde0022
// “tde 0022 \r\n”
// Get the Wifi MAC address
// \d\a+WFMAC:D4:3D:39:E5:9C:08
//
char *tde0022()
{
    static char mac[60];
    int         ret = 0;
    wifi_msg_t  msg;

    k_timeout_t   timeout   = K_MSEC(500);
    k_timepoint_t timepoint = sys_timepoint_calc(timeout);

    // TODO  we need to tie requests to responses so we don't have to flush the queue
    wifi_flush_msgs();

    ret = wifi_send_timeout("at+wfmac=?", timeout);
    if (ret != 0) {
        LOG_ERR("Failed to send at cmd, ret = %d", ret);
        return "tde 0022.000000000000";
    }

    while (K_TIMEOUT_EQ(timeout, K_NO_WAIT) == false) {
        if ((ret = wifi_recv(&msg, timeout)) != 0) {
            LOG_ERR("Didn't receive a response to at+wfmac=?, ret= %d", ret);
            return "tde 0022.000000000000";
        }
        int addrs[6];
        int num = sscanf(
            msg.data, "\r\n+WFMAC:%X:%X:%X:%X:%X:%X", &addrs[0], &addrs[1], &addrs[2], &addrs[3], &addrs[4], &addrs[5]);
        if (num == 6) {
            snprintf(
                mac, 60, "tde 0022.%02X%02X%02X%02X%02X%02X", addrs[0], addrs[1], addrs[2], addrs[3], addrs[4], addrs[5]);
            wifi_msg_free(&msg);
            return mac;
        }
        timeout = sys_timepoint_timeout(timepoint);
    }
    LOG_ERR("Timed out waiting for response to at+wfmac=?");
    return "tde 0022.000000000000";
}

/////////////////////////////////////////////////////////
// tde0026
// tde 0026.XXXXXXXXXX.YYYYYYYYYY \r\n”
// Connect to a ssid
// “tde 0026.ZZZZZZZZZZZZZ \r\n”
//
// “ZZZZZZZZZZZZZ” is the IP address of wireless router.
// “ZZZZZZZZZZZZZ =192.168.100.1”,
// connection success and the WiFi IP address is “192.168.100.1”;
//
// “ZZZZZZZZZZZZZ =0”, connection fail.
char *tde0026(char *ssid, char *pass)
{
    static char result[30];
    int         ret;
    wifi_msg_t  msg;

    strncpy(result, "tde 0026.0000000000", 29);
    result[29]              = 0;
    k_timeout_t   timeout   = K_MSEC(6000);
    k_timepoint_t timepoint = sys_timepoint_calc(timeout);

    // EAS XXX TODO  we need to tie requests to responses so we don't have to flush the queue
    wifi_flush_msgs();

    ret     = wifi_initiate_connect_to_ssid(ssid, pass, 4, 0, 2, 0, timeout);
    timeout = sys_timepoint_timeout(timepoint);
    if ((ret != 0) || (K_TIMEOUT_EQ(timeout, K_NO_WAIT))) {
        return result;
    }

    // Wait for the +WFJAP:1,'AP_SSID',192.168.2.131
    while (1) {
        timeout = sys_timepoint_timeout(timepoint);
        if (K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
            LOG_ERR("Timeout waiting for connect msg");
            break;
        }
        if ((ret = wifi_recv(&msg, timeout)) != 0) {
            LOG_ERR("Didn't receive a +WFJAP in time. ret=%d", ret);
            break;
        }
        char *sub = strstr(msg.data, "+WFJAP:");
        char  ip[17];
        char  ssid[33];
        if (sub != NULL) {
            sscanf(sub, "+WFJAP:1,'%[^']',%s", ssid, ip);
            snprintf(result, 30, "tde 0026.%s", ip);
            wifi_msg_free(&msg);
            break;
        }
        wifi_msg_free(&msg);
    }
    return result;
}

/////////////////////////////////////////////////////////
// tde0027
// tde 0027
// Get signal strength
//
// AT+WFRSSI
// \r\l+RSSI:-46\r\n
// \r\lOK\r\n
// \r\nERROR:-400\r\n
//
// return: “tde 0026.-46 \r\n”
char *tde0027()
{
    static char result[30];
    int         ret;
    wifi_msg_t  msg;
    strncpy(result, "tde 0027.0", 29);
    result[29]              = 0;
    k_timeout_t   timeout   = K_MSEC(500);
    k_timepoint_t timepoint = sys_timepoint_calc(timeout);

    // EAS XXX TODO  we need to tie requests to responses so we don't have to flush the queue
    wifi_flush_msgs();

    ret     = wifi_send_timeout("AT+WFRSSI", timeout);
    timeout = sys_timepoint_timeout(timepoint);
    if ((ret != 0) || (K_TIMEOUT_EQ(timeout, K_NO_WAIT))) {
        return result;
    }

    // Wait for the result + OK or ERROR
    while (1) {
        timeout = sys_timepoint_timeout(timepoint);
        if (K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
            LOG_ERR("Timeout waiting for connect msg");
            break;
        }
        if ((ret = wifi_recv(&msg, timeout)) != 0) {
            LOG_ERR("Didn't receive a +WFJAP in time. ret=%d", ret);
            break;
        }
        char *sub = strstr(msg.data, "+RSSI:");
        char  rssi[17];
        if (sub != NULL) {
            sscanf(sub, "+RSSI:%s", rssi);
            snprintf(result, 30, "tde 0027.%s", rssi);
            wifi_msg_free(&msg);
        } else if (strstr(msg.data, "\r\nOK") != NULL) {
            wifi_msg_free(&msg);
            break;
        } else if (strstr(msg.data, "\r\nERROR") != NULL) {
            wifi_msg_free(&msg);
            break;
        }
    }
    return result;
}

/////////////////////////////////////////////////////////
// tde0028
// Get connected status
//
// “tde 0028.X \r\n”
// “X” is the status of WiFi.
// “X =1”, the WiFi has connected;
// “X =0”, the WiFi has disconnected.
//
// AT+WFSTAT
// +WFSTAT:softap1 mac_address=ec:9f:0d:9f:fa:65 wpa_state=DISCONNECTED
char *tde0028()
{
    static char result[30];
    int         ret;
    wifi_msg_t  msg;
    strncpy(result, "tde 0028.0", 29);
    result[29]              = 0;
    k_timeout_t   timeout   = K_MSEC(1500);
    k_timepoint_t timepoint = sys_timepoint_calc(timeout);

    // EAS XXX TODO  we need to tie requests to responses so we don't have to flush the queue
    wifi_flush_msgs();

    ret     = wifi_send_timeout("AT+WFSTAT", timeout);
    timeout = sys_timepoint_timeout(timepoint);
    if ((ret != 0) || (K_TIMEOUT_EQ(timeout, K_NO_WAIT))) {
        return result;
    }

    // Wait for the result + OK or ERROR
    while (1) {
        timeout = sys_timepoint_timeout(timepoint);
        if (K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
            LOG_ERR("Timeout waiting for connect msg");
            break;
        }
        if ((ret = wifi_recv(&msg, timeout)) != 0) {
            LOG_ERR("'%s'(%d) waiting for response to WFSTAT", wstrerr(-ret), ret);
            break;
        }
        char *sub = strstr(msg.data, "+WFSTAT:");
        if (sub != NULL) {
            sub = strstr(msg.data, "wpa_state=COMPLETED");
            if (sub != NULL) {
                strncpy(result, "tde 0028.1", 29);
                result[29] = 0;
            }
            wifi_msg_free(&msg);
        } else if (strstr(msg.data, "\r\nOK") != NULL) {
            wifi_msg_free(&msg);
            break;
        } else if (strstr(msg.data, "\r\nERROR") != NULL) {
            wifi_msg_free(&msg);
            break;
        }
    }
    return result;
}

/////////////////////////////////////////////////////////
// tde0058
// “tde 0058.d43d39e59c08 \r\n”
// Set the Wifi MAC address
// AT+WFMAC=EC:9F:0D:9F:FA:64
//
char *tde0058(char *newmac)
{
    int         macaddr[6];
    static char cmd[50];

    if (sscanf(
            newmac, "%02X:%02X:%02X:%02X:%02X:%02X", &macaddr[0], &macaddr[1], &macaddr[2], &macaddr[3], &macaddr[4], &macaddr[5])
        != 6) {
        LOG_ERR("Invalid mac address");
        return "tde 0058.000000000000";
    }

    wifi_flush_msgs();
    snprintf(
        cmd,
        50,
        "at+wfspf=%02X:%02X:%02X:%02X:%02X:%02X",
        macaddr[0],
        macaddr[1],
        macaddr[2],
        macaddr[3],
        macaddr[4],
        macaddr[5]);
    if (!wifi_send_ok_err_atcmd(cmd, NULL, K_MSEC(1000))) {
        LOG_ERR("At command failed");
        return "tde 0058.000000000000";
    }
    snprintf(
        cmd, 50, "tde 0058.%02X%02X%02X%02X%02X%02X", macaddr[0], macaddr[1], macaddr[2], macaddr[3], macaddr[4], macaddr[5]);
    return cmd;
}

/////////////////////////////////////////////////////////
// tde0060
// “tde 0060.
// “tde 0060.57 \r\n”
//
// Get the current crystal tuning
// Register range is 0x00 ~ 0x7F, and there is about 2 kHz deviation per register code.
//
char *tde0060()
{
    static char result[12] = "tde 0060.0";
    int         ret        = wifi_get_xtal(K_MSEC(1000));
    if (ret != -1) {
        snprintf(result, 12, "tde 0060.%d", ret);
    }
    return result;
}

/////////////////////////////////////////////////////////
// tde0061
// “tde 0061.60
// “tde 0061.60 \r\n”
//
// Set the current crystal tuning temporarily
// Register range is 0x00 ~ 0x7F, and there is about 2 kHz deviation per register code.
//
char *tde0061(int newval)
{
    static char result[20] = "tde 0061.00";
    if (newval > 0 && newval < 128) {
        int ret = wifi_set_xtal(newval, NULL, K_MSEC(1000));
        if (ret == 0) {
            snprintf(result, 20, "tde 0061.%d", newval);
        }
    }
    return result;
}

/////////////////////////////////////////////////////////
// tde0062
// “tde 0062.60
// “tde 0062.60 \r\n”
//
// Set the current crystal tuning permanently
// Register range is 0x00 ~ 0x7F, and there is about 2 kHz deviation per register code.
//
char *tde0062(int newval)
{
    static char result[20] = "tde 0062.00";
    if (newval > 0 && newval < 128) {
        int ret = wifi_set_otp_register(0x428, 1, newval, K_MSEC(1000));
        if (ret != -1) {
            snprintf(result, 20, "tde 0062.%d", newval);
        }
    }
    return result;
}

/////////////////////////////////////////////////////////
// tde0063
// “tde 0063.0 or tde 0063.1
// “tde 0062.0 \r\n”   error
// “tde 0062.1 \r\n”   success
//
// Set the DA into RF Test mode
//
char *tde0063(int start)
{
    static char result[20] = "tde 0063.0";
    if (start == 1) {
        int ret = wifi_start_XTAL_test();
        if (ret != -1) {
            return "tde 0063.1";
        }
    } else {
        wifi_stop_XTAL_test();
        return "tde 0063.1";
    }
    return result;
}
