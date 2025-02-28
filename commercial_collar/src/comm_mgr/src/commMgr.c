/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#include "commMgr.h"
#include "d1_zbus.h"
#include <net/mqtt_helper.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include "d1_json.h"
#include "modem_interface_types.h"
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/zbus/zbus.h>
#include "fota.h"
#include "uicr.h"
#include <cJSON_os.h>
#include "modem.h"
#include "wifi.h"
#include "wifi_at.h"
#include "net_mgr.h"
#include "imu.h"
#include "pmic.h"
#include "radioMgr.h"
#include <zephyr/fs/fs.h>
#include "log_telemetry.h"
#include <zephyr/sys/base64.h>
#include <zephyr/random/rand32.h>
#include "utils.h"
#include "app_version.h"
#include "ble.h"
#include "tracker_service.h"
#include "pmic_leds.h"
#include "wi.h"
#include <zephyr/sys/timeutil.h>
#include <string.h>
#if defined(CONFIG_ARCH_POSIX) && defined(CONFIG_EXTERNAL_LIBC)
#include <time.h>
#else
#include <zephyr/posix/time.h>
#endif
#ifdef CONFIG_ML_ENABLE
#include "ml.h"
#endif

/* Register log module */
LOG_MODULE_REGISTER(comm_mgr, CONFIG_COMM_MGR_LOG_LEVEL);

/* Register subscriber */
static void comm_mgr_listener(const struct zbus_channel *chan);
ZBUS_LISTENER_DEFINE(comm_mgr_sub, comm_mgr_listener);

int             g_quick_reconnect_cnt = 0;
extern uint64_t g_last_ssid_scan_time;
extern bool     g_radio_mgmt;
bool            g_send_shadow = false;

extern volatile modem_status_t modem_status_shadow;

static bool  usb_connected              = false;
static float battery_percentage         = 0.0;
bool         first_9160_status_recieved = false;

static K_SEM_DEFINE(nrt_sem, 0, 1);

static fmd_exit_t g_last_fmd_status = FMD_NOT_ACTIVE;

struct k_work_q commMgr_work_q;
K_THREAD_STACK_DEFINE(commMgr_stack_area, 16384);    // 8192

// All the S work related stuff
bool          g_comm_mgr_disable_S_work = CONFIG_IOT_DISABLE_S_WORK_DEFAULT;
struct k_work commMgr_S_work;
void          S_work_handler(struct k_work *work);
void          queue_S_work()
{
    k_work_submit_to_queue(&commMgr_work_q, &commMgr_S_work);
}
void S_timer_handler(struct k_timer *dummy)
{
    queue_S_work();
}
K_TIMER_DEFINE(S_work_timer, S_timer_handler, NULL);

void Fota_timer_handler(struct k_timer *dummy)
{
    fota_update_all_devices();
}
K_TIMER_DEFINE(Fota_work_timer, Fota_timer_handler, NULL);

typedef enum
{
    LTE_MQTT_NOT_INITIALIZED = 0,
    LTE_MQTT_INITIALIZING,
    LTE_MQTT_INITIALIZED,
} lte_mqtt_config_state_t;
static lte_mqtt_config_state_t modem_mqtt_config_state = LTE_MQTT_NOT_INITIALIZED;
static uint64_t                last_9160_uptime        = 0;

int             gS_val                = 0;
int             gT_val                = 0;
int             gT_count              = 0;
static uint64_t lte_status_count      = 0;
static uint64_t lte_zbus_status_count = 0;

void mqttQ_work_handler(struct k_work *work);
void handle_queue_mqttQ_work()
{
    workref_t *mqttQ_work = wr_get(NULL, __LINE__);
    if (mqttQ_work == NULL) {
        LOG_ERR("Failed to allocate memory for mqttQ work");
        return;
    }
    k_work_init(&mqttQ_work->work, mqttQ_work_handler);
    int ret = k_work_submit_to_queue(&commMgr_work_q, &mqttQ_work->work);
    if (ret <= 0) {
        wr_put(mqttQ_work);
        LOG_ERR("Failed to submit mqttQ work: %d", ret);
    }
}
void mqttQ_timer_handler(struct k_timer *dummy)
{
    handle_queue_mqttQ_work();
}
K_TIMER_DEFINE(mqttQ_work_timer, mqttQ_timer_handler, NULL);

bool g_comm_mgr_disable_Q_work = CONFIG_IOT_DISABLE_Q_WORK_DEFAULT;

K_FIFO_DEFINE(wifi_ssids_fifo);

typedef struct mqtt_msg
{
    struct k_work *work;
    char          *msg;
    int            len;
    uint8_t        topic;
    uint8_t        qos;
    uint8_t        priority;
} mqtt_msg_t;
K_MSGQ_DEFINE(mqttq, sizeof(mqtt_msg_t), 30, 4);
// MQTT telemetry message are often around 1k. There is a 2k limit. Alerts and such are < 200 bytes
// Telemetry message can be tossed if not sent timely.  Alerts mostly can't.
// So the mqtt msg heap should be around 20k to hold a decent number of messages
K_HEAP_DEFINE(mqtt_heap, 20 * 1024);

void WMD_work_handler(struct k_work *work);
typedef struct WMD_work_info
{
    struct k_work WMD_work;
    char          request_id[REQUEST_ID_SIZE];
} WMD_work_info_t;

WMD_work_info_t my_WMD_work_info;

void FMD_work_handler(struct k_work *work);
typedef struct FMD_work_info
{
    struct k_work FMD_work;
    uint16_t      duration;
    bool          state;
} FMD_work_info_t;

FMD_work_info_t my_FMD_work_info;

typedef struct nrfstatus_work_info
{
    workref_t     *nrfstatus_work;
    uint32_t       what_changed;
    modem_status_t status;
} nrfstatus_work_info_t;
// nrfstatus_work_info_t my_nrfstatus_work_info;
struct k_work nrfstatus_work;

typedef struct fotastatus_work_info
{
    struct k_work fotastatus_work;
    fota_status_t status;
} fotastatus_work_info_t;
fotastatus_work_info_t my_fotastatus_work_info;

typedef struct send_SRF_nonce_work_info
{
    struct k_work srf_nonce_work;
    bool          status;
} SRF_nonce_work_info_t;
SRF_nonce_work_info_t my_SRF_nonce_work_info;

typedef struct gps_work_info
{
    struct k_work gps_work;
    uint32_t      gps_poll_period;
} gps_work_info_t;
gps_work_info_t my_gps_work_info;

typedef struct bluetooth_adv_work_info
{
    struct k_work bluetooth_adv_work;
    uint32_t      bluetooth_adv_timeout;
    bool          start_ble_adv;
} bluetooth_adv_work_info_t;
bluetooth_adv_work_info_t my_bluetooth_adv_work_info;

typedef struct da_state_work_info
{
    workref_t *da_state_work;
    da_event_t evt;
} da_state_work_info_t;

typedef struct mqtt_work_info
{
    workref_t      *work;
    mqtt_payload_t *mqtt_msg;
} mqtt_work_info_t;

// FMD variables
static uint64_t fmd_start_time       = 0;
static uint64_t fmd_max_time_in_mins = 0;
static bool     is_in_fmd_mode       = false;
static uint64_t last_fmd_ssid_scan   = 0;

static uint64_t srf_nonce = 0;

shadow_doc_t shadow_doc = { .S_Norm                    = CONFIG_IOT_S_NORM_DEFAULT,
                            .S_FMD                     = CONFIG_IOT_S_FMD_DEFAULT,
                            .T_Norm                    = CONFIG_IOT_T_NORM_DEFAULT,
                            .T_FMD                     = CONFIG_IOT_T_FMD_DEFAULT,
                            .Rec                       = CONFIG_IOT_REC_VAR_DEFAULT,
                            .Q                         = CONFIG_IOT_Q_VAR_DEFAULT,
                            .mot_det                   = true,
                            .ths                       = CONFIG_LSM6DSV16X_D1_SLEEP_THRESHOLD,
                            .dur                       = CONFIG_LSM6DSV16X_D1_SLEEP_DURATION,
                            .fota_in_progress_duration = 60,
                            .gps_poll_period           = 0,
                            .zones                     = { { .idx = 0, .ssid = "", .safe = 0 },
                                                           { .idx = 1, .ssid = "", .safe = 0 },
                                                           { .idx = 2, .ssid = "", .safe = 0 },
                                                           { .idx = 3, .ssid = "", .safe = 0 },
                                                           { .idx = 4, .ssid = "", .safe = 0 } } };
void         da_state_work_handler(struct k_work *item);
////////////////////////////////////////////////////

int write_shadow_doc()
{
    struct fs_file_t shadow_file;
    int              ret;

    fs_file_t_init(&shadow_file);
    if ((ret = fs_open(&shadow_file, "/lfs1/shadow_doc.txt", FS_O_WRITE | FS_O_CREATE)) == 0) {
        char *shadow_json = json_shadow_report("MID", &shadow_doc, NULL);
        if (shadow_json != NULL) {
            ret = fs_write(&shadow_file, shadow_json, strlen(shadow_json));
            if (ret < 0) {
                LOG_ERR("Failed to write shadow doc to lfs: %s", wstrerr(-ret));
            }
        } else {
            LOG_ERR("Failed to create shadow json");
            ret = -ENOMEM;
        }
        fs_close(&shadow_file);
    } else {
        LOG_ERR("Cannot open/create shadow doc");
        ret = -ENOENT;
    }
    return ret;
}

static int read_shadow_doc()
{
    struct fs_file_t shadow_file;
    int              ret;

    fs_file_t_init(&shadow_file);
    if ((ret = fs_open(&shadow_file, "/lfs1/shadow_doc.txt", FS_O_READ | FS_O_CREATE)) == 0) {
        char *shadow_json = (char *)k_malloc(1000);
        if (shadow_json != NULL) {
            int amt = fs_read(&shadow_file, shadow_json, 1000);
            if (amt > 0) {
                ret = json_parse_shadow_report(shadow_json, &shadow_doc, &shadow_doc);
                if (ret != 0) {
                    LOG_ERR("Failed to parse shadow doc: %s", wstrerr(-ret));
                }
                k_free(shadow_json);
                // Storing versions in shadow is what we have to do to minimize
                // changes to the code but since the source of the version info is
                // the FW files, storing is only introdues the risk that we might
                // think the stored values are authoratative.  To avoid this as soon
                // as we read them out, we "correct" them by using the version info
                // from its sources.   So while storing version in the file is dumb,
                // it allows us to make the smallest change

                // the mvu version is correct. The wifi and lte version arrive
                // after' those chips start talking to the 5340
                shadow_doc.mcuVer[0] = APP_VERSION_MAJOR;
                shadow_doc.mcuVer[1] = APP_VERSION_MINOR;
                shadow_doc.mcuVer[2] = APP_VERSION_PATCH;
            } else {
                LOG_ERR("No data in shadow doc, initializing it from defaults");
                fs_close(&shadow_file);
                k_free(shadow_json);
                return write_shadow_doc();
            }
        } else {
            LOG_ERR("Failed to allocate memory for shadow json buf");
            ret = -ENOMEM;
        }
        fs_close(&shadow_file);
    } else {
        LOG_ERR("Cannot open/create shadow doc");
        ret = -ENOENT;
    }
    return ret;
}

bool status_getBit(int status, int pos)
{
    return (status >> pos) & 1;
}

void fota_status_update_work_handler(struct k_work *item)
{
    fotastatus_work_info_t *updatestatus = CONTAINER_OF(item, fotastatus_work_info_t, fotastatus_work);
    fota_status_update(updatestatus->status.status, updatestatus->status.percentage, updatestatus->status.device_type);
}

void gps_handler(struct k_work *item)
{
    gps_work_info_t *status = CONTAINER_OF(item, gps_work_info_t, gps_work);
    LOG_ERR("Status is %d", status->gps_poll_period);

    if (my_gps_work_info.gps_poll_period == shadow_doc.gps_poll_period) {
        LOG_ERR("No change to GPS status, nothing to do ");
        return;
    } else {
        if (status->gps_poll_period == 0) {
            LOG_ERR("Disabling GPS");
            int ret = modem_send_gps_disable();
            if (ret != 0) {
                LOG_ERR("'%s'(%d) while trying to disable GPS", wstrerr(-ret), ret);
            }
        } else {
            LOG_ERR("Enabling GPS");
            int ret = modem_send_gps_enable();
            if (ret != 0) {
                LOG_ERR("'%s'(%d) while trying to enable GPS", wstrerr(-ret), ret);
            }
        }
        shadow_doc.gps_poll_period = status->gps_poll_period;
        write_shadow_doc();
    }
}

void bluetooth_work_handler(struct k_work *item)
{
    bluetooth_adv_work_info_t *ble_work_obj = CONTAINER_OF(item, bluetooth_adv_work_info_t, bluetooth_adv_work);
    LOG_DBG("BLE CHANGE ON USB EVENT %d,%d", ble_work_obj->start_ble_adv, ble_work_obj->bluetooth_adv_timeout);
    if (ble_work_obj->start_ble_adv == true) {
        ble_advertise_start(ble_work_obj->bluetooth_adv_timeout);
    } else {
        ble_stop();
    }
}

static bool get_lte_connected()
{
    return status_getBit(modem_status_shadow.status_flags, STATUS_LTE_CONNECTED);
}

////////////////////////////////////////////////////
// commMgr_enable_S_work()
//  Enable or disable the S var work, like
// scanning for SSIDs, sending telemetry, etc
//
//  @param enable true to enable, false to disable
//
//  @return 0 on success, <0 on error
int commMgr_enable_S_work(bool enable)
{
    g_comm_mgr_disable_S_work = !enable;
    return 0;
}

////////////////////////////////////////////////////
// commMgr_enable_Q_work()
//  Enable or disable the work that sends mqtt msgs
// that are queued to send.
//
//  @param enable true to enable, false to disable
//
//  @return 0 on success, <0 on error
int commMgr_enable_Q_work(bool enable)
{
    g_comm_mgr_disable_Q_work = !enable;
    return 0;
}

////////////////////////////////////////////////////
// commMgr_queue_srf_nonce()
//  queue srf nonce one time per boot
//
//
//  @return 0 on success, <0 on error
static bool srf_nonce_sent = false;
void        commMgr_queue_srf_nonce()
{
    // check if we are provisioned

    int ret;

    LOG_DBG("Queuing srf nonce");

    char *machine_id = uicr_serial_number_get();
    if (srf_nonce == 0) {
        srf_nonce = abs(sys_rand32_get());
    }
    char *json = json_srf_nonce(machine_id, srf_nonce);
    if (json == NULL) {
        LOG_ERR("Failed to create srf nonce json");
        return;
    }

    ret = commMgr_queue_mqtt_message(json, strlen(json), MQTT_MESSAGE_TYPE_SRF_NONCE, 0, 5);
    if (ret != 0) {
        LOG_ERR("'%s'(%d) queueing srf nonce", wstrerr(-ret), ret);
    } else {
        LOG_INF("SRF nonce queue");
        srf_nonce_sent = true;
    }

    return;
}

void do_send_srf_nonce(const struct shell *sh, size_t argc, char **argv)
{
    commMgr_queue_srf_nonce();
}

static void calculate_S_and_T(bool is_in_fmd_mode)
{
    int T = shadow_doc.T_Norm;
    int S = shadow_doc.S_Norm;
    if (is_in_fmd_mode) {
        T = shadow_doc.T_FMD;
        S = shadow_doc.S_FMD;
    }
    // There are two situations where we want try to reconnect to wifi quicker
    // then the normal Rec value.
    //      The first is when we are on power. We should try to reconnect quickly
    // because the cost of doing so (power) is not in play.
    //     The second is when we just got disconnected for a reason that maybe
    // transitory, such as the dog walking behind something that blocks the signal
    // or the wifi router renewing a lease or something.  In this case we want to
    // try to reconnect a few times quickly before we resume the normal Rec period.
    // In this case, when we detect suspect disconnect, we set g_quick_reconnect_cnt
    // to 3 (or something) and each time we get a disconnect we decrement it. When
    // it reaches 0, we resume the normal Rec period.
    //
    // So in this routine, which calculates the, S and T values that are actually
    // used, we simply need to check if we are on USB power or if the
    // g_quick_reconnect_cnt is > 0 and if so, use a Rec value of 10 seconds
    int Rec = shadow_doc.Rec;
    if (g_usb_connected || g_quick_reconnect_cnt > 0) {
        Rec = 10;
    }

    // If we are not connected to Wifi, then set the S (SSID Scan) value
    // to the lower of the standard S value and the Rec (reconnect)
    // value because we need a current SSID scan to reconnect to wifi
    comm_device_type_t device = rm_get_active_mqtt_radio();
    if (device != COMM_DEVICE_DA16200) {
        if (device == COMM_DEVICE_NONE) {
            // If we aren't on LTE or Wifi, scan more often should only be at boot
            gS_val = 20;
        } else {
            gS_val = MIN(Rec, S);
        }

    } else {
        gS_val = S;
    }
    // The actual SSID scan rate may not be the S value the T value is
    // tied to so we calulate the T value needed to produce the same
    // result
    float temp = (float)S / (float)gS_val;
    gT_val     = (int)(temp * T);
    // We may have reduced gT_val to below gT_count, so we need to
    // make sure we don't exceed the new value
    if (gT_count > gT_val) {
        gT_count = gT_val;
    }
}

static void reset_S_timer(bool is_in_fmd_mode)
{
    int remaining = k_timer_remaining_get(&S_work_timer);
    k_timer_stop(&S_work_timer);
    calculate_S_and_T(is_in_fmd_mode);
    if (remaining == 0) {
        remaining = gS_val;
    }
    int newval = MIN(remaining, gS_val);
    k_timer_start(&S_work_timer, K_SECONDS(newval), K_NO_WAIT);
}

////////////////////////////////////////////////////
// commMgr_switched_to_wifi()
//  Called when the radio has switched to wifi
int commMgr_switched_to_wifi()
{
    int ret;

    reset_S_timer(is_in_fmd_mode);

    if (da_state.onboarded != DA_STATE_KNOWN_TRUE) {
        // We are not onboarded, send that alert
        LOG_WRN("Not Onboarded, sending onboard");
        ret = commMgr_queue_onboarding();
        if (ret != 0) {
            LOG_ERR("'%s'(%d) queueing onboarding alert", strerror(-ret), ret);
        } else {
            LOG_DBG("Queued onboarding alert");
        }
        return 0;
    }
    if (g_send_shadow == false) {
        commMgr_queue_shadow(NULL);
        g_send_shadow = true;
    }
    commMgr_queue_srf_nonce();

    return 0;
}

////////////////////////////////////////////////////
//  Called when the radio has switched to lte
int commMgr_switched_to_lte()
{
    int ret;
    reset_S_timer(is_in_fmd_mode);

    if (da_state.onboarded != DA_STATE_KNOWN_TRUE) {
        // We are not onboarded, send that alert
        LOG_WRN("Not Onboarded, sending onboard");
        ret = commMgr_queue_onboarding();
        if (ret != 0) {
            LOG_ERR("'%s'(%d) queueing onboarding alert", strerror(-ret), ret);
        } else {
            LOG_DBG("Queued onboarding alert");
        }
        return 0;
    }
    if (g_send_shadow == false) {
        commMgr_queue_shadow(NULL);
        g_send_shadow = true;
    }
    commMgr_queue_srf_nonce();

    return 0;
}

////////////////////////////////////////////////////
// commMgr_queue_telemetry()
//  queue telemetry msg for sending the cloud
// when able
//
//  @param include_ssids true to include the SSIDs in the
//         telemetry message

//  @return 0 on success, <0 on error
int commMgr_queue_telemetry(bool include_ssids)
{
    int                ret = 0;
    fuel_gauge_info_t  batt_info;
    radio_t            radio  = RADIO_TYPE_LTE;
    comm_device_type_t device = rm_get_active_mqtt_radio();

    char *machine_id = uicr_serial_number_get();

    // fill SSID fifo.   TODO: move this to a more global location, i.e. use a fifo right from
    // the start
    wifi_arr_t *list = NULL;
    if (include_ssids) {
        list = wifi_get_last_ssid_list();
        if (list != NULL && list->count > 0) {
            for (int i = 0; i < list->count; i++) {
                k_fifo_alloc_put(&wifi_ssids_fifo, &(list->wifi[i]));
            }
        }
    }

    fuel_gauge_get_latest(&batt_info);

    if (device == COMM_DEVICE_DA16200) {
        radio = RADIO_TYPE_WIFI;
    }

    // Send telemetry uses cached results
    char *json       = NULL;
    int   loop_count = 0;
    int   more_data  = 1;    // just wait, you'll see
    while (more_data > 0) {
        more_data = json_telemetry(
            &json,
            machine_id,
            batt_info,
            get_charging_active(),
            radio,
            da_state.version,
            da_state.ap_name,
            da_state.ap_safe == 1,
            usb_connected,
            &wifi_ssids_fifo,
            loop_count,
            da_state.rssi);
        loop_count++;
        if (json == NULL) {
            LOG_ERR("Failed to create telemetry json");
            goto tele_exit;
        }
        ret = commMgr_queue_mqtt_message(json, strlen(json), MQTT_MESSAGE_TYPE_INFO_TELEMETRY, 0, 20);
        if (ret != 0) {
            LOG_ERR("'%s'(%d) queueing telemetry", wstrerr(-ret), ret);
        } else {
            LOG_INF("Telemetry queued");
        }
    }

tele_exit:
    return ret;
}

////////////////////////////////////////////////////
// commMgr_queue_fmd_telemetry()
//  send fmd mode telemetry to the cloud using the active radio
//
//  @return 0 on success, <0 on error
int commMgr_queue_fmd_telemetry()
{
    int                ret = 0;
    fuel_gauge_info_t  batt_info;
    radio_t            radio  = RADIO_TYPE_LTE;
    comm_device_type_t device = rm_get_active_mqtt_radio();

    char       *machine_id = uicr_serial_number_get();
    wifi_arr_t *list       = NULL;
    list                   = wifi_get_last_ssid_list();

    if (list != NULL && list->count > 0) {
        for (int i = 0; i < list->count; i++) {
            k_fifo_alloc_put(&wifi_ssids_fifo, &(list->wifi[i]));
        }
    }

    fuel_gauge_get_latest(&batt_info);

    if (device == COMM_DEVICE_DA16200) {
        radio = RADIO_TYPE_WIFI;
    }

    // Send telemetry uses cached results
    int   cellID                 = 12345;
    int   trackingArea           = 12345;
    char  reqID[REQUEST_ID_SIZE] = "0";
    bool  in_safe_zone           = da_state.ap_safe == 1;
    char *json                   = NULL;
    int   loop_count             = 0;
    int   more_data              = 1;    // just wait, you'll see
    while (more_data > 0) {
        more_data = json_wheresmydog(
            &json, machine_id, &cellID, &trackingArea, da_state.ap_name, &in_safe_zone, reqID, true, &wifi_ssids_fifo, loop_count);
        loop_count++;

        if (json == NULL) {
            LOG_ERR("Failed to create FMD telemetry json");
            goto fmd_tele_exit;
        }

        ret = commMgr_queue_mqtt_message(json, strlen(json), MQTT_MESSAGE_TYPE_NEAR_REAL_TIME, 0, 10);
        if (ret != 0) {
            LOG_ERR("'%s'(%d) sending telemetry", wstrerr(-ret), ret);
        } else {
            LOG_INF("FMD Telemetry queued");
        }
    }

fmd_tele_exit:
    return ret;
}

////////////////////////////////////////////////////
// commMgr_queue_pairing_nonce()
//  queue a pairing nonce msg to send to the cloud
// when able
//
//  @param nonce the nonce to send
//
//  @return 0 on success, <0 on error
int commMgr_queue_pairing_nonce(char *nonce)
{
    int ret;

    LOG_DBG("Queuing pairing nonce");

    char         iccid_str[32] = "UNKNOWN";
    char         imei_str[32]  = "UNKNOWN";
    char         modelNum[32]  = "DT1A";
    modem_info_t info;
    if (modem_get_info(&info) == 0) {
        memset(iccid_str, 0, sizeof(iccid_str));
        memset(imei_str, 0, sizeof(imei_str));
        strcpy(iccid_str, info.iccid);
        strcpy(imei_str, info.imei);
    }

    char *machine_id = uicr_serial_number_get();
    char *json       = json_pair_msg(machine_id, nonce, iccid_str, imei_str, uicr_ble_mac_address_get(), modelNum);
    if (json == NULL) {
        LOG_ERR("Failed to create pairing json");
        return -EINVAL;
    }

    ret = commMgr_queue_mqtt_message(json, strlen(json), MQTT_MESSAGE_TYPE_NEAR_REAL_TIME, 0, 5);
    if (ret != 0) {
        LOG_ERR("'%s'(%d) sending pairing nonce", wstrerr(-ret), ret);
    } else {
        LOG_INF("Pairing nonce queued");
    }

    return ret;
}

////////////////////////////////////////////////////
// commMgr_queue_safe_zone_alert()
//
//  @return 0 on success, <0 on error
int commMgr_queue_safe_zone_alert(uint64_t time, int type, char *ssid, bool entering)
{
    int ret;

    LOG_DBG("Queuing safe zone alert");

    char *machine_id = uicr_serial_number_get();
    char *reason     = NULL;
    if (!entering) {
        reason = da_state.ap_disconnect_reason;
    }
    char *json = json_safe_zone_alert(machine_id, time, type, ssid, entering, reason);
    if (json == NULL) {
        LOG_ERR("Failed to create safe zone alert json");
        return -EINVAL;
    }

    ret = commMgr_queue_mqtt_message(json, strlen(json), MQTT_MESSAGE_TYPE_NEAR_REAL_TIME, 0, 10);
    if (ret != 0) {
        LOG_ERR("'%s'(%d) queuing safe zone alert", wstrerr(-ret), ret);
    } else {
        LOG_INF("Safe zone alert queued");
    }

    return ret;
}

////////////////////////////////////////////////////
// commMgr_queue_connectivity()
//  queue a connectivity msg for sendign to the cloud
// when able.
//
//  @param num_bytes the number of bytes to send
//
//  @return 0 on success, <0 on error
int commMgr_queue_connectivity(int num_bytes)
{
    int ret;

    LOG_DBG("Queueing connectivity msg of size %d", num_bytes);

    char *machine_id = uicr_serial_number_get();
    char *json       = json_connectivity_msg(machine_id, num_bytes);
    if (json == NULL) {
        LOG_ERR("Failed to create connectivity msg");
        return -EINVAL;
    }

    ret = commMgr_queue_mqtt_message(json, strlen(json), MQTT_MESSAGE_TYPE_CONN_TEST, 0, 30);
    if (ret != 0) {
        LOG_ERR("'%s'(%d) queueing connectivity", wstrerr(-ret), ret);
    } else {
        LOG_INF("Connectivity msg queued");
    }

    return ret;
}

////////////////////////////////////////////////////
// commMgr_queue_alert()
//  queue a alert msg for sending to cloud when able
//
//  @param sub_type the type of alert
//  @param msg the message to send
//
//  @return 0 on success, <0 on error
int commMgr_queue_alert(int sub_type, char *msg)
{
    int ret;

    LOG_DBG("Sending alert msg of type %d with |%s|", sub_type, msg);

    char *machine_id = uicr_serial_number_get();    // max 12 bytes
    char *json       = json_near_realtime_msg(machine_id, sub_type, msg);
    if (json == NULL) {
        LOG_ERR("Failed to create alert msg");
        return -EINVAL;
    }

    ret = commMgr_queue_mqtt_message(json, strlen(json), MQTT_MESSAGE_TYPE_NEAR_REAL_TIME, 0, 10);
    if (ret != 0) {
        LOG_ERR("'%s'(%d) queueing alert", wstrerr(-ret), ret);
    } else {
        LOG_INF("Alert msg queued");
    }

    return ret;
}

////////////////////////////////////////////////////
// send_temp_alert()
// queue a near real time alert for the temperature moving into a danger zone
//
// @param curr_status the current status (TOO_COLD/JUST_RIGHT/TOO_HOT)
//
//  @return 0 on success, <0 on error
static int send_temp_alert(goldilocks_t curr_status)
{
    int ret = 0;

    switch (curr_status) {
    case TOO_COLD:
        ret = commMgr_queue_alert(TEMPERATURE_SUB_TYPE, "T0:Warning: too cold for many dogs");
        LOG_ERR("Warning: too cold for many dogs");
        break;
    case JUST_RIGHT:
        ret = commMgr_queue_alert(TEMPERATURE_SUB_TYPE, "T1:Temperature is back to normal range");
        LOG_WRN("Temperature is back to normal range");
        break;
    case TOO_HOT:
        ret = commMgr_queue_alert(TEMPERATURE_SUB_TYPE, "T2:Warning: too hot for most dogs");
        LOG_ERR("Warning: too hot for many dogs");
        break;
    }
    return ret;
}

////////////////////////////////////////////////////
// send_shutdown_alert()
// queue a near real time alert for imminent emergency shutdown
//
//
//  @return 0 on success, <0 on error
static int send_shutdown_alert(void)
{
    int ret;

    ret = commMgr_queue_alert(TEMPERATURE_SUB_TYPE, "Shutting down");
    // the next thing to happen is entering ship mode ... but we need to wait for the alert to
    // get out. But don't wait too long!
    if (ret == 0) {
        ret = k_sem_take(&nrt_sem, K_SECONDS(10));
    }
    return ret;
}

///////////////////////////////////////////////////
// commMgr_queue_onboarding()
//  queue a onboarding msg for sending to the cloud
// when able
//
//  @return 0 on success, <0 on error
int commMgr_queue_onboarding()
{
    int ret;

    LOG_DBG("Queueing onboarding msg");

    char *machine_id = uicr_serial_number_get();
    char *json       = json_onboarding(machine_id, "Test GWINFO D1");
    if (json == NULL) {
        LOG_ERR("Failed to create onboarding msg");
        return -EINVAL;
    }

    ret = commMgr_queue_mqtt_message(json, strlen(json), MQTT_MESSAGE_TYPE_ONBOARDING, 0, 1);
    if (ret != 0) {
        LOG_ERR("'%s'(%d) sending onboarding", wstrerr(-ret), ret);
    } else {
        LOG_INF("Onboarding msg queued");
    }

    return ret;
}

///////////////////////////////////////////////////
// commMgr_queue_shadow()
//  queue a shadow message to be sent to the cloud
// using the when able. THis will report our current
// settings.   If extra is provided then
// the variable definitions in that array will
// also be sent.  THis is to allow for deleting
// old variables from the IoT Hub
//
//  @return 0 on success, <0 on error
int commMgr_queue_shadow(shadow_extra_t *extra)
{
    int                ret;
    comm_device_type_t device = rm_get_active_mqtt_radio();

    LOG_DBG("Sending shadow msg of over %s", comm_dev_str(device));

    char *machine_id = uicr_serial_number_get();
    char *json       = json_shadow_report(machine_id, &shadow_doc, extra);
    if (json == NULL) {
        LOG_ERR("Failed to create shadow msg");
        return -EINVAL;
    }

    ret = commMgr_queue_mqtt_message(json, strlen(json), MQTT_MESSAGE_TYPE_SHADOW_PROXY, 0, 20);
    if (ret != 0) {
        LOG_ERR("'%s'(%d) sending shadow", wstrerr(-ret), ret);
    } else {
        LOG_INF("Shadow msg queued on %s", comm_dev_str(device));
    }

    return ret;
}

////////////////////////////////////////////////////
// handle_conn_test_message()
//  process incoming connectivity mqtt messages
// from cloud
//
//  @param msg mqtt_payload_t
//
//  @return void
void handle_conn_test_message(char *pl)
{
    LOG_DBG("Received CONN Test response");
    LOG_HEXDUMP_DBG(pl, strlen(pl), "Connectivity resp:");
}

////////////////////////////////////////////////////
// handle_onboarding_message()
//  process incoming mqtt messages from cloud
//
//  @param msg mqtt_payload_t
//
//  @return void
int handle_onboarding_message(cJSON *m)
{
    cJSON *m_st = cJSON_GetObjectItem(m, "ST");
    if (m_st == NULL) {
        LOG_ERR("Failed to get ST from onboarding response");
        return -1;
    }

    if (cJSON_IsString(m_st) && strstr(m_st->valuestring, "OK")) {
        // We received a valid onboarding response
        if (rm_prepare_radio_for_use(COMM_DEVICE_DA16200, false, K_SECONDS(3)) == false) {
            LOG_ERR("Can't wake up DA to write ONBOARDING flag to!!!!");
            return -1;
        }
        das_tri_state_t old = da_state.onboarded;

        int ret = net_set_saved_bool(
            DA_NV_ONBOARDED_ADDR, &(da_state.onboarded), DA_EVENT_TYPE_ONBOARDED, DA_STATE_KNOWN_TRUE);
        if (ret != 0) {
            LOG_ERR("Failed to save onboarded state to DA");
        } else {
            if (old != DA_STATE_KNOWN_TRUE) {
                if (rm_get_active_mqtt_radio() == COMM_DEVICE_DA16200) {
                    g_radio_mgmt = false;
                    ret          = wifi_set_mqtt_state(0, K_MSEC(500));
                    if (ret != 0) {
                        LOG_ERR("'%s' (%d) to set mqtt state to 0 after onboarding", wstrerr(-ret), ret);
                        pmic_reboot("Onboarding");
                    }
                    int topics[15];
                    topics[0]      = MQTT_MESSAGE_TYPE_ONBOARDING;
                    topics[1]      = MQTT_MESSAGE_TYPE_FOTA;
                    topics[2]      = MQTT_MESSAGE_TYPE_REMOTE_FUNCTION;
                    topics[3]      = MQTT_MESSAGE_TYPE_CONN_TEST;
                    topics[4]      = MQTT_MESSAGE_TYPE_SHADOW_PROXY;
                    topics[5]      = MQTT_MESSAGE_TYPE_SRF_NONCE;
                    topics[6]      = MQTT_MESSAGE_TYPE_SRF_FUNC;
                    topics[7]      = MQTT_MESSAGE_TYPE_CONFIG_HUB;
                    int num_topics = 8;
                    if ((ret = wifi_set_mqtt_sub_topics_by_type(topics, num_topics, K_MSEC(500))) != 0) {
                        LOG_ERR(
                            "'%s'(%d) setting MQTT sub topics after "
                            "onboarding",
                            wstrerr(-ret),
                            ret);
                    }
                    ret = wifi_set_mqtt_state(1, K_MSEC(500));
                    if (ret != 0) {
                        LOG_ERR("'%s' (%d) to set mqtt state to 0 after onboarding", wstrerr(-ret), ret);
                        pmic_reboot("Onboarding");
                    }
                    g_radio_mgmt = true;
                } else {
                    LOG_ERR("Onboarded over LTE");
                    // on LTE, ok to reboot
                    pmic_reboot("Onboarded OK");
                }
            } else {
                LOG_DBG("Got Onboard, was already onboarded");
            }
            // Either we rebooted or we were already onboarded
            // no need to publish and event
        }
        rm_done_with_radio(COMM_DEVICE_DA16200);
    } else {
        LOG_ERR("Failed to onboard: %s", m_st->valuestring);
    }
    return 0;
}

///////////////////////////////////////////////////////////
// commMgr_set_vars()
//  Set the S and T related variables which affects
// the time between when we get SSIDs, telementry
// and AP reconnect
//
//  S_ values are in seconds between 15 < S < 36000
//  T_ values are in numbers of S periods between 1 < T < 10000
//  REC is in seconds between 15 < REC < 36000
//  Q is in seconds between 15 < Q < 10000
//  @param S_Norm the time between SSID scans in normal mode
//         or 0 for no change
//  @param S_FMD the time between SSID scans in FMD mode or
//         0 for no change
//  @param T_Norm number of S periods between telemetry send
//          in Normal mode or 0 for no change
//  @param T_FMD number of S periods between telemetry send
//          in FMD mode or 0 for no change
//  @param Rec the new period in seconds between reconnects
//          checks when on LTE or 0 for no change
//  @param Q the time between attempts to send queued mqtt
//          messages or 0 for no change
//  @param save true to save the new values to flash
//
//  @return 0 on success, <0 on error
int commMgr_set_vars(int S_Norm, int S_FMD, int T_Norm, int T_FMD, int Rec, int Q, bool save)
{
    if (S_Norm == 0) {
        S_Norm = shadow_doc.S_Norm;
    }
    if (S_FMD == 0) {
        S_FMD = shadow_doc.S_FMD;
    }
    if (T_Norm == 0) {
        T_Norm = shadow_doc.T_Norm;
    }
    if (T_FMD == 0) {
        T_FMD = shadow_doc.T_FMD;
    }
    if (Rec == 0) {
        Rec = shadow_doc.Rec;
    }
    if (Q == 0) {
        Q = shadow_doc.Q;
    }

    if (S_Norm < 10 && S_Norm > 65534) {
        LOG_ERR("Invalid commMgr S_Norm value: %d", S_Norm);
        return -EINVAL;
    }
    if (S_FMD < 10 && S_FMD > 65534) {
        LOG_ERR("Invalid commMgr S_FMD value: %d", S_FMD);
        return -EINVAL;
    }
    if (T_Norm < 1 && T_Norm > 100) {
        LOG_ERR("Invalid commMgr T_Norm value: %d", T_Norm);
        return -EINVAL;
    }
    if (T_FMD < 1 && T_FMD > 100) {
        LOG_ERR("Invalid commMgr T_FMD value: %d", T_FMD);
        return -EINVAL;
    }
    if (Rec < 10 && Rec > 65534) {
        LOG_ERR("Invalid commMgr Rec value: %d", Rec);
        return -EINVAL;
    }
    if (Q < 1 && Q > 65534) {
        LOG_ERR("Invalid commMgr Q value: %d", Q);
        return -EINVAL;
    }

    int old_S_Norm    = shadow_doc.S_Norm;
    int old_S_FMD     = shadow_doc.S_FMD;
    shadow_doc.S_Norm = S_Norm;
    shadow_doc.S_FMD  = S_FMD;
    shadow_doc.T_Norm = T_Norm;
    shadow_doc.T_FMD  = T_FMD;
    shadow_doc.Rec    = Rec;
    shadow_doc.Rec    = Rec;

    if (save) {
        write_shadow_doc();
    }

    if (S_Norm != old_S_Norm || S_FMD != old_S_FMD) {
        reset_S_timer(is_in_fmd_mode);
        LOG_DBG(
            "Changed S_Norm %d | S_FMD %d | T_Norm %d | T_SMD %d | Rec %d | Q %d",
            shadow_doc.S_Norm,
            shadow_doc.S_FMD,
            shadow_doc.T_Norm,
            shadow_doc.T_FMD,
            shadow_doc.Rec,
            shadow_doc.Q);
    }
    return 0;
}

////////////////////////////////////////////////////
// FMD_work_handler()
//
void FMD_work_handler(struct k_work *item)
{
    FMD_work_info_t *info = CONTAINER_OF(item, FMD_work_info_t, FMD_work);
    if (info->state == 1) {
        enable_fmd_mode(info->duration);
    } else {
        disable_fmd_mode(FMD_CLOUD_REQUEST);
    }
}

////////////////////////////////////////////////////
// WMD_work_handler()
//
void WMD_work_handler(struct k_work *item)
{
    WMD_work_info_t *info             = CONTAINER_OF(item, WMD_work_info_t, WMD_work);
    char            *request_id       = info->request_id;
    char            *json             = NULL;
    char            *machine_id       = uicr_serial_number_get();
    wifi_arr_t      *data             = NULL;
    int             *cellid           = NULL;
    int             *tracking_area    = NULL;
    char            *dog_ssid         = NULL;
    bool            *dog_in_safe_zone = NULL;
    bool             doginsafe        = true;
    radio_t          radio            = RADIO_TYPE_WIFI;
    int              ret;

    if (rm_prepare_radio_for_use(COMM_DEVICE_DA16200, true, K_SECONDS(3)) == false) {
        LOG_ERR("Failed to prepare radio");
        return;
    }

    if (rm_is_wifi_enabled()) {
        // Scan for a new list if older then 20 seconds
        ret = wifi_refresh_ssid_list(true, 20, K_MSEC(1500));
        if (ret != 0) {
            LOG_ERR("'%s'(%d) when scanning for SSIDs for WMD", wstrerr(-ret), ret);
        }

        rm_done_with_radio(COMM_DEVICE_DA16200);
    } else {
        LOG_ERR("Wifi use is disabled, skipping ssid refresh");
        return;
    }

    if (da_state.ap_connected == DA_STATE_KNOWN_TRUE) {
        // in this case, all we send back is the AP name and the safe status
        dog_ssid         = da_state.ap_name;
        dog_in_safe_zone = &doginsafe;
        doginsafe        = (da_state.ap_safe == DA_STATE_KNOWN_TRUE);
        if (!doginsafe) {
            data = wifi_get_last_ssid_list();
        }
    }

    int cell = 0;
    int ta   = 0;
    if (get_lte_connected()) {
        // LTE Radio
        // EAS TODO: Unfake the data next
        cell          = 123456;
        ta            = 564321;
        cellid        = &cell;
        tracking_area = &ta;
        data          = wifi_get_last_ssid_list();
        radio         = RADIO_TYPE_LTE;
    }

    if (data != NULL && data->count > 0) {
        for (int i = 0; i < data->count; i++) {
            k_fifo_alloc_put(&wifi_ssids_fifo, &(data->wifi[i]));
        }
    }

    int loop_count = 0;
    ret            = 1;    // just wait, you'll see
    int more_data  = 1;
    while (more_data > 0) {
        more_data = json_wheresmydog(
            &json, machine_id, cellid, tracking_area, dog_ssid, dog_in_safe_zone, request_id, false, &wifi_ssids_fifo, loop_count);
        loop_count++;

        const char *rname = comm_dev_str(rm_get_active_mqtt_radio());
        ret               = commMgr_queue_mqtt_message(json, strlen(json), MQTT_MESSAGE_TYPE_NEAR_REAL_TIME, 0, 10);
        if (ret == 0) {
            LOG_DBG("WMD sent on %s", rname);
        } else {
            if (ret == -ENOTCONN) {
                LOG_WRN("Can't send WMD on %s because MQTT is not connected", rname);
            } else {
                LOG_ERR("'%s'(%d) sending WMD on %s", wstrerr(-ret), ret, rname);
            }
        }
    }
}

////////////////////////////////////////////////////
// handle_shadow_message()
//  process incoming mqtt messages from cloud
//
//  @param msg mqtt_payload_t
//
//  @return void
int handle_shadow_message(char *payload, radio_t radio)
{
    int          ret = 0;
    shadow_doc_t new_doc;

    cJSON *mObj;
    cJSON *json = cJSON_Parse(payload);
    if (json != NULL) {
        if ((mObj = cJSON_GetObjectItem(json, "M")) == NULL) {
            cJSON_Delete(json);
            ret = -EINVAL;
            goto handle_shadow_exit;
        }
    }
    cJSON_Delete(json);
    LOG_DBG("Received shadow message |%s|", payload);
    ret = json_parse_shadow_report(payload, &new_doc, &shadow_doc);
    if (ret != 0) {
        LOG_ERR("'%s'(%d) parsing shadow report", wstrerr(-ret), ret);
        goto handle_shadow_exit;
    }
    // If the incoming document doesn't have any changes to any vars we actually
    // care about, we should not return an unchanged shadow to the backend otherwise
    // the backend will re-send the deltas.  Since some units have deprecated vars
    // we would get into a loop
    bool doc_changed = false;

    if (new_doc.S_Norm == shadow_doc.S_Norm && new_doc.S_FMD == shadow_doc.S_FMD && new_doc.T_Norm == shadow_doc.T_Norm
        && new_doc.T_FMD == shadow_doc.T_FMD && new_doc.Rec == shadow_doc.Rec && new_doc.Q == shadow_doc.Q) {
        LOG_DBG("No changes to shadow comms vars");
    } else {
        doc_changed = true;
        ret =
            commMgr_set_vars(new_doc.S_Norm, new_doc.S_FMD, new_doc.T_Norm, new_doc.T_FMD, new_doc.Rec, new_doc.Q, true);
    }
    if (new_doc.ths != shadow_doc.ths) {
        doc_changed = true;
        imu_set_threshold(new_doc.ths);
        shadow_doc.ths = new_doc.ths;
    }
    if (new_doc.mot_det != shadow_doc.mot_det) {
        doc_changed = true;
        imu_set_mot_det(new_doc.mot_det);
        shadow_doc.mot_det = new_doc.mot_det;
    }
    if (new_doc.dur != shadow_doc.dur) {
        doc_changed = true;
        imu_set_duration(new_doc.dur);
        shadow_doc.dur = new_doc.dur;
    }

    if (new_doc.fota_in_progress_duration != shadow_doc.fota_in_progress_duration) {
        doc_changed = true;
        ret         = fota_set_in_progress_timer(new_doc.fota_in_progress_duration, true);
    }

    for (int i = 0; i < NUM_ZONES; i++) {
        if (strncmp(new_doc.zones[i].ssid, shadow_doc.zones[i].ssid, 32) != 0) {
            if (new_doc.zones[i].ssid[0] == 0) {
                ret = wifi_saved_ssids_del(i, K_MSEC(500));
                if (ret != 0) {
                    LOG_WRN("'%s'(%d) deleting zone %d", wstrerr(-ret), ret, i);
                    doc_changed = false;
                } else {
                    doc_changed                 = true;
                    shadow_doc.zones[i].ssid[0] = 0;
                    shadow_doc.zones[i].safe    = new_doc.zones[i].safe;
                }
            } else {
                LOG_ERR(
                    "incoming shadow safe zone name was changed which is not "
                    "allowed!");
            }
        } else {
            // The SSID is the same, so just update the safe flag
            if (shadow_doc.zones[i].safe != new_doc.zones[i].safe) {
                ret = wifi_set_zone_safe(i, new_doc.zones[i].safe, K_MSEC(1000));
                if (ret != 0) {
                    LOG_ERR("'%s'(%d) changing zone safe flag to %d", wstrerr(-ret), ret, new_doc.zones[i].safe);
                    doc_changed = false;
                } else {
                    doc_changed              = true;
                    shadow_doc.zones[i].safe = new_doc.zones[i].safe;
                }
            }
        }
    }

    if (doc_changed) {
        ret = write_shadow_doc();
        if (ret < 0) {
            LOG_ERR("'%s'(%d) saving shadow doc", wstrerr(-ret), ret);
        }
        ret = commMgr_queue_shadow(NULL);
    }
handle_shadow_exit:
    return ret;
}

int queue_srf_response(char *rid, char *err, char *errmsg)
{
    int ret;

    LOG_DBG("Queueing SRF response");

    uint64_t currTime = commMgr_get_unix_time();
    char    *json     = json_srf_response_message(rid, err, errmsg, currTime);
    if (json == NULL) {
        LOG_ERR("Failed to create SRF response json");
        return -EINVAL;
    }

    ret = commMgr_queue_mqtt_message(json, strlen(json), MQTT_MESSAGE_TYPE_SRF_FUNC, 0, 5);
    if (ret != 0) {
        LOG_ERR("'%s'(%d) to queue SRF response", wstrerr(-ret), ret);
    } else {
        LOG_INF("SRF response queued");
    }

    return ret;
}

int handle_srf_func_message(cJSON *m)
{
    int decoded_len = 0;

    cJSON *sm_st = cJSON_GetObjectItem(m, "SM");
    if (sm_st == NULL) {
        LOG_ERR("Failed to get SM from SRF response");
        return -1;
    }

    if (cJSON_IsString(sm_st)) {
        // We received a valid SRF response
        char  header[256];
        char *period      = strchr(sm_st->valuestring, '.');
        int   header_len  = period - sm_st->valuestring;
        char *payload     = period + 1;
        char *period2     = strchr(payload, '.');
        int   payload_len = period2 - payload;

        LOG_WRN("SRF header: %.*s", header_len, sm_st->valuestring);
        int ret = base64_decode(header, 256, &decoded_len, sm_st->valuestring, header_len);
        if (ret != 0) {
            LOG_ERR("Failed to decode SRF header");
            return -1;
        }

        ret = base64_decode(
            NULL, 0, &decoded_len, payload, payload_len);    // passing NULL just sets decoded_len to the size it would be

        decoded_len           = decoded_len + 32;    // pad it a bit
        char *payload_decoded = (char *)k_malloc(decoded_len);
        if (payload_decoded == NULL) {
            LOG_ERR("Failed to allocate memory for SRF payload");
            return -1;
        }

        int decode_len2;
        memset(payload_decoded, 0, decoded_len);
        ret = base64_decode(payload_decoded, decoded_len, &decode_len2, payload, payload_len);
        if (ret != 0) {
            LOG_ERR("Failed to decode SRF payload");
            k_free(payload_decoded);
            return -1;
        }

        // TODO:  Hack here, for some reason the decode is not getting the last 3 characters
        //        so I am faking them, luckily they are not super important, as long as the
        //        order of the json doesnt change.   Really strange behavior
        strncat(payload_decoded, "0}", 2);

        LOG_DBG("SRF decoded: %d, %s", decode_len2, payload_decoded);
        srf_data_t srf_data;
        ret = json_parse_srf(payload_decoded, &srf_data);
        if (ret != 0) {
            LOG_ERR("Failed to parse SRF response");
            k_free(payload_decoded);
            return -1;
        }

        // do the "security things"
        // check the nonce
        if (srf_nonce != srf_data.nonce) {
            LOG_ERR("SRF Nonce mismatch: %llu != %llu", srf_nonce, srf_data.nonce);
            k_free(payload_decoded);
            return -1;
        }

        // check the target device id
        char *machine_id = uicr_serial_number_get();
        char *underscore = strchr(srf_data.aud, '_');
        if (strcmp(machine_id, underscore + 1) != 0) {
            LOG_ERR("SRF Target device mismatch: %s != %s", machine_id, underscore + 1);
            k_free(payload_decoded);
            return -1;
        }

        // check the expiration time stamp
        if (srf_data.exp < commMgr_get_unix_time()) {
            LOG_ERR("SRF Expired: %llu < %llu", srf_data.exp, k_uptime_get());
            k_free(payload_decoded);
            return -1;
        }

        // OK: The request has been successfully executed
        // ERROR: The request reached the device, but it was not able to successfully
        // execute it. Reasons of failing are optionally reported in fres attribute EXPIRED:
        // The incoming request is expired (exp attribute is less than the current time)
        // MEMORY: The request cannot be executed due to lack of memory.

        switch (srf_data.command) {
        case SRF_COMMAND_WHERE_IS_MY_DOG:
            LOG_WRN("SRF Where is my dog: %s", srf_data.param1);
            if (srf_data.param1[0] != 0) {
                strncpy(my_WMD_work_info.request_id, srf_data.param1, REQUEST_ID_SIZE - 1);
                my_WMD_work_info.request_id[REQUEST_ID_SIZE - 1] = 0;
                ret = k_work_submit_to_queue(&commMgr_work_q, &my_WMD_work_info.WMD_work);
                if (ret <= 0) {
                    LOG_ERR("Failed to submit WMD work: %d", ret);
                }
                queue_srf_response(srf_data.rid, "OK", "Where is my dog command received");
            }
            break;
        case SRF_COMMAND_FIND_MY_DOG:
            LOG_WRN("SRF Find my Dog: %s", srf_data.param1);
            int fmd_state             = atoi(srf_data.param1);
            my_FMD_work_info.state    = 0;
            my_FMD_work_info.duration = 0;

            if (fmd_state > 0) {
                my_FMD_work_info.state    = fmd_state;
                int fmd_period            = atoi(srf_data.param2);
                my_FMD_work_info.duration = fmd_period;
            }
            k_work_submit_to_queue(&commMgr_work_q, &my_FMD_work_info.FMD_work);
            char msg[64];
            sprintf(
                msg, "Find my dog command received: Dog in Safe Zone = %d", (da_state.ap_connected && da_state.ap_safe));
            queue_srf_response(srf_data.rid, "OK", msg);
            break;
        case SRF_COMMAND_REBOOT:
            int reboot_delay_in_sec = atoi(srf_data.param1);
            if (reboot_delay_in_sec < 10) {
                reboot_delay_in_sec = 10;
            } else if (reboot_delay_in_sec > 3600) {
                reboot_delay_in_sec = 3600;
            }
            LOG_WRN("SRF Reboot in %d seconds", reboot_delay_in_sec);
            queue_srf_response(srf_data.rid, "OK", "Rebooting");
            k_sleep(K_SECONDS(reboot_delay_in_sec));
            pmic_reboot("SRF");
            break;
        case SRF_COMMAND_CHECK_FOTA:
            LOG_WRN("SRF FOTA check recieved");
            fota_update_all_devices();
            queue_srf_response(srf_data.rid, "OK", "FOTA command received");
            break;
        case SRF_COMMAND_GPS_ENABLE:
            LOG_WRN("SRF GPS: period=%s", srf_data.param1);
            my_gps_work_info.gps_poll_period = atoi(srf_data.param1);
            ret                              = k_work_submit_to_queue(&commMgr_work_q, &(my_gps_work_info.gps_work));
            if (ret <= 0) {
                LOG_ERR("Failed to submit GPS work: %d", ret);
            }
            queue_srf_response(srf_data.rid, "OK", "GPS enable/disable received");
            break;
        case SRF_COMMAND_FACTORY_RESET:
            LOG_WRN("SRF Factory Reset received");
            if (rm_get_active_mqtt_radio() == COMM_DEVICE_DA16200) {
                rm_switch_to(COMM_DEVICE_NRF9160, false, false);
            }
            int ret = wifi_saved_ssids_del_all(K_MSEC(3000));
            if (ret != 0) {
                LOG_ERR("'%s'(%d) returned from wifi_saved_ssids_del_all in factory reset", wstrerr(-ret), ret);
            } else {
                static shadow_zone_t fromDA[NUM_ZONES];    // static to keep off stack
                ret = wifi_get_ap_list(fromDA, K_MSEC(3000));
                if (ret != 0) {
                    LOG_ERR("'%s'(%d) returned from wifi_get_ap_list in factory reset", wstrerr(-ret), ret);
                    // not much we can do here but print
                }
            }
            if (ret == 0) {
                ret = queue_srf_response(srf_data.rid, "OK", "Factory Reset received");
            } else {
                char err[64];
                sprintf(err, "'%20s'(%d) executing Factory Reset", wstrerr(-ret), ret);
                ret = queue_srf_response(srf_data.rid, "ERROR", err);
            }
            if (ret != 0) {
                LOG_ERR("'%s'(%d) returned from queue_srf_response in factory reset", wstrerr(-ret), ret);
            }
            break;
        case SRF_COMMAND_NOOP:
            LOG_TELEMETRY_WRN(1, "SRF Noop received");
            queue_srf_response(srf_data.rid, "OK", "Noop command received");
            break;
        default:
            LOG_WRN("SRF UNKNOWN: %d", srf_data.command);
            queue_srf_response(srf_data.rid, "ERROR", "Unknown command");
            break;
        }

        k_free(payload_decoded);
    } else {
        LOG_ERR("Failed to find SM in SRF Func message");
    }
    return 0;
}

////////////////////////////////////////////////////
// handle_mqtt_cloud_to_dev_message()
//  process incoming mqtt messages from cloud
//
//  @param msg mqtt_payload_t
//
//  @return void
char hmcstart[256];
char hmcend[512];
void handle_mqtt_cloud_to_dev_message(struct k_work *work)
{
    workref_t        *wr   = CONTAINER_OF(work, workref_t, work);
    mqtt_work_info_t *info = (mqtt_work_info_t *)wr->reference;
    mqtt_payload_t   *msg  = info->mqtt_msg;
    // log_panic();
    // printk("Received message %p from cloud(%d): %s\n", msg->user_data, msg->payload_length,
    // msg->payload); int offset = 450; snprintf(hmcstart, 128, "%s", msg->payload);
    // snprintf(hmcend, offset, "%s", (msg->payload+(msg->payload_length-offset-1)));
    // LOG_DBG("Received message from cloud(%d): %.*s...%.*s", new_mqtt.payload_length, 256,
    // start, 256, end);

    // LOG_DBG("Received message from cloud(%d): [%d/%d] ^%s...%s^", msg->payload_length,
    // msg->payload[msg->payload_length-2], msg->payload[msg->payload_length-1], hmcstart,
    // hmcend); LOG_DBG("Received message from cloud of len %d:\r\n", msg->payload_length);

    LOG_DBG("Received MQTT message from cloud on %s (%d)", comm_dev_str(msg->radio), msg->payload_length);

    // break down the outter wrapper, get the type and send the rest for specific processing
    cJSON *root = json_parse_mqtt_string(msg->payload);
    if (root == NULL) {
        strncpy(hmcstart, msg->payload, 255);
        hmcstart[255] = 0;
        LOG_ERR("Failed to parse message from cloud |%s|", hmcstart);
        goto cleanup;
    }

    cJSON *type = cJSON_GetObjectItem(root, "T");    // do we need to check this against the type in the topic?
    if (type == NULL) {
        LOG_ERR("Failed to get type from message");
        LOG_HEXDUMP_DBG(msg->payload, msg->payload_length, "Received message:");
        goto cleanup;
    }

    cJSON *m = cJSON_GetObjectItem(root, "M");
    if (m == NULL) {
        LOG_ERR("Failed to get M-message from message");
        goto cleanup;
    }

    LOG_DBG("Received message from cloud: type = %d", type->valueint);
    switch (type->valueint) {
    case MQTT_MESSAGE_TYPE_FOTA:    // FOTA
        handle_fota_message(m);
        break;
    case MQTT_MESSAGE_TYPE_CONN_TEST:    // CONN Test
        handle_conn_test_message(msg->payload);
        break;
    case MQTT_MESSAGE_TYPE_ONBOARDING:    // OnBoarding
        handle_onboarding_message(m);
        break;
    case MQTT_MESSAGE_TYPE_SHADOW_PROXY:    // OnBoarding
        handle_shadow_message(msg->payload, msg->radio);
        break;
    case MQTT_MESSAGE_TYPE_SRF_FUNC:    // SRF Func
        handle_srf_func_message(m);
        break;
    default:
        LOG_ERR("Unknown message type: %d", type->valueint);
        break;
    }
cleanup:
    if (root) {
        cJSON_Delete(root);
    }

    if (msg) {
        if (msg->radio == COMM_DEVICE_DA16200) {
            wifi_msg_free((wifi_msg_t *)(msg->user_data));
            k_free(msg->user_data);
        } else {
            if (msg->payload) {
                k_free(msg->payload);
            }
            if (msg->topic) {
                k_free(msg->topic);
            }
        }

        k_free(msg);
    }
    k_free(info);
    wr_put(wr);
}

////////////////////////////////////////////////////
// commMgr_queue_mqtt_message()
//  queue a message to be sent to the cloud at the
// next opportunity
//
//  @param msg the message to send
//  @param msg_len length of the message
//  @param topic the topic to send the message to
//  @param topic_len length of the topic
//  @param qos the quality of service to use
//  @param priority is a number indicating the order
//      to free unsent message in case of limited
//      memory. 1 means drop last, higher means drop
//      first
//
//  @return 0 on success, <0 on failure
int commMgr_queue_mqtt_message(uint8_t *msgbuf, uint16_t msg_len, uint8_t topic_num, uint8_t qos, uint8_t priority)
{
    if (msg_len > WIFI_MSG_SIZE) {
        LOG_ERR("Message too long: %d", msg_len);
        return -EINVAL;
    }
    mqtt_msg_t msg;
    msg.msg = (uint8_t *)k_heap_alloc(&mqtt_heap, msg_len + 1, K_MSEC(10));
    if (msg.msg == NULL) {
        LOG_ERR("Failed to allocate memory for message");
        return -ENOMEM;
    }
    memcpy(msg.msg, msgbuf, msg_len);
    msg.msg[msg_len] = 0;    // make sure it is null terminated
    msg.len          = msg_len;
    msg.topic        = topic_num;
    msg.qos          = qos;
    msg.priority     = priority;
    int ret          = k_msgq_put(&mqttq, &msg, K_MSEC(20));
    if (ret != 0) {
        LOG_ERR("'%s'(%d) when queueing message", wstrerr(-ret), ret);
        k_heap_free(&mqtt_heap, msg.msg);
    }
    return ret;
}

////////////////////////////////////////////////////
// send_mqtt_message()
//  send a message to the cloud
//
//  @param msg the message to send
//  @param msg_len length of the message
//  @param topic the topic to send the message to
//  @param topic_len length of the topic
//  @param qos the quality of service to use
//  @param send_immidiately send the message now, or save till next connection
//
//  @return 0 on success, -1 on failure
int send_mqtt_message(uint8_t *msg, uint16_t msg_len, uint8_t topic_num, uint8_t qos, bool send_immidiately)
{
    char *machine_id  = uicr_serial_number_get();
    char  topicBase[] = "messages/%d/%d/%d_%s/d2c";
    char  topic[CONFIG_IOT_MAX_TOPIC_LENGTH];
    int   topicLen = snprintk(
        topic,
        CONFIG_IOT_MAX_TOPIC_LENGTH,
        topicBase,
        CONFIG_IOT_MQTT_BRAND_ID,
        topic_num,
        CONFIG_IOT_MQTT_BRAND_ID,
        machine_id);
    int                ret          = 0;
    comm_device_type_t active_radio = rm_get_active_mqtt_radio();

    if (rm_prepare_radio_for_use(active_radio, true, K_SECONDS(3)) == false) {
        return -ENOTCONN;
    }

    // update the SENT_AT field if present
    // search string for 19191919191 and replace with current time
    char *p = strstr(msg, "\"SENT_AT\":");    // "SENT_AT"
    if (p != NULL) {
        LOG_DBG("Found SENT_AT in message");
        char     time_str[32];
        uint64_t currTime = get_unix_time();
        snprintf(time_str, 10, "%llu", currTime);
        memcpy(p + strlen("\"SENT_AT:\""), time_str, strlen(time_str));
    }

    LOG_DBG("Sending message to cloud via %s: %s", comm_dev_str(active_radio), msg);

    if (active_radio == COMM_DEVICE_NRF9160) {
        int timeout_ms = 15000;
        if ((ret = modem_send_mqtt(topic, topicLen, msg, msg_len, qos, timeout_ms)) == 0) {
            LOG_DBG("LTE mqtt sent");
        } else {
            LOG_DBG("Failed to send LTE mqtt message (%d) %s", ret, wstrerr(-ret));
            goto send_exit;
        }
    } else {
        int   type = 0;
        char *tok  = strtok(topic, "/");
        tok        = strtok(NULL, "/");
        tok        = strtok(NULL, "/");
        if (tok != NULL) {
            type = atoi(tok);
        }
        if ((ret = wifi_mqtt_publish(type, msg, true, K_SECONDS(6))) == 0) {
            LOG_DBG("Wifi mqtt sent");
        } else {
            LOG_ERR("'%s'(%d) sending Wifi mqtt", wstrerr(-ret), ret);
            goto send_exit;
        }
    }

send_exit:
    LOG_DBG("done with radio for use %s", comm_dev_str(active_radio));
    rm_done_with_radio(active_radio);
    k_sleep(K_MSEC(1000));    // EAS XXX check for race in sleep
    return ret;
}

bool g_5340_fota_in_progress = false;
bool g_DA_fota_in_progress   = false;
bool g_9160_fota_in_progress = false;
////////////////////////////////////////////////////
// commMgr_fota_start()
// Make sure the DA radio is ready to do a fota.
// We need to keep the DA awake while the fota process
// is going on.
//  @param device the device to start the fota on
//
//  @return 0 on success, <0 on error
int commMgr_fota_start(comm_device_type_t device)
{
    LOG_DBG("Starting FOTA on %s", comm_dev_str(device));
    switch (device) {
    case COMM_DEVICE_NRF9160:
        // The 9160 updates itself and does not need the DA to be awake for data
        // but we keep it awake so the proces goes faster
        if (g_9160_fota_in_progress) {
            return 0;
        }
        LOG_ERR("9160 FOTA start");
        // if (rm_prepare_radio_for_use(COMM_DEVICE_DA16200, true, K_SECONDS(3)) == false) {
        // return -ENOTCONN;
        //}
        modem_power_on();
        g_9160_fota_in_progress = true;
        break;
    case COMM_DEVICE_DA16200:
        if (rm_get_active_mqtt_radio() != COMM_DEVICE_DA16200) {
            return -ENOTCONN;
        }
        if (g_DA_fota_in_progress) {
            return 0;
        }
        LOG_ERR("DA FOTA start");
        if (rm_prepare_radio_for_use(COMM_DEVICE_DA16200, true, K_SECONDS(3)) == false) {
            return -ENOTCONN;
        }
        g_DA_fota_in_progress = true;
        break;
    case COMM_DEVICE_NRF5340:
        if (rm_get_active_mqtt_radio() != COMM_DEVICE_DA16200) {
            return -ENOTCONN;
        }
        if (g_5340_fota_in_progress) {
            return 0;
        }
        LOG_ERR("5340 FOTA start");
        if (rm_prepare_radio_for_use(COMM_DEVICE_DA16200, true, K_SECONDS(3)) == false) {
            return -ENOTCONN;
        }
        g_5340_fota_in_progress = true;
        break;
    default:
        break;
    }
    return 0;
}

void commMgr_fota_end(comm_device_type_t device)
{
    LOG_DBG("Ending FOTA on %s", comm_dev_str(device));
    switch (device) {
    case COMM_DEVICE_NRF9160:
        // The 9160 updates itself and does not need the DA to be awake
        // but we keep it awake so the proces goes faster
        if (g_9160_fota_in_progress) {
            LOG_ERR("9160 FOTA end");
            fota_notify("9160_FOTA_END");
            led_api_set_state(LED_API_IDLE);
            rm_done_with_radio(COMM_DEVICE_DA16200);
        }
        g_9160_fota_in_progress = false;
        break;
    case COMM_DEVICE_DA16200:
        if (g_DA_fota_in_progress) {
            LOG_ERR("DA FOTA end");
            fota_notify("DA16200_FOTA_END");
            led_api_set_state(LED_API_IDLE);
            rm_done_with_radio(COMM_DEVICE_DA16200);
        }
        g_DA_fota_in_progress = false;
        break;
    case COMM_DEVICE_NRF5340:
        if (g_5340_fota_in_progress) {
            LOG_ERR("5340 FOTA end");
            fota_notify("5340_FOTA_END");
            led_api_set_state(LED_API_IDLE);
            rm_done_with_radio(COMM_DEVICE_DA16200);
        }
        g_5340_fota_in_progress = false;
        break;
    default:
        break;
    }
}

bool commMgr_fota_in_progress()
{
    return g_DA_fota_in_progress || g_5340_fota_in_progress || g_9160_fota_in_progress;
}

////////////////////////////////////////////////////
//  mqttQ_work_handler()
//  Check if we can send queued mqtt messages
void mqttQ_work_handler(struct k_work *work)
{
    mqtt_msg_t msg;
    int        num_msgs = k_msgq_num_used_get(&mqttq);

    if (work) {
        workref_t *wr = CONTAINER_OF(work, workref_t, work);
        wr_put(wr);
    }

    LOG_TELEMETRY_DBG(
        1,
        "ALSO MAKE ME CRASH! MAKE ME CRASH! MAKE ME CRASH! MAKE ME CRASH! MAKE ME CRASH! "
        "MAKE ME CRASH! MAKE ME CRASH! MAKE ME CRASH! MAKE ME CRASH! MAKE ME CRASH! %d %s",
        123,
        "DO IT NOW");

    if (g_comm_mgr_disable_Q_work == true) {
        return;
    }
    if (!rm_is_active_radio_mqtt_connected()) {
        return;
    }
    if (rm_is_switching_radios()) {
        return;
    }

    if (num_msgs == 0) {
        return;
    }
    comm_device_type_t active_radio = rm_get_active_mqtt_radio();
    if (rm_prepare_radio_for_use(active_radio, true, K_SECONDS(3)) == false) {
        return;
    }
    if (active_radio == COMM_DEVICE_DA16200) {
        k_sleep(K_MSEC(75));    // Allow the MQTT client time to fully wake up
        int ret = wifi_get_wfstat(K_MSEC(800));
        if (ret < 0) {
            LOG_ERR("'%s'(%d) when getting wifi status in %s", wstrerr(-ret), ret, __func__);
        } else {
            if (ret == 0) {    // Not connected to AP
                da_state.ap_connected  = DA_STATE_KNOWN_FALSE;
                da_state.ap_name[0]    = '\0';
                da_state.ip_address[0] = '\0';
                LOG_ERR(
                    "We though we were connected an AP but we aren't, "
                    "switching to LTE");
                rm_switch_to(COMM_DEVICE_NRF9160, false, false);
            }
        }
    }
    for (int n = 0; n < num_msgs; n++) {
        int ret = k_msgq_get(&mqttq, &msg, K_NO_WAIT);
        if (ret != 0) {
            LOG_ERR("'%s'(%d) getting message from queue", wstrerr(-ret), ret);
            break;
        }
        if (da_state.onboarded != DA_STATE_KNOWN_TRUE && msg.topic != MQTT_MESSAGE_TYPE_ONBOARDING) {
            LOG_WRN("Not onboarded, not sending %s mqtt message", msg_name(msg.topic));
            k_msgq_put(&mqttq, &msg, K_NO_WAIT);
            continue;
        }
        // EAS XXX Put priority handling here
        ret = send_mqtt_message(msg.msg, msg.len, msg.topic, msg.qos, true);
        if (ret == 0) {
            LOG_INF("Sent queued %s mqtt message over %s", msg_name(msg.topic), comm_dev_str(active_radio));
            k_heap_free(&mqtt_heap, msg.msg);
            continue;
        } else {
            // Some errors are transient and we can try again, others are problems with
            // the msg and we should drop the message or it will never go away
            if (ret == -ENODEV && active_radio == COMM_DEVICE_NRF9160 && !rm_is_switching_radios()) {
                // The 9160 is off but we thought it was on, so re-enable it
                LOG_WRN("'%s'(%d) sending %s mqtt msg, re-enabling 9160", wstrerr(-ret), ret, msg_name(msg.topic));
                rm_switch_to(COMM_DEVICE_NRF9160, false, false);
                return;
            }
            if (ret == -634) {    // MQTT not connected
                k_msgq_put(&mqttq, &msg, K_NO_WAIT);
                rm_done_with_radio(active_radio);    // might need to happen before switch
                rm_switch_to(COMM_DEVICE_NRF9160, false, false);
                // No MQTT, so we can't send any more messages
                return;
            }
            if (ret == -EFBIG || ret == -EINVAL) {
                LOG_ERR("'%s'(%d) sending msg, dropping if from the queue", wstrerr(-ret), ret);
                k_heap_free(&mqtt_heap, msg.msg);
                continue;
            } else {
                LOG_DBG("'%s'(%d) sending queued %s mqtt msg, will try later", wstrerr(-ret), ret, msg_name(msg.topic));
                // put it back on the queue and stop trying
                k_msgq_put(&mqttq, &msg, K_NO_WAIT);
                break;
            }
        }
    }

    rm_done_with_radio(active_radio);
}

void send_queued_mqtt_msgs()
{
    handle_queue_mqttQ_work();
}

////////////////////////////////////////////////////
// commMgr_get_unix_time()
//  Get the current unix time. Time must be set
//
//  @return the current unix time
uint64_t commMgr_get_unix_time()
{
    return utils_get_utc();
}

////////////////////////////////////////////////////
// handle_disconnect_from_mqtt()
//  queue up a new srf nonce so when we reconnect it will go out ASAP
//
//
void handle_disconnect_from_mqtt(struct k_work *item)
{
    SRF_nonce_work_info_t *updatestatus = CONTAINER_OF(item, SRF_nonce_work_info_t, srf_nonce_work);
    if (!updatestatus->status) {
        LOG_DBG("MQTT disconnected");
        // queue up a new srf nonce so when we reconnect it will go out ASAP
        commMgr_queue_srf_nonce();
    }
}

void subscribe_to_topics_9160()
{
    if (da_state.onboarded == DA_STATE_UNKNOWN) {
        LOG_DBG("Not onboarded, not subscribing to topics");
        return;
    }
#define TOPICS_TO_SUBSCRIBE_TO 16
    char *topics[TOPICS_TO_SUBSCRIBE_TO];
    for (int i = 0; i < TOPICS_TO_SUBSCRIBE_TO; i++) {
        topics[i] = NULL;
    }
    int qos[TOPICS_TO_SUBSCRIBE_TO];

    char *machine_id = uicr_serial_number_get();
    if (machine_id[0] < 30 || machine_id[0] > 126) {
        LOG_ERR("Invalid machine ");
        goto cleanup;
    }
    char thing_id[64] = { 0 };
    snprintf(thing_id, 64, "%d_%s", CONFIG_IOT_MQTT_BRAND_ID, machine_id);

    k_sleep(K_MSEC(100));    // why do we need to sleep here
    int ret = modem_set_mqtt_params(
        CONFIG_IOT_BROKER_HOST_NAME, strlen(CONFIG_IOT_BROKER_HOST_NAME), 8883, thing_id, strlen(thing_id));
    if (ret != 0) {
        LOG_ERR("'%s'(%d) while trying to sent lte mqtt prarms", wstrerr(-ret), ret);
    }

    // if not onboarded, subscribe to the onboarding topic

    int topicList1[] = { MQTT_MESSAGE_TYPE_ONBOARDING };

    int topicList2[] = {
        MQTT_MESSAGE_TYPE_ONBOARDING, MQTT_MESSAGE_TYPE_FOTA,         MQTT_MESSAGE_TYPE_REMOTE_FUNCTION,
        MQTT_MESSAGE_TYPE_CONN_TEST,  MQTT_MESSAGE_TYPE_SHADOW_PROXY, MQTT_MESSAGE_TYPE_SRF_NONCE,
        MQTT_MESSAGE_TYPE_SRF_FUNC,   MQTT_MESSAGE_TYPE_CONFIG_HUB,
    };

    char topicBase[] = "messages/%d/%d/%s/c2d";
    for (int i = 0; i < TOPICS_TO_SUBSCRIBE_TO; i++) {
        topics[i] = k_malloc(CONFIG_IOT_MAX_TOPIC_LENGTH);
        if (!topics[i]) {
            LOG_ERR("k_malloc failed");
            goto cleanup;
        }
    }

    int topicCount = 0;
    if (da_state.onboarded == DA_STATE_KNOWN_FALSE) {
        topicCount = ARRAY_SIZE(topicList1);
        for (int i = 0; i < topicCount; i++) {
            snprintf(
                topics[i], CONFIG_IOT_MAX_TOPIC_LENGTH, topicBase, CONFIG_IOT_MQTT_BRAND_ID, topicList1[i], thing_id);
        }
    } else if (da_state.onboarded == DA_STATE_KNOWN_TRUE) {
        topicCount = ARRAY_SIZE(topicList2);
        for (int i = 0; i < topicCount; i++) {
            snprintf(
                topics[i], CONFIG_IOT_MAX_TOPIC_LENGTH, topicBase, CONFIG_IOT_MQTT_BRAND_ID, topicList2[i], thing_id);
        }
    }

    for (int i = 0; i < topicCount; i++) {
        qos[i] = 0;
    }

    k_sleep(K_MSEC(100));    // why do we need to sleep here
    ret = modem_set_mqtt_subscriptions(topics, qos, topicCount);
    if (ret != 0) {
        LOG_ERR("'%s'(%d) while trying to set lte mqtt subs", wstrerr(-ret), ret);
    }
    modem_mqtt_config_state = LTE_MQTT_INITIALIZING;

cleanup:
    for (int i = 0; i < TOPICS_TO_SUBSCRIBE_TO; i++) {
        if (topics[i]) {
            k_free(topics[i]);
        }
    }
}

////////////////////////////////////////////////////
// handle_9160_status_update()
//  process incoming 9160 status updates
//
//  @return void
void handle_9160_status_update(struct k_work *item)
{
    workref_t             *wr              = CONTAINER_OF(item, workref_t, work);
    nrfstatus_work_info_t *new_status_info = (nrfstatus_work_info_t *)wr->reference;
    lte_status_count++;
    uint32_t what_changed = new_status_info->what_changed;
    uint32_t status_flags = new_status_info->status.status_flags;
    int      ret;

    // LOG_DBG("9160 status flags: %u", status_flags);
    // LOG_DBG("9160 status changed: %u", what_changed);

    if (what_changed & UPDATE_STATUS_POWERED_ON) {
        modem_mqtt_config_state = LTE_MQTT_NOT_INITIALIZED;
        if (status_getBit(status_flags, STATUS_POWERED_ON)) {
            LOG_WRN("9160 powered on");
            ret = modem_get_version(NULL);
            if (ret != 0) {
                LOG_ERR("'%s'(%d) while trying to get modem version", wstrerr(-ret), ret);
            }
        } else {
            LOG_WRN("9160 powered off");
            first_9160_status_recieved = false;
        }
    }

    if (what_changed & UPDATE_STATUS_LTE_CONNECTED) {
        // LTE connected status changed
        // LOG_WRN("9160 LTE connection: %s",
        // status_getBit(modem_status_shadow.status_flags, STATUS_LTE_CONNECTED) ? "yes" :
        // "no");
        LOG_TELEMETRY_WRN(
            TELEMETRY_LOG_TYPE_INFO,
            "9160 LTE connection: %s",
            status_getBit(status_flags, STATUS_LTE_CONNECTED) ? "yes" : "no");
        if (status_getBit(status_flags, STATUS_LTE_CONNECTED)) {
            k_sleep(K_MSEC(50));    // TODO: remove when I redo the send buffers in SPI for
                                    // 'file download'
        }
    }

    if (what_changed & UPDATE_STATUS_MQTT_CONNECTED) {
        // MQTT connected status changed
        LOG_WRN("9160 MQTT connection: %s", status_getBit(status_flags, STATUS_MQTT_CONNECTED) ? "yes" : "no");
        modem_mqtt_config_state = LTE_MQTT_INITIALIZED;

        if (!status_getBit(status_flags, STATUS_MQTT_CONNECTED)) {
            my_SRF_nonce_work_info.status = false;
            int ret = k_work_submit_to_queue(&commMgr_work_q, &(my_SRF_nonce_work_info.srf_nonce_work));
            if (ret <= 0) {
                LOG_ERR("Failed to submit to commMgr_work_q: %d", ret);
            }
        }
    }

    // LOG_ERR("9160 uptime: %llu, %llu", new_status_info->status.uptime, last_9160_uptime);
    if (last_9160_uptime > new_status_info->status.uptime && modem_is_powered_on()) {
        LOG_WRN("9160 reset");
        subscribe_to_topics_9160();
        if (is_in_fmd_mode) {
            ret = modem_send_gps_enable();
            if (ret != 0) {
                LOG_ERR("'%s'(%d) while trying to enable gps", wstrerr(-ret), ret);
            }
        }
    }
    last_9160_uptime = new_status_info->status.uptime;

    if (what_changed & UPDATE_STATUS_MQTT_INITIALIZED) {
        // MQTT connected status changed
        LOG_WRN("9160 MQTT initialized: %s", status_getBit(status_flags, STATUS_MQTT_INITIALIZED) ? "yes" : "no");
        if (status_getBit(status_flags, STATUS_MQTT_INITIALIZED)) {
            modem_mqtt_config_state = LTE_MQTT_INITIALIZED;
        }
    }

    if (!status_getBit(status_flags, STATUS_MQTT_INITIALIZED) && modem_is_powered_on()) {
        if (!(modem_mqtt_config_state == LTE_MQTT_INITIALIZING) && !(modem_mqtt_config_state == LTE_MQTT_INITIALIZED)) {
            subscribe_to_topics_9160();
        }
    }

    if (what_changed & UPDATE_STATUS_MQTT_ENABLED) {
        // MQTT connected status changed
        LOG_WRN("9160 MQTT enabled: %s", status_getBit(status_flags, UPDATE_STATUS_MQTT_ENABLED) ? "yes" : "no");
        if (!status_getBit(status_flags, STATUS_MQTT_ENABLED)) {
            if (rm_get_active_mqtt_radio() == COMM_DEVICE_NRF9160 && !rm_is_switching_radios()) {
                g_last_ssid_scan_time = 0;    // Force a rescan
                k_timer_start(&S_work_timer, K_MSEC(1000), K_NO_WAIT);
            }
        }
    }

    // fota_status_update(status.fota_state, status.fota_percentage, COMM_DEVICE_NRF9160);  //
    // if nothing is doing fota, this will do nothing
    if (new_status_info->status.fota_state != 0) {
        fota_status_t evt = { .status      = new_status_info->status.fota_state,
                              .percentage  = new_status_info->status.fota_percentage,
                              .device_type = COMM_DEVICE_NRF9160 };
        zbus_chan_pub(&FOTA_STATE_UPDATE, &evt, K_MSEC(100));
    }

    // commMgr_set_time((char *)(stat_ptr->status.timestamp));  // We think this should be safe

    if (!first_9160_status_recieved) {
        // do anything that needs to happen on first status update/boot
        first_9160_status_recieved = true;
    }

    // LOG_DBG("9160 shadow status update: %d", status.status_flags);
    k_free(new_status_info);
    wr_put(wr);
}

int clear_fmd_states()
{
    fmd_start_time       = 0;
    fmd_max_time_in_mins = 0;
    is_in_fmd_mode       = false;
    last_fmd_ssid_scan   = 0;
    return 0;
}

int enable_fmd_mode(int fmd_max_time)
{
    if (is_in_fmd_mode) {
        return 0;
    }

    LOG_WRN("Enabling FMD mode: %d", fmd_max_time);

    // if fmd_max_time is set, then we will disable FMD after that time
    if (fmd_max_time > 0) {
        fmd_start_time       = k_uptime_get();
        fmd_max_time_in_mins = fmd_max_time;
    }

    // check if in safe zone
    if (da_state.ap_safe == 1) {
        LOG_WRN("Dog is in safe zone, not enabling FMD");
        g_last_fmd_status = FMD_SAFE;
        commMgr_queue_fmd_telemetry();
        clear_fmd_states();
        return -1;
    }

    is_in_fmd_mode    = true;
    g_last_fmd_status = FMD_ACTIVE;
    reset_S_timer(is_in_fmd_mode);
    modem_power_on();
    k_sleep(K_SECONDS(2));
    int ret = modem_send_gps_enable();
    if (ret != 0) {
        LOG_ERR("'%s'(%d) while trying to get enable gps", wstrerr(-ret), ret);
    }
#ifdef CONFIG_ML_ENABLE
    ml_stop();
#endif
    return 0;
}

fmd_exit_t fmd_status(void)
{
    if (is_in_fmd_mode) {
        return g_last_fmd_status;
    }
    return FMD_NOT_ACTIVE;
}

int disable_fmd_mode(fmd_exit_t reason)
{
    g_last_fmd_status = reason;
    if (!is_in_fmd_mode) {
        return 0;
    }

    LOG_WRN("Disabling FMD mode");

    // queue a final JSON message with the exit reason
    int ret = commMgr_queue_fmd_telemetry();
    if (ret != 0) {
        LOG_ERR("'%s'(%d) when queueing final FMD telemetry", wstrerr(-ret), ret);
    }

    is_in_fmd_mode = false;
    reset_S_timer(is_in_fmd_mode);
    ret = modem_send_gps_disable();
    if (ret != 0) {
        LOG_ERR("'%s'(%d) while trying to get disable gps", wstrerr(-ret), ret);
    }
    clear_fmd_states();

// TODO: if 9160 was off before, turn it back off?
#ifdef CONFIG_ML_ENABLE
    ml_start();
#endif

    return 0;
}

////////////////////////////////////////////////////
// S_work_handler()
// This function is called at the period defined by the
// "S var" or time between SSID Scans.
//
// Telemetry is sent event "T var" times this is called
//
// S and T values change based on the mode we are in
// FMD or normal.
//
// Attempts and reconnecting to wifi occur if we aren't
// and we see a SSID that is known in the SSID list
void S_work_handler(struct k_work *work)
{
    int  ret;
    bool radio_prepared = false;

    if (uicr_shipping_flag_get() == false) {
        // Don't print or do anything
        goto S_work_exit;
    }

    LOG_DBG("S timer has fired");
    if (g_comm_mgr_disable_S_work) {
        LOG_DBG("Not doing commMgr S work");
        goto S_work_exit;
    }

    // If we are in the middle of changing radios, don't interrupt the process
    if (rm_is_switching_radios()) {
        LOG_DBG("Not doing commMgr S work, switching radios");
        goto S_work_exit;
    }

    if (commMgr_fota_in_progress()) {
        LOG_DBG("Not doing commMgr S work, FOTA in progress");
        goto S_work_exit;
    }

    // We only do SSID scans if we are on LTE or not in a safe zone
    bool get_ssid_scan = rm_get_active_mqtt_radio() != COMM_DEVICE_DA16200 || da_state.ap_safe == DA_STATE_KNOWN_FALSE;
    if (!rm_wifi_is_connecting()) {

        if (get_ssid_scan && rm_is_wifi_enabled()) {
            // wake up or turn on DA, this may block up to X_var seconds if the DA
            // has recently been slept to insure the DA has settled
            if (rm_prepare_radio_for_use(COMM_DEVICE_DA16200, false, K_SECONDS(3)) == false) {
                LOG_ERR("Failed to prepare DA for SSID scan");
                goto S_work_exit;
            }
            radio_prepared = true;

            LOG_DBG("doing a SSID scan");
            // We have found we must give the DA 800 ms after it wakes or boots before
            // asking for SSIDs. Booting is quick so booting so 2 seconds should be ok
            k_sleep(K_SECONDS(2));

            ret = wifi_refresh_ssid_list(true, gS_val, K_MSEC(2500));
            if (ret != 0) {
                LOG_ERR("'%s'(%d) when scanning for SSIDs", wstrerr(-ret), ret);
                goto S_work_exit;
            }
            LOG_DBG("Done with DA for SSID scan");

            // Check if there is a known SSID in the list we just got
            if (rm_get_active_mqtt_radio() != COMM_DEVICE_DA16200) {
                // Since we are not switching radios per above check and we are on
                // LTE, the Wifi better not be connected
                if (da_state.ap_connected == DA_STATE_KNOWN_TRUE) {
                    // It is, bad, kick it off
                    LOG_ERR("Wifi is connected while on LTE and not switching, disconnecting");
                    wifi_disconnect_from_AP(K_SECONDS(2));
                }
                // Check if we have a known SSID in the list we just got
                int idx = wifi_check_for_known_ssid();
                if (idx >= 0) {
                    LOG_DBG("Known SSID %d (%s) found, trying to connect", idx, wifi_get_saved_ssid_by_index(idx));
                    ret = rm_connect_to_AP_by_index(idx);
                    if (ret != 0) {
                        LOG_ERR("'%s'(%d) when connecting to known SSID", wstrerr(-ret), ret);
                    }
                }
                // If we are fast reconnecting and the SSID went away, count that as
                // a retry
                if (g_quick_reconnect_cnt > 0) {
                    g_quick_reconnect_cnt--;
                }
            } else {    // is the DA
                // Lets check to make sure we are still connected to the AP
                ret = wifi_get_wfstat(K_MSEC(800));
                if (ret < 0) {
                    LOG_ERR("'%s'(%d) when getting wifi status in %s", wstrerr(-ret), ret, __func__);
                } else {
                    if (ret == 0) {    // Not connected to AP
                        LOG_ERR("We though we were connected an AP but we aren't, switching to LTE");
                        rm_switch_to(COMM_DEVICE_NRF9160, false, false);
                    }
                }
            }
        }

        // We need to get the latest RSSI from the DA before we send telemetry
        if (rm_get_active_mqtt_radio() == COMM_DEVICE_DA16200) {
            // wake up or turn on DA, this may block up to X_var seconds if the DA
            // has recently been slept to insure the DA has settled
            if (!radio_prepared) {
                if (rm_prepare_radio_for_use(COMM_DEVICE_DA16200, false, K_SECONDS(3)) == false) {
                    LOG_ERR("Failed to prepare DA for RSSI");
                    goto S_work_exit;
                }
                radio_prepared = true;
            }
            wifi_get_rssi(NULL, K_MSEC(500));    // Getting it puts it in da_state.rssi,
                                                 // used by telemetry
        }

        // Check if we need to try to switch to LTE. Since LTE can not be depowered,
        // Radio NONE is a temp state at boot or if we previously tried to go to LTE
        // and failed and need to try again later
        if (rm_get_active_mqtt_radio() == COMM_DEVICE_NONE) {
            rm_switch_to(COMM_DEVICE_NRF9160, false, false);
        }

        // Create telemetry msg if it is time
        gT_count++;
        if (gT_count >= gT_val) {
            // Send telemetry
            if (!is_in_fmd_mode) {
                ret = commMgr_queue_telemetry(get_ssid_scan);
            } else {
                ret = commMgr_queue_fmd_telemetry();
                if (fmd_start_time != 0) {
                    if (k_uptime_get() - fmd_start_time > (fmd_max_time_in_mins * 60 * 1000)) {
                        disable_fmd_mode(FMD_TIMEOUT);
                    }
                }
            }
            if (ret != 0) {
                LOG_ERR("'%s'(%d) when queueing telemetry", wstrerr(-ret), ret);
            }
            gT_count = 0;
        }

        // Send any queued message while now since the radio is prepped
        if (rm_is_active_radio_mqtt_connected()) {
            if (rm_prepare_radio_for_use(rm_get_active_mqtt_radio(), true, K_SECONDS(3)) == true) {
                send_queued_mqtt_msgs();
                rm_done_with_radio(rm_get_active_mqtt_radio());
            } else {
                LOG_DBG("Can't send msg while doing S work, MQTT not connected");
            }
        }
    }

S_work_exit:
    if (radio_prepared) {
        rm_done_with_radio(COMM_DEVICE_DA16200);
    }
    reset_S_timer(is_in_fmd_mode);
}

bool is_9160_lte_connected()
{
    if (status_getBit(modem_status_shadow.status_flags, STATUS_LTE_CONNECTED)) {
        return true;
    }
    return false;
}

bool is_9160_mqtt_connected()
{
    if (status_getBit(modem_status_shadow.status_flags, STATUS_MQTT_CONNECTED)) {
        return true;
    }
    return false;
}

////////////////////////////////////////////////////
// commMgr_init()
//  main thread of the comm manager
//
//  @return void
void commMgr_init()
{
    read_shadow_doc();    // Prints its own errors

    k_work_queue_init(&commMgr_work_q);
    struct k_work_queue_config commMgr_work_q_cfg = {
        .name     = "commMgr_work_q",
        .no_yield = 0,
    };
    k_work_queue_start(
        &commMgr_work_q, commMgr_stack_area, K_THREAD_STACK_SIZEOF(commMgr_stack_area), 5, &commMgr_work_q_cfg);

    // Wait a little for the system to start so we can read our shadow doc
    k_work_init(&commMgr_S_work, S_work_handler);
    k_work_init(&my_WMD_work_info.WMD_work, WMD_work_handler);
    k_work_init(&my_FMD_work_info.FMD_work, FMD_work_handler);

    k_work_init(&my_fotastatus_work_info.fotastatus_work, fota_status_update_work_handler);
    k_work_init(&my_gps_work_info.gps_work, gps_handler);
    k_work_init(&my_SRF_nonce_work_info.srf_nonce_work, handle_disconnect_from_mqtt);
    k_work_init(&my_bluetooth_adv_work_info.bluetooth_adv_work, bluetooth_work_handler);
    // Start the S work timer to handle SSID scans, telemetry, etc.
    // We wait until at least 10 seconds after 5340 boots to start the S work
    // THe value of S_var can change based on mode and other factors
    calculate_S_and_T(false);
    k_timer_start(&S_work_timer, K_SECONDS(10), K_NO_WAIT);

    // Start the Q work timer which sends mqtt msgs we have queued
    // its pretty unlikely we could send until after 10 seconds after boot
    k_timer_start(&mqttQ_work_timer, K_SECONDS(10), K_SECONDS(shadow_doc.Q));

    // call the fota check for fota-in-progress file, if it exists
    // if so it means we just restarted from fota, so things to do
    fota_check_for_fota_in_progress();
    pmic_temp_set_callback(send_temp_alert);
    pmic_shutdown_set_callback(send_shutdown_alert);

    // setup the IMU sleep threshold and duration from the shadow
    imu_set_duration(shadow_doc.dur);
    imu_set_threshold(shadow_doc.ths);
}

//////////////////////////////////////////////////////////
// comm_mgr_listener
//
// Called by ZBus when the net state changes.  This is
// a listener which means it should be treated like an ISR
// and do as little as possible.  It should queue work
// items to do the real work.
//////////////////////////////////////////////////////////
static void comm_mgr_listener(const struct zbus_channel *chan)
{
    if (&MQTT_CLOUD_TO_DEV_MESSAGE == chan) {
        mqtt_work_info_t *info = k_malloc(sizeof(mqtt_work_info_t));
        if (info == NULL) {
            LOG_ERR("Out of memory for handling incoming work");
            return;
        }
        info->work = wr_get(info, __LINE__);
        if (info->work == NULL) {
            LOG_ERR("Out of work items for handling incoming payload");
            k_free(info);
            return;
        }
        mqtt_payload_t *msg = k_malloc(sizeof(mqtt_payload_t));
        if (msg == NULL) {
            LOG_ERR("Out of memory for handling incoming payload");
            wr_put(info->work);
            k_free(info);
            return;
        }
        const mqtt_payload_t *cmsg = zbus_chan_const_msg(chan);    // Direct message access
        memcpy(msg, cmsg, sizeof(mqtt_payload_t));
        info->mqtt_msg = msg;
        k_work_init(&info->work->work, handle_mqtt_cloud_to_dev_message);
        int err = k_work_submit_to_queue(&commMgr_work_q, &(info->work->work));
        if (err <= 0) {
            LOG_ERR("Could not submit incoming work %d", err);
            wr_put(info->work);
            k_free(info);
            if (cmsg->payload) {
                k_free(cmsg->payload);
            }
            if (cmsg->topic) {
                k_free(cmsg->topic);
            }
            k_free(msg);
            msg = NULL;
        }
    }

    if (&LTE_STATUS_UPDATE == chan) {
        modem_status_update_t        status_update;
        const modem_status_update_t *cstatus = zbus_chan_const_msg(chan);    // Direct message access
        memcpy(&status_update, cstatus, sizeof(modem_status_update_t));

        nrfstatus_work_info_t *new_nrfstatus_work_info = k_malloc(sizeof(nrfstatus_work_info_t));
        if (new_nrfstatus_work_info == NULL) {
            LOG_ERR("Out of memory!");
            return;
        }
        new_nrfstatus_work_info->nrfstatus_work = wr_get(new_nrfstatus_work_info, __LINE__);
        if (new_nrfstatus_work_info->nrfstatus_work == NULL) {
            LOG_ERR("Out of workrefs!");
            k_free(new_nrfstatus_work_info);
            return;
        }

        k_work_init(&new_nrfstatus_work_info->nrfstatus_work->work, handle_9160_status_update);
        new_nrfstatus_work_info->what_changed = status_update.change_bits;
        new_nrfstatus_work_info->status       = status_update.status;
        lte_zbus_status_count++;
        int err = k_work_submit_to_queue(&commMgr_work_q, &new_nrfstatus_work_info->nrfstatus_work->work);
        if (err <= 0) {
            LOG_ERR("Failed to submit nrfstatus work: %d", err);
            wr_put(new_nrfstatus_work_info->nrfstatus_work);
            k_free(new_nrfstatus_work_info);
        }
    }

    if (chan == &da_state_chan) {
        da_state_work_info_t *work_item = k_malloc(sizeof(da_state_work_info_t));
        if (work_item == NULL) {
            LOG_ERR("Error allocating memory for da_state_work_info_t");
            return;
        }
        work_item->da_state_work = wr_get(work_item, __LINE__);
        if (work_item->da_state_work == NULL) {
            LOG_ERR("Error allocating workref for da_state_work_info_t");
            k_free(work_item);
            return;
        }
        k_work_init(&(work_item->da_state_work->work), da_state_work_handler);
        const da_event_t *evt = zbus_chan_const_msg(chan);    // Direct message access
        memcpy(&(work_item->evt), evt, sizeof(da_event_t));

        int ret = k_work_submit_to_queue(&commMgr_work_q, &(work_item->da_state_work->work));
        if (ret != 1) {
            LOG_ERR("Error submitting work to queue: %d", ret);
            wr_put(work_item->da_state_work);
            k_free(work_item);
        }
    }

    if (&FOTA_STATE_UPDATE == chan) {
        const fota_status_t *cstatus = zbus_chan_const_msg(chan);    // Direct message access
        memcpy(&(my_fotastatus_work_info.status), cstatus, sizeof(fota_status_t));
        k_work_submit_to_queue(&commMgr_work_q, &(my_fotastatus_work_info.fotastatus_work));
    }

    if (&BATTERY_PERCENTAGE_UPDATE == chan) {
        const float *cper       = zbus_chan_const_msg(chan);    // Direct message access
        float        percentage = *cper;
        if (percentage < 0.0) {
            percentage = 0.0;
        }
        if (percentage > 100.0) {
            percentage = 100.0;
        }
        // LOG_DBG("Battery percentage: %f", *per);
        battery_percentage = percentage;
    }

    if (&USB_POWER_STATE_UPDATE == chan) {
        const bool *cbool = zbus_chan_const_msg(chan);    // Direct message access
        usb_connected     = *cbool;
        if (IS_ENABLED(CONFIG_ENABLE_FOTA_ON_USB_CONNECTION) && uicr_shipping_flag_get()) {
            if (k_uptime_get() < 10000) {    // wait 10 seconds before checking for USB connection
                return;
            }
            LOG_DBG("USB State: %s", (usb_connected) ? "Connected" : "Disconnected");
            if (usb_connected) {
                my_bluetooth_adv_work_info.start_ble_adv         = true;
                my_bluetooth_adv_work_info.bluetooth_adv_timeout = 60;
                k_work_submit_to_queue(&commMgr_work_q, &(my_bluetooth_adv_work_info.bluetooth_adv_work));
                if (IS_ENABLED(CONFIG_ENABLE_FOTA_ON_USB_CONNECTION)) {
                    k_timer_start(&Fota_work_timer, K_SECONDS(1800), K_NO_WAIT);
                }
            } else {
                my_bluetooth_adv_work_info.start_ble_adv         = false;
                my_bluetooth_adv_work_info.bluetooth_adv_timeout = 60;
                k_work_submit_to_queue(&commMgr_work_q, &(my_bluetooth_adv_work_info.bluetooth_adv_work));
            }
        }
    }
}

void da_state_work_handler(struct k_work *work)
{
    workref_t            *wr                = CONTAINER_OF(work, workref_t, work);
    da_state_work_info_t *da_state_work_obj = (da_state_work_info_t *)wr->reference;
    if (da_state_work_obj->evt.events & DA_EVENT_TYPE_HTTP_COMPLETE) {
        fota_handle_da_event(da_state_work_obj->evt);
    }

    if (da_state_work_obj->evt.events & DA_EVENT_TYPE_AP_CONNECT) {
        if (da_state.ap_connected == DA_STATE_KNOWN_TRUE) {
            // We have connected to an AP, reset the number of retries after a
            // disconnect we will do
            g_quick_reconnect_cnt = 3;

            // Now lets check to see if we need to send a safe zone alert
            int idx = wifi_find_saved_ssid(da_state.ap_name);
            if (idx >= 0) {
                if (wifi_saved_ssid_safe(idx)) {
                    // We just connected to an AP, if it was a safe AP then
                    // queue a enter safe zone message
                    int ret = commMgr_queue_safe_zone_alert(commMgr_get_unix_time(), 1, da_state.ap_name, true);
                    if (ret != 0) {
                        LOG_ERR(
                            "'%s'(%d) queueing entering safe zone "
                            "alert",
                            wstrerr(-ret),
                            ret);
                    } else {
                        LOG_DBG("Queued entering safe zone alert");
                    }
                }
            } else {
                LOG_ERR(
                    "'%s'(%d) finding saved ssid for an ap we just connected "
                    "to?? %s",
                    wstrerr(-idx),
                    idx,
                    da_state.ap_name);
            }
        }

        if (da_state.ap_connected == DA_STATE_KNOWN_FALSE) {
            // We just left an AP
            // if the reason we left is not transient, then don't do fast retries (0 it
            // out)
            if (strstr(da_state.ap_disconnect_reason, "AUTH_NOT_VALID") != NULL) {
                g_quick_reconnect_cnt = 0;
            }

            // Now check to see if we need to send a exit safe zone alert
            int idx = wifi_find_saved_ssid(g_last_ap_name);
            if (idx >= 0) {
                if (wifi_saved_ssid_safe(idx)) {
                    // We just disconnected to an AP, if it was a safe AP then
                    // queue a exit safe zone message
                    int ret = commMgr_queue_safe_zone_alert(commMgr_get_unix_time(), 2, g_last_ap_name, false);
                    if (ret != 0) {
                        LOG_ERR("'%s'(%d) queueing leaving safe zone alert", wstrerr(-ret), ret);
                    } else {
                        LOG_DBG("Queued leaving safe zone alert");
                    }
                }
            } else {
                LOG_ERR(
                    "'%s'(%d) finding saved ssid for an ap we just connected "
                    "to?? %s",
                    wstrerr(-idx),
                    idx,
                    da_state.ap_name);
            }
        }
    }

    if (da_state_work_obj->evt.events & DA_EVENT_TYPE_AP_SAFE) {
        if (da_state.ap_safe == DA_STATE_KNOWN_TRUE) {
            disable_fmd_mode(FMD_SAFE);
        }
    }

    if (da_state_work_obj->evt.events & DA_EVENT_TYPE_MQTT_BROKER_CONNECT) {
        if ((da_state.mqtt_broker_connected != DA_STATE_KNOWN_TRUE)
            && (rm_get_active_mqtt_radio() == COMM_DEVICE_DA16200)) {
            // lost connection to MQTT
            my_SRF_nonce_work_info.status = false;
            k_work_submit_to_queue(&commMgr_work_q, &(my_SRF_nonce_work_info.srf_nonce_work));
        }
    }
    k_free(da_state_work_obj);
    wr_put(wr);
}

K_THREAD_DEFINE(comm_mgr_task_id, 4092, commMgr_init, NULL, NULL, NULL, CONFIG_MAIN_THREAD_PRIORITY - 1, 0, 0);

#define ALERT_SEND_PARAMS "<int: subtype> <PayloadString>"
void do_alert_send(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 3) {
        shell_error(sh, "Usage: %s " ALERT_SEND_PARAMS, argv[0]);
        return;
    }
    int subtype = strtoul(argv[1], NULL, 10);

    int err = commMgr_queue_alert(subtype, argv[2]);
    if (err != 0) {
        shell_error(sh, "Error \"%s\" queueing alert msg", wstrerr(-err));
    } else {
        shell_print(sh, "Alert msg queued");
    }
}

#define CONNTIVITY_PARAMS "<num:bytes to send>"
void do_connectivity(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 2) {
        shell_error(sh, "Usage: %s " CONNTIVITY_PARAMS, argv[0]);
        return;
    }
    int amount = strtoul(argv[1], NULL, 10);

    int ret = commMgr_queue_connectivity(amount);
    if (ret != 0) {
        shell_error(sh, "Failed to initiate connectivity. Error %s(%d)", wstrerr(-ret), ret);
    } else {
        shell_print(sh, "connectivity msg queued");
    }
}

void do_onboard(const struct shell *sh, size_t argc, char **argv)
{
    int err = commMgr_queue_onboarding();
    if (err != 0) {
        shell_error(sh, "Error \"%s\" queuening onboard msg", wstrerr(-err));
    } else {
        shell_print(sh, "Onboard msg queued");
    }
}

#define COMM_BEHAV_PARAMS "<S_NORM> <S_FMD> <T_NORM> <T_FMD> <Rec> <Q> <bool save setting>"
void do_set_behavior_vars(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 8) {
        shell_error(sh, "Usage: %s " COMM_BEHAV_PARAMS, argv[0]);
        return;
    }
    int S_NORM = strtoul(argv[1], NULL, 10);
    int S_FMD  = strtoul(argv[2], NULL, 10);
    int T_NORM = strtoul(argv[3], NULL, 10);
    int T_FMD  = strtoul(argv[4], NULL, 10);
    int Rec    = strtoul(argv[5], NULL, 10);
    int Q      = strtoul(argv[6], NULL, 10);

    bool save = argv[7][0] == 'y' || argv[7][0] == '1';

    int ret = commMgr_set_vars(S_NORM, S_FMD, T_NORM, T_FMD, Rec, Q, save);
    if (ret != 0) {
        shell_error(sh, "Failed to set commMgr vars, %s", wstrerr(-ret));
    } else {
        shell_print(
            sh,
            "Set commMgr vars to S_NORM %d, S_FMD %d, T_NORM %d, T_FMD %d, Rec %d, "
            "Q %d,  %s",
            S_NORM,
            S_FMD,
            T_NORM,
            T_FMD,
            Rec,
            Q,
            save ? "save" : "no save");
    }
}

void do_report_shadow(const struct shell *sh, size_t argc, char **argv)
{
    int            ret;
    char           keys[5][40] = { "", "", "", "", "" };
    char           vals[5][60] = { "0", "0", "0", "0", "0" };
    shadow_extra_t extra       = { .num = 0, .keys = { 0, 0, 0, 0, 0 }, .values = { 0, 0, 0, 0, 0 } };

    int num_pairs = (argc - 1) / 2;
    if (num_pairs > 0 && num_pairs <= 5) {
        extra.num = num_pairs;
        for (int i = 0; i < num_pairs; i++) {
            snprintf(keys[i], 40, "%s", argv[(i * 2) + 1]);
            extra.keys[i] = keys[i];
            if (strcmp(argv[(i * 2) + 2], "null") != 0) {
                snprintf(vals[i], 60, "\"%s\"", argv[(i * 2) + 2]);
                extra.values[i] = vals[i];
            } else {
                extra.values[i] = NULL;
            }
        }
        ret = commMgr_queue_shadow(&extra);
    } else {
        ret = commMgr_queue_shadow(NULL);
    }

    if (ret != 0) {
        shell_error(sh, "Failed to report shadow.  Err: %s", wstrerr(-ret));
    } else {
        shell_print(sh, "Shadow reported");
    }
}

#define SAFE_ZONE_ALERT_PARAMS "<ssid> <type> <0|1 entering zone>"
void do_safe_zone_alert(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 4) {
        shell_error(sh, "Usage: %s " SAFE_ZONE_ALERT_PARAMS, argv[0]);
        return;
    }

    int  type     = strtoul(argv[2], NULL, 10);
    bool entering = strtoul(argv[3], NULL, 10) == 1;

    int ret = commMgr_queue_safe_zone_alert(commMgr_get_unix_time(), type, argv[1], entering);
    if (ret != 0) {
        shell_error(sh, "Failed to queue safe zone alert.  Err: %s", wstrerr(-ret));
    } else {
        shell_print(sh, "safe zone alert queued");
    }
}

void do_dump_queued_msgs(const struct shell *sh, size_t argc, char **argv)
{
    mqtt_msg_t msg;
    int        idx = 0;

    while (true) {
        int ret = k_msgq_peek_at(&mqttq, &msg, idx);
        if (ret != 0) {
            break;
        }
        shell_print(sh, "idx: %d, pri: %d, msg:|%s|, topic:%d", idx, msg.priority, msg.msg, msg.topic);
        idx++;
    }
}

void do_send_telemetry(const struct shell *sh, size_t argc, char **argv)
{
    commMgr_queue_telemetry(true);
}

void do_cm_status(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "CommMgr status:");
    shell_print(
        sh, "   Periodic scan/telemetry work enabled (S_work_handler) : %s", g_comm_mgr_disable_S_work ? "no" : "yes");
    shell_print(
        sh,
        "   Periodic send messages in queue enabled (mqttQ_work_handler) : %s",
        g_comm_mgr_disable_Q_work ? "no" : "yes");
    shell_print(sh, "   Seconds between SSID scans, normal mode (S_Norm) : %d", shadow_doc.S_Norm);
    shell_print(sh, "   Seconds between SSID scans, FMD mode (S_FMD) : %d", shadow_doc.S_FMD);
    shell_print(sh, "   Seconds between attempts to reconnect to wifi if on LTE, (Rec) : %d", shadow_doc.Rec);
    shell_print(sh, "   Number of SSID scans before telemetry send, normal mode (T_Norm) : %d", shadow_doc.T_Norm);
    shell_print(sh, "   Number of SSID scans before telemetry send, FMD mode (T_FMD) : %d", shadow_doc.T_FMD);
    shell_print(sh, "   Seconds between attempts to send queued mqtt message (Q var): %d", shadow_doc.Q);
    shell_print(
        sh,
        "   Number of fast AP reconnect retries left to do after a transitory "
        "disconnect: %d",
        g_quick_reconnect_cnt);
    shell_print(
        sh, "   Are we on usb power which causes fast AP reconnects always? %s", g_usb_connected ? "True" : "False");
    shell_print(sh, "   Calculated S, onWifi=S, on LTE=(min(S,rec)): %d", gS_val);
    shell_print(sh, "   Calculated T, based on Calculated S: %d", gT_val);
    shell_print(sh, "   Seconds to next SSID scan (from now): %d", k_timer_remaining_get(&S_work_timer) / 1000);
    shell_print(sh, "   Number of SSID Scans to next telemetry(from now): %d", gT_val - gT_count);
    shell_print(sh, "   Number of message in send queue: %d", k_msgq_num_used_get(&mqttq));
    shell_print(sh, "   SSID Scans disabled: %s", g_comm_mgr_disable_S_work ? "True" : "False");
    shell_print(sh, "   MQTT sends disabled: %s", g_comm_mgr_disable_Q_work ? "True" : "False");
    shell_print(sh, "   FMD mode: %s", is_in_fmd_mode ? "True" : "False");
}

void do_ssid_scan(const struct shell *sh, size_t argc, char **argv)
{
    g_last_ssid_scan_time = 0;    // Force a rescan
    k_timer_start(&S_work_timer, K_MSEC(1000), K_NO_WAIT);
}

#define ENABLE_PARAMS "<1|on|0|off> [default both, S|Q|Both]"
void do_cm_enable(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "Usage: %s " ENABLE_PARAMS, argv[0]);
        return;
    }
    bool enable = argv[1][0] == '1' || strcmp("on", argv[1]) || strcmp("On", argv[1]) == 0;

    if (argc < 3 || argv[2][0] == 'b' || argv[2][0] == 'B') {
        commMgr_enable_S_work(enable);
        commMgr_enable_Q_work(enable);
    } else {
        if (argv[2][0] == 's' || argv[2][0] == 'S') {
            commMgr_enable_S_work(enable);
        } else if (argv[2][0] == 'q' || argv[2][0] == 'Q') {
            commMgr_enable_Q_work(enable);
        }
    }
}

#define ENABLE_FMD_PARAMS "<1|on|0|off> [default both, S|Q|Both]"
void do_fmd_enable(const struct shell *sh, size_t argc, char **argv)
{

    int ret = 0, *err = &ret;
    if (argc > 1 && shell_strtobool(argv[1], 10, err)) {
        enable_fmd_mode(36000);
    } else {
        disable_fmd_mode(FMD_CLOUD_REQUEST);
    }
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_commMgr,
    SHELL_CMD(alert, NULL, "Queue an alert to staging. " ALERT_SEND_PARAMS, do_alert_send),
    SHELL_CMD(conn_test, NULL, "Queue a connectivity msg. " CONNTIVITY_PARAMS, do_connectivity),
    SHELL_CMD(dump_queued_msgs, NULL, "print the contents of the mqtt msg queue. ", do_dump_queued_msgs),
    SHELL_CMD(enable, NULL, "Enable or disable commMgr work. " ENABLE_PARAMS, do_cm_enable),
    SHELL_CMD(fmd, NULL, "Enable or disable fmd. " ENABLE_FMD_PARAMS, do_fmd_enable),
    SHELL_CMD(onboard, NULL, "Send an onboarding msg", do_onboard),
    SHELL_CMD(safe_zone, NULL, "Queue a safe zone alert. " SAFE_ZONE_ALERT_PARAMS, do_safe_zone_alert),
    SHELL_CMD(set_behavior_vars, NULL, "Change the commMgr behavior vars . " COMM_BEHAV_PARAMS, do_set_behavior_vars),
    SHELL_CMD(shadow_report, NULL, "Queue shadow report.", do_report_shadow),
    SHELL_CMD(ssid_scan, NULL, "Move the time to the next SSID scan to 1 second from now.", do_ssid_scan),
    SHELL_CMD(status, NULL, "Display status.", do_cm_status),
    SHELL_CMD(telemetry, NULL, "Queue a telemetry", do_send_telemetry),
    SHELL_CMD(nonce, NULL, "send new NONCE", do_send_srf_nonce),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(commMgr, &sub_commMgr, "Commands to control the Comm Manager", NULL);
