/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
//#include <zephyr.h>
#include "modem.h"
#include "modem_spi.h"
#include "nrf.h"
#include <nrfx_spim.h>
#include <nrfx_gpiote.h>
#include <drivers/nrfx_errors.h>
#include <zephyr/kernel.h>
//#include <zephyr/printk.h>
#include <string.h>
#include "nrfx_spis.h"
#include "nrfx_gpiote.h"
#include <zephyr/sys/printk.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include "d1_json.h"
#include "commMgr.h"
#include "modem_interface_types.h"
#include "uicr.h"
#include <zephyr/zbus/zbus.h>
#include <d1_zbus.h>
#include "pmic.h"
#include <zephyr/logging/log_ctrl.h>
#include "gps.h"
#include "log_telemetry.h"
#include "modem.h"
#include "d1_time.h"
#include "utils.h"
#include "wi.h"

extern shadow_doc_t              shadow_doc;
static const struct gpio_dt_spec spi4cs       = GPIO_DT_SPEC_GET(DT_NODELABEL(spi4cs), gpios);
static const struct gpio_dt_spec spiDataReady = GPIO_DT_SPEC_GET(DT_NODELABEL(modemdataready), gpios);
static int                       modem_spi_send(uint8_t *buf, uint16_t len, uint8_t *buf2);

static struct gpio_callback spi_data_ready_cb;

static version_response_t shadow_version = { 0 };

LOG_MODULE_REGISTER(spi_modem, CONFIG_SPI_MODEM_LOG_LEVEL);

int modem_spi_send_command(modem_message_type_t type, uint8_t *data, uint16_t dataLen, bool reply_requested);

#define SPI_INSTANCE 4
static const nrfx_spim_t spim = NRFX_SPIM_INSTANCE(SPI_INSTANCE);

// This holds a copy of the last status message received from the 9160
volatile modem_status_t modem_status_shadow = {
    .status_flags = 0, .rssi = 0, .fota_state = 0, .fota_percentage = 0, .uptime = 0, .timestamp = { 0 }
};

#define APP_SPIM_CS_PIN   (2)
#define APP_SPIM_SCK_PIN  (8)
#define APP_SPIM_MISO_PIN (32 + 13)
#define APP_SPIM_MOSI_PIN (9)

#define NRF9160_OFFLINE   (0xff)
#define NRF9160_NOT_READY (0xcc)
#define NRF9160_UNKNOWN   (0xfe)

#define NRFX_CUSTOM_ERROR_CODES 0    //used in nrfx_errors.h

#define MAX_MODEM_HANDLES 32
typedef struct
{
    bool     handle_used;
    uint8_t *data;
    uint16_t dataLen;
} modem_handle_t;
modem_handle_t modem_handles[MAX_MODEM_HANDLES];

K_MUTEX_DEFINE(spi_mutex);
K_MUTEX_DEFINE(spi_reply_mutex);

K_SEM_DEFINE(spi_data_ready, 0, 1);

static uint64_t status_count = 0;

static nrfx_spim_config_t spim_config =
    NRFX_SPIM_DEFAULT_CONFIG(APP_SPIM_SCK_PIN, APP_SPIM_MOSI_PIN, APP_SPIM_MISO_PIN, APP_SPIM_CS_PIN);

#define SPIM_RX_BUFF_SIZE 3072
static uint8_t m_rx_buf[SPIM_RX_BUFF_SIZE];

static uint8_t *spim_rx_buff_ptr;    // pointer to rx buffer
static uint8_t *spim_rx_buff;        // pointer to rx buffer
typedef void (*spim_on_rx_cb_t)(uint8_t *data, size_t len, void *user_data);
spim_on_rx_cb_t spim_on_rx_cb          = NULL;
void           *spim_on_rx_cb_userData = NULL;
//static uint8_t* spim_tx_buff; // pointer to tx buffer

volatile bool modem_ready_for_commands = true;    // default to true because LTE is on by default a boot
bool          rebooting_9160           = false;

struct k_work_q modemSpi_recv_work_q;
K_THREAD_STACK_DEFINE(modemSpi_stack_area, 8192);
struct k_work modemSpi_rcv_work;

struct k_work_q modemSpi_Int_work_q;
K_THREAD_STACK_DEFINE(modemSpi_Int_stack_area, 4096);
struct k_work modemSpi_Int_work;

struct k_work_q modemSpi_send_work_q;
K_THREAD_STACK_DEFINE(modemSpi_send_stack_area, 2048);

struct k_work_q modemSpi_utility_work_q;
K_THREAD_STACK_DEFINE(modemSpi_utility_stack_area, 2048);

typedef struct
{
    workref_t           *work;
    message_command_v1_t cmd;
    uint8_t             *data;
    uint16_t             dataLen;
} spi_send_message_work_t;

static cell_info_t current_cell_info;
static uint64_t    last_modem_enable_time = 0;
static uint64_t    last_msg_received_time = 0;
///////////////////////////////
///
///     manual_isr_setup
///
static void manual_isr_setup()
{
    IRQ_DIRECT_CONNECT(SPIM4_IRQn, 0, nrfx_spim_4_irq_handler, 0);
    irq_enable(SPIM4_IRQn);
}


int modem_print_status(modem_status_t *status)
{
    LOG_DBG(
        "LTE Connected:                         %s", ((status->status_flags >> STATUS_LTE_CONNECTED) & 1) ? "yes" : "no");
    LOG_DBG(
        "LTE Enabled (factory test flag):       %s", ((status->status_flags >> STATUS_LTE_ENABLED) & 1) ? "yes" : "no");
    LOG_DBG(
        "LTE Working (connected at least once): %s", ((status->status_flags >> STATUS_LTE_WORKING) & 1) ? "yes" : "no");
    LOG_DBG(
        "FOTA in progress:                      %s",
        ((status->status_flags >> STATUS_FOTA_IN_PROGRESS) & 1) ? "yes" : "no");
    LOG_DBG(
        "MQTT Connected:                        %s",
        ((status->status_flags >> STATUS_MQTT_CONNECTED) & 1) ? "yes" : "no");
    LOG_DBG(
        "GPS Connected:                         %s", ((status->status_flags >> STATUS_GPS_CONNECTED) & 1) ? "yes" : "no");
    LOG_DBG(
        "MQTT Initialized:                      %s",
        ((status->status_flags >> STATUS_MQTT_INITIALIZED) & 1) ? "yes" : "no");
    LOG_DBG(
        "MQTT Enabled:                          %s", ((status->status_flags >> STATUS_MQTT_ENABLED) & 1) ? "yes" : "no");
    LOG_DBG(
        "MQTT Working (connected at least once):%s", ((status->status_flags >> STATUS_MQTT_WORKING) & 1) ? "yes" : "no");
    LOG_DBG("Powered On:                           %s", ((status->status_flags >> STATUS_POWERED_ON) & 1) ? "yes" : "no");
    LOG_DBG(
        "Certs loaded:                          %s", ((status->status_flags >> STATUS_CERTS_LOADED) & 1) ? "yes" : "no");
    LOG_DBG("status_flags:                          0x%x", status->status_flags);
    LOG_DBG("timestamp:                             %s", status->timestamp);
    LOG_DBG("rssi:                                  %d", status->rssi);
    //LOG_DBG("uptime: %lld", status->uptime);
    return 0;
}

void copy_modem_status(modem_status_t *src, modem_status_t *dst)
{
    dst->status_flags    = src->status_flags;
    dst->rssi            = src->rssi;
    dst->fota_state      = src->fota_state;
    dst->fota_percentage = src->fota_percentage;
    dst->uptime          = src->uptime;
    memcpy(dst->timestamp, src->timestamp, 32);
}

cell_info_t modem_spi_getCellInfo()
{
    return current_cell_info;
}

void clear_cell_info()
{
    memset(&current_cell_info, 0, sizeof(cell_info_t));
}
///////////////////////////////////////////////////////
// merge_shadow_status()
//
//  This function compares the current status with the
//  shadow status and returns a bitmask of the changes
//  that have occurred.  It also updates the shadow
//  status with the current status.
//  The bits in the return value correspond to the
//  UPDATE_STATUS_* flags in modem_interface_types.h
//
//  Returns:  uint32_t - bitmask of changes

uint32_t merge_shadow_status(modem_status_t *status)
{
    uint32_t incoming    = status->status_flags;
    uint32_t shadow      = modem_status_shadow.status_flags;
    uint32_t change_bits = incoming ^ shadow;

    if (modem_status_shadow.rssi != status->rssi) {
        change_bits |= UPDATE_STATUS_RSSI;
    }
    if (modem_status_shadow.fota_state != status->fota_state) {
        change_bits |= UPDATE_STATUS_FOTA_STATE;
    }
    if (modem_status_shadow.fota_percentage != status->fota_percentage) {
        change_bits |= UPDATE_STATUS_FOTA_PERCENTAGE;
    }


    modem_status_shadow.status_flags    = status->status_flags;
    modem_status_shadow.rssi            = status->rssi;
    modem_status_shadow.fota_state      = status->fota_state;
    modem_status_shadow.fota_percentage = status->fota_percentage;
    memcpy(
        (void *)(&(modem_status_shadow.uptime)),
        &(status->uptime),
        sizeof(uint64_t));    // Need to do this because incoming structure isn't gguarenteeded to be aligned
    memcpy((void *)(modem_status_shadow.timestamp), status->timestamp, 32);
    return change_bits;
}

/**
 * process an incoming message
 * @returns < 0 an error occurred.
 *            0 message processed, no further action required
 *            1 message processed, followup action may be needed
 */
int process_async_message(message_command_v1_t *cmd, uint8_t *data_ptr)
{
    //LOG_DBG("process_async_message: %d", cmd->messageType);
    // these are special in that they are initiated from the 9160 (or thru the cloud) and are not a response to a command
    // general status and incoming MQTT messages are examples
    switch (cmd->messageType) {
    case MESSAGE_TYPE_DEVICE_STATUS:
        if (cmd->dataLen == sizeof(modem_status_t)) {
            //
            // modem_status_t *statusTest = (modem_status_t*)(((uint8_t*)cmd));
            // if (statusTest->status_flags == 0xFEFEFE00) {
            //     LOG_ERR("bad status packet, ignoring");
            //     LOG_HEXDUMP_ERR(cmd, cmd->dataLen, "Status:");
            //     return 0;
            // }
            // LOG_ERR("status flags = %d", statusTest->status_flags);
            // LOG_ERR("status lte connected = %s", ((statusTest->status_flags >> STATUS_LTE_CONNECTED) & 1) ? "yes" : "no");
            // LOG_ERR("status mqtt enabled = %s", ((statusTest->status_flags >> STATUS_MQTT_ENABLED) & 1) ? "yes" : "no");
            // LOG_ERR("status mqtt connected = %s", ((statusTest->status_flags >> STATUS_MQTT_CONNECTED) & 1) ? "yes" : "no");
            // LOG_ERR("status mqtt init'd = %s", ((statusTest->status_flags >> STATUS_MQTT_INITIALIZED) & 1) ? "yes" : "no");
            // LOG_ERR("proccessing/peeking at status message - handle %d", cmd->messageHandle);
            modem_status_t *status = (modem_status_t *)data_ptr;
            //modem_print_status(status);
            status_count++;
            status->status_flags |= (1 << STATUS_POWERED_ON);    // force this bit on, as we are not powered off

            if (!(status->status_flags & (1 << STATUS_LTE_ENABLED))) {
                if (uicr_shipping_flag_get() || CONFIG_NRF9160_ENABLE_ON_BOOT) {
                    uint64_t now = utils_get_currentmillis();
                    if (now - last_modem_enable_time > 100) {
                        modem_enable();
                        LOG_INF("Shipping flag is set or pre-DVT unit, enabling modem (uptime=%llu)", status->uptime);
                        last_modem_enable_time = now;
                        send_version_no_reply();    // its clearly rebooted, or first boot, so toss in an update to version.  if rebooted, this will update the cached version
                    }
                }
            }

            if (strlen(status->timestamp) > 10) {
                if (status->timestamp[0] > 32 && status->timestamp[1] > 32) {
                    d1_set_5340_time(status->timestamp, false);
                }
            }

            modem_status_update_t modem_status_update;
            copy_modem_status(status, &modem_status_update.status);
            //modem_print_status(&modem_status_update.status);
            //modem_print_status((modem_status_t*)&modem_status_shadow);
            modem_status_update.change_bits = merge_shadow_status(status);
            //LOG_DBG("status change bits(%llu): %d %08x %08x", status_count, modem_status_update.change_bits, modem_status_update.status.status_flags, status->status_flags);
            int err = zbus_chan_pub(&LTE_STATUS_UPDATE, &modem_status_update, K_MSEC(1000));
            if (err) {
                LOG_ERR("zbus_chan_pub, error: %d", err);
                return err;
            }

        } else {
            //LOG_HEXDUMP_WRN(cmd, 128, "Oversize status message");
            //LOG_WRN("Unexpected status length: %d, expected %d", cmd->dataLen, sizeof(modem_status_t));
            return -EINVAL;
        }

        if (cmd->messageHandle == 255) {
            // this is a status update, not a response to a specific status request by some other part of the code
            //LOG_ERR("status message handled - %d", cmd->messageHandle);
            return 0;
        }
        return 1;
    case MESSAGE_TYPE_CELL_INFO:
        LOG_DBG("Got cell info message - %d bytes", cmd->dataLen);
        cell_info_t *call_info = (cell_info_t *)(data_ptr);
        current_cell_info      = *call_info;
        LOG_DBG("Cell Info: CellID: %s", call_info->cellID);
        LOG_DBG("Cell Info: Ip: %d.%d.%d.%d", call_info->ip[0], call_info->ip[1], call_info->ip[2], call_info->ip[3]);
        LOG_DBG("Cell Info: Rssi: %d", call_info->rssi);
        LOG_DBG("Cell Info: Mode: %s", ((call_info->lte_nbiot_mode == 0) ? "LTE-m" : "NB-IoT"));
        LOG_DBG("Cell Info: Band: %d", call_info->lte_band);
        LOG_DBG("Cell Info: Tracking area: %d (0x04%x)", call_info->tracking_area, call_info->tracking_area);
        LOG_DBG("Cell Info: MCC: %d", call_info->mcc);
        LOG_DBG("Cell Info: MNC: %d", call_info->mnc);

        current_cell_info = *call_info;

        //LOG_HEXDUMP_DBG((uint8_t*)cmd, cmd->dataLen, "Cell Info:");
        break;
    case MESSAGE_TYPE_DEVICE_INFO:
        // cache the stuff so we dont need to ask again, set flag and return cache in modem.c:modem_get_info
        break;
    case MESSAGE_TYPE_GPS_DATA:
        //LOG_DBG("Got GPS data message - %d bytes", cmd->dataLen);
        gps_info_t *gps = (gps_info_t *)(data_ptr);
        if (gps->timeToLock == 0) {
            LOG_WRN(
                "GPS: no lock in %d seconds (tracked sats: %d) time = %lld",
                gps->secSinceLock,
                gps->numSatellites,
                gps->timestamp);
            return 0;
        }

        if (gps->latitude == 0 || gps->longitude == 0) {
            LOG_WRN("GPS: lat/lon is 0, skipping");
            return -EINVAL;
        } else {
            LOG_DBG(
                "GPS: lat: %lld lon: %lld alt: %d speed: %u sats: %u",
                gps->latitude,
                gps->longitude,
                gps->altitude,
                gps->speed,
                gps->numSatellites);
            LOG_DBG(
                "GPS: heading: %u acc: %u spd_acc: %u padd=%u", gps->heading, gps->accuracy, gps->speed_accuracy, gps->pad1);
            LOG_DBG("GPS: timestamp: %lld", gps->timestamp);
            LOG_DBG("GPS: secSinceLock: %d timeToLock: %d", gps->secSinceLock, gps->timeToLock);
        }

        gps_add(gps);
        break;
    case MESSAGE_TYPE_LTE_DEBUG:
        //LOG_DBG("Got LTE debug message - %d bytes", cmd->dataLen);
        //LOG_HEXDUMP_DBG((uint8_t*)cmd, cmd->dataLen, "LTE Debug:");
        debug_info_message_t *debug_msg = (debug_info_message_t *)(data_ptr);
        if (debug_msg->debug_string_length > 0) {
            char *debug_str_mem = k_malloc(debug_msg->debug_string_length + 1);
            if (debug_str_mem == NULL) {
                LOG_ERR("debug string k_malloc failed");
                return -ENOMEM;
            }
            strncpy(
                debug_str_mem,
                (char *)(((uint8_t *)debug_msg) + sizeof(debug_info_message_t)),
                debug_msg->debug_string_length);
            debug_str_mem[debug_msg->debug_string_length] = '\0';
            // get the high bit of debug_msg->debug_level
            bool upload_log = (debug_msg->debug_level & 0x0080) ? true : false;
            debug_msg->debug_level &= 0x007F;
            //LOG_DBG("incoming 9160 debug message: %d %d %d", debug_msg->debug_string_length, upload_log, debug_msg->debug_level);
            //LOG_HEXDUMP_DBG(debug_msg, sizeof(debug_info_message_t) + debug_msg->debug_string_length + 32, "Debug Msg:");
            int log_type = TELEMETRY_LOG_TYPE_INFO;
            switch (debug_msg->debug_level) {
            case 4:
                if (upload_log) {
                    LOG_TELEMETRY_DBG(log_type, "9160 log message (%d): %s", debug_msg->error_code, debug_str_mem);
                } else {
                    LOG_DBG("9160 log message (%d): %s", debug_msg->error_code, debug_str_mem);
                }
                break;
            case 3:
                if (upload_log) {
                    LOG_TELEMETRY_INF(log_type, "9160 log message (%d): %s", debug_msg->error_code, debug_str_mem);
                } else {
                    LOG_INF("9160 log message (%d): %s", debug_msg->error_code, debug_str_mem);
                }
                break;
            case 2:
                if (upload_log) {
                    LOG_TELEMETRY_WRN(log_type, "9160 log message (%d): %s", debug_msg->error_code, debug_str_mem);
                } else {
                    LOG_WRN("9160 log message (%d): %s", debug_msg->error_code, debug_str_mem);
                }
                break;
            case 1:
                if (upload_log) {
                    LOG_TELEMETRY_ERR(log_type, "9160 log message (%d): %s", debug_msg->error_code, debug_str_mem);
                } else {
                    LOG_ERR("9160 log message (%d): %s", debug_msg->error_code, debug_str_mem);
                }
                break;
            default:
                break;
            }
            k_free(debug_str_mem);
        }

        return 0;
        break;
    case MESSAGE_TYPE_MQTT:
        // we got JSON/MQTT from the cloud, do something with it

        if (cmd->dataLen > 0) {
            //data_ptr = (char*)(m_rx_buf);

            uint16_t topic_len = (data_ptr[1] << 8 | data_ptr[0]);
            uint16_t msg_len   = (data_ptr[3] << 8 | data_ptr[2]);
            char    *topic     = k_malloc(topic_len);
            if (topic == NULL) {
                LOG_ERR("topic k_malloc failed");
                return -ENOMEM;
            }
            char *message = k_malloc(msg_len);
            if (message == NULL) {
                LOG_ERR("message k_malloc failed");
                k_free(topic);
                return -ENOMEM;
            }
            memset(topic, 0, topic_len);
            memset(message, 0, msg_len);
            memcpy(topic, data_ptr + 4, topic_len);
            //topic[topic_len] = '\0';
            memcpy(message, data_ptr + 4 + topic_len, msg_len);
            //message[msg_len] = '\0';

            //LOG_DBG("Modem JSON lengths: t:%d m:%d", topic_len, msg_len);
            //LOG_DBG("Modem JSON message(%d): %s", msg_len, message);

            mqtt_payload_t new_mqtt;
            new_mqtt.topic          = topic;
            new_mqtt.topic_length   = topic_len;
            new_mqtt.payload        = message;
            new_mqtt.payload_length = strlen(new_mqtt.payload);
            new_mqtt.qos            = 0;
            new_mqtt.radio          = COMM_DEVICE_NRF9160;

            static char start[256];
            static char end[256];
            snprintf(start, 256, "%s", new_mqtt.payload);
            snprintf(end, 256, "%s", (new_mqtt.payload + (new_mqtt.payload_length - 255)));
            LOG_DBG("Received message from cloud(%d/%d): %s...%s", msg_len, strlen(new_mqtt.payload), start, end);

            // send to zbus
            int err = zbus_chan_pub(&MQTT_CLOUD_TO_DEV_MESSAGE, &new_mqtt, K_SECONDS(1));
            if (err) {
                LOG_ERR("zbus_chan_pub, error: %d", err);
                k_free(topic);
                k_free(message);
                return -1;
            }
            // do not free here, the zbus will free it
            return 0;
        }
        break;
    case MESSAGE_TYPE_COMMAND_RESP:
        //data_ptr = (char*)(m_rx_buf);
        command_type_t cmdType = (command_type_t) * (data_ptr);    // only valid if cmd == MESSAGE_TYPE_COMMAND_RESP
        switch (cmdType) {
        case COMMAND_GET_VERSION:
            LOG_DBG("Got version response - caching results");
            version_response_t *ver_reply = (version_response_t *)(data_ptr + 1);
            shadow_doc.lteVer[0] = shadow_version.major = ver_reply->major;
            shadow_doc.lteVer[1] = shadow_version.minor = ver_reply->minor;
            shadow_doc.lteVer[2] = shadow_version.patch = ver_reply->patch;
            strncpy(shadow_version.githash, ver_reply->githash, 16);
            strncpy(shadow_version.build_date, ver_reply->build_date, 32);
            strncpy(shadow_version.build_machine, ver_reply->build_machine, 16);
            strncpy(shadow_version.modem_fw, ver_reply->modem_fw, 32);
            // LOG_INF("9160 Version: %d.%d.%d (%s)", shadow_version.major, shadow_version.minor, shadow_version.patch, shadow_version.githash);
            // LOG_INF("Build Date: %s", shadow_version.build_date);
            // LOG_INF("Build Machine: %s", shadow_version.build_machine);
            // LOG_INF("Modem FW: %s", shadow_version.modem_fw);
            if (cmd->messageHandle == 255) {
                // this is a status update, not a response to a specific status request by some other part of the code
                return 0;
            }
            return 1;    // let whoever asked for this handle it
        default:
            break;
        }
        break;
    default:
        break;
    }

    if (cmd->messageHandle == 255) {
        // this is a status update, not a response to a specific status request by some other part of the code
        return 0;
    }

    return 1;
}

static void spi_send_work_handler(struct k_work *work)
{
    workref_t               *wr       = CONTAINER_OF(work, workref_t, work);
    spi_send_message_work_t *msg      = (spi_send_message_work_t *)wr->reference;
    int                      retryCnt = 0;
    int                      ret      = -1;
    while (ret != 0) {
        // in case there were any work items already queued when the modem was powered off, we need to check here
        if (pmic_is_9160_powered() == PMIC_LTE_POWER_OFF) {
            LOG_ERR("Modem is not powered on, cannot send message");
            // TODO: need to build a response to the message handle here, it it was not a 255
            break;
        }

        // if its powered on BUT we havent heard from it yet, we need to wait
        int waitCnt = 0;
        while (modem_ready_for_commands == false) {
            if (waitCnt > 25) {
                LOG_ERR("Modem is not ready, giving up");
                goto cleanup;
            }
            //LOG_DBG("Modem is not ready for commands, waiting");
            k_sleep(K_MSEC(100));
            waitCnt++;
        }

        //LOG_HEXDUMP_ERR(msg->data, msg->dataLen, "Sending:");
        ret = modem_spi_send(msg->data, msg->dataLen, NULL);
        if (ret != 0) {
            if (retryCnt > 20) {
                LOG_ERR("modem_spi_send failed %d, giving up", ret);
                break;
            }
            k_sleep(K_MSEC(20));
            retryCnt++;
        }

        // if the message is a COMMAND_HARD_STOP, then send it and call pmic_power_off_modem(false)
        // this has to be done here to ensure no race conditions with the modem being powered off and the message going out.
        if (msg->data[1] == MESSAGE_TYPE_COMMAND && msg->data[sizeof(message_command_v1_t)] == COMMAND_HARD_STOP) {
            LOG_WRN("sent hard stop command, now powering down modem");
            modem_ready_for_commands = false;
            pmic_power_off_modem(false);
        }
    }
cleanup:
    if (msg->data) {
        k_free(msg->data);
    }
    k_free(msg);
    wr_put(wr);
}


void send_version_no_reply()
{
    if (modem_is_powered_on() == false) {
        return;
    }
    LOG_DBG("sending 9160 version command");
    uint8_t cmd = COMMAND_GET_VERSION;
    modem_send_command(MESSAGE_TYPE_COMMAND, &cmd, 1, false);
}

void send_status_no_reply()
{
    if (modem_is_powered_on() == false) {
        return;
    }
    //LOG_DBG("sending 9160 status command");
    modem_send_command(MESSAGE_TYPE_DEVICE_STATUS, NULL, 0, false);
}

int modem_spi_stop_waiting_for_resp(int handle)
{
    if (handle >= MAX_MODEM_HANDLES) {
        return -1;
    }
    k_mutex_lock(&spi_reply_mutex, K_MSEC(1));
    if (modem_handles[handle].handle_used) {
        k_free(modem_handles[handle].data);
        modem_handles[handle].handle_used = false;
    }
    k_mutex_unlock(&spi_reply_mutex);
    return 0;
}


void modem_alive_check_work_handler(struct k_work *work)
{
    static uint32_t num_fails = 0;

    if (modem_is_powered_on() == false) {
        return;
    }
    uint64_t now = k_uptime_get();
    if (now - last_msg_received_time < 10000) {
        // last msg was recent (10 seconds), just return
        num_fails = 0;
        return;
    }

    extern bool    do_simulate_fail;
    modem_status_t status;
    int            ret = modem_get_status(&status);
    if (ret < 0 || do_simulate_fail) {
        if (commMgr_fota_in_progress()) {
            LOG_DBG("modem alive_check: FOTA in progress, not depowering");
            return;
        } else {
            LOG_ERR("modem alive_check failed (%d), setting modem to offline", ret);
            set_modem_power_state(false);
            modem_power_off();
            do_simulate_fail = false;
            num_fails++;
        }
        if (num_fails > 10) {
            LOG_ERR("Modem does not appear to have come to life after %d attempts - reboot", num_fails);
            LOG_PANIC();
            pmic_reboot("Modem died");
        }
        return;
    }
    //LOG_DBG("modem alive_check: It's Alive!");
}
K_WORK_DEFINE(my_alive_check_work, modem_alive_check_work_handler);
void alive_check_timer_handler(struct k_timer *dummy)
{
    k_work_submit_to_queue(&modemSpi_utility_work_q, &my_alive_check_work);
}
K_TIMER_DEFINE(alive_check_work_timer, alive_check_timer_handler, NULL);


///////////////////////////////
///
///     spim_recv_action_work_handler
///
static void spim_recv_action_work_handler(struct k_work *work)
{
    workref_t               *wr  = CONTAINER_OF(work, workref_t, work);
    spi_send_message_work_t *msg = (spi_send_message_work_t *)wr->reference;

    message_command_v1_t *cmd = &msg->cmd;

    // LOG_WRN("spim_recv_action_work_handler: messageHandle: %u, messageType: %d, dataLen: %d", cmd->messageHandle, cmd->messageType, cmd->dataLen);
    // LOG_HEXDUMP_ERR(msg->data, cmd->dataLen, "work_handler Received:");
    command_type_t cmdType = (command_type_t) * (msg->data);    // only valid if cmd == MESSAGE_TYPE_COMMAND_RESP

    if (cmd->messageHandle == 255 && (cmd->messageType == MESSAGE_TYPE_NO_OP || cmd->messageType == MESSAGE_TYPE_NULL)) {
        goto cleanup;    // handle for NO_OP, theres nothing to do here.
    }

    // if the modem is shutting down, it might still send some status.  this seems to confuse everyone downstream, so we just ignore it.
    if (modem_is_powered_on() == false) {
        goto cleanup;
    }

    //LOG_ERR("spim_recv_action_work_handler: messageHandle: %d, messageType: %d, dataLen: %d", cmd->messageHandle, cmd->messageType, cmd->dataLen);
    // handle messages that are clearly async and not a response to a command
    if (cmd->messageHandle == 255) {
        if (process_async_message(cmd, (uint8_t *)(msg->data)) <= 0) {
            // the message was properly handled and we can return
            //LOG_DBG("async message handled: %d", cmd->messageType);
            goto cleanup;
        }
    } else {
        // go ahead and process these as well if they match the right types
        // even if someone asked for this, we should update any status' or info while its passing thru.
        if (cmd->messageType == MESSAGE_TYPE_DEVICE_INFO || cmd->messageType == MESSAGE_TYPE_DEVICE_STATUS
            || (cmd->messageType == MESSAGE_TYPE_COMMAND_RESP && cmdType == COMMAND_GET_VERSION)) {
            int pam_ret = process_async_message(cmd, (uint8_t *)(msg->data));
            if (pam_ret <= 0) {
                // do NOT return as above, just pretend nothing happened, we were just peeking at the message
                LOG_DBG("info/status message consumed");
                goto cleanup;
            } else if (pam_ret == -1) {
                // this is an error
                LOG_ERR("info/status message error");
                goto cleanup;
            } else {
                // this is a message that was not handled, but not an error
                LOG_DBG("info/status message not consumed");
            }
        }
    }

    uint16_t dataLen = cmd->dataLen;    //(m_rx_buf[3] << 8) + (m_rx_buf[4]) + 6;
    if (msg->data[0] == NRF9160_NOT_READY && msg->data[1] == NRF9160_NOT_READY) {
        // commented because it breaks the passthru shell to print this all the time.  It's OK to happen
        //LOG_ERR("spim_recv_action_work_handler: SPIS busy or ignoring - 0xcc");
        goto cleanup;
    }
    if (msg->data[0] == NRF9160_UNKNOWN && msg->data[1] == NRF9160_UNKNOWN) {
        LOG_ERR("spim_recv_action_work_handler: SPIS overread - 0xfe");
        goto cleanup;
    }
    if (dataLen > SPIM_RX_BUFF_SIZE) {
        LOG_ERR("spim_recv_action_work_handler: SPIS overread - dataLen > SPIM_RX_BUFF_SIZE");
        goto cleanup;
    }

    if (spim_on_rx_cb) {
        spim_on_rx_cb(msg->data, dataLen, spim_on_rx_cb_userData);
    }

    if (cmd->messageHandle >= MAX_MODEM_HANDLES) {
        //LOG_DBG("message handle too large: %d", cmd->messageHandle);
        goto cleanup;
    }

    //LOG_ERR("spim_recv_action_work_handler IMPORTANT: messageHandle: %d, messageType: %d, dataLen: %d", cmd->messageHandle, cmd->messageType, dataLen);
    //LOG_HEXDUMP_ERR(msg->data, dataLen, "Received:");
    int mutex_ret = k_mutex_lock(&spi_reply_mutex, K_MSEC(1));
    if (mutex_ret != 0) {
        LOG_DBG("spim mutex lock failed try again\n");
        goto cleanup;
    }

    for (int i = 0; i < MAX_MODEM_HANDLES; i++) {
        if (modem_handles[i].handle_used && (i == cmd->messageHandle)) {
            //LOG_DBG("work_handler: found handle of waiting request: %d", i);
            modem_handles[i].data = k_malloc(dataLen);
            if (modem_handles[i].data == NULL) {
                LOG_ERR("work_handler: k_malloc failed");
                k_mutex_unlock(&spi_reply_mutex);
                goto cleanup;
            }
            LOG_DBG("work_handler: alloc %d", dataLen);
            memcpy(modem_handles[i].data, msg->data, dataLen);
            modem_handles[i].dataLen = dataLen;
            break;
        }
    }
    k_mutex_unlock(&spi_reply_mutex);

cleanup:
    if (msg->data)
        k_free(msg->data);
    k_free(msg);
    wr_put(wr);
}

///////////////////////////////
///
///     spim_event_handler
///
void spim_event_handler(nrfx_spim_evt_t const *p_event, void *p_context)
{
    // if (p_event->type == NRFX_SPIM_EVENT_DONE) {    // NRFX_SPIM_EVENT_DONE is the ONLY type... literally the only one defined in the nrfx_spim.h file
    //     //LOG_DBG("spim_event_handler: freeing spim_tx_buff");
    //     if (spim_tx_buff) { k_free(spim_tx_buff); spim_tx_buff = NULL; }
    // }
    //     gpio_pin_set_dt(&spi4cs, 1);
    //     if (m_rx_buf[0] != 0xff)
    //     {
    //         k_work_submit_to_queue(&modemSpi_recv_work_q, &modemSpi_rcv_work);
    //     }
}
///////////////////////////////
///
///     modem_spi_set_rx_cb
///
void modem_spi_set_rx_cb(void (*cb)(uint8_t *data, size_t len, void *user_data), void *user_data)
{
    spim_on_rx_cb          = cb;
    spim_on_rx_cb_userData = user_data;
}

void spi_rx_app_cb(uint8_t *data, size_t len, void *user_data)
{
    //LOG_ERR("Got %d bytes back\n",len);
}

void dataready_int_work_handler(struct k_work *work)
{
    //LOG_DBG("Dataready interrupt");

    // can still get INT while the modem is powering down, ignore
    if (modem_is_powered_on() == false) {
        return;
    }
    modem_ready_for_commands = true;
    modem_send_command(MESSAGE_TYPE_NO_OP, NULL, 0, false);
}


void dataready_int_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    int ret = k_work_submit_to_queue(&modemSpi_Int_work_q, &modemSpi_Int_work);
    if (ret <= 0) {
        LOG_ERR("Could not queue int work: %d", ret);
    }
}

///////////////////////////////
///
///     modem_spi_init
///
int modem_spi_init(void)
{
    LOG_DBG("SPIM setup");
    spim_rx_buff     = NULL;
    spim_rx_buff_ptr = NULL;

    int ret = gpio_pin_configure_dt(&spi4cs, GPIO_OUTPUT);

    if (ret != 0) {
        LOG_ERR("Error %d: failed to configure %s pin %d\n", ret, spi4cs.port->name, spi4cs.pin);
        return -1;
    }
    gpio_pin_set_dt(&spi4cs, 1);

    spim_config.frequency = NRFX_MHZ_TO_HZ(4);

    if (NRFX_SUCCESS != nrfx_spim_init(&spim, &spim_config, NULL, NULL)) {
        LOG_ERR("Init Failed\n");
        return 0;
    }
    for (int i = 0; i < MAX_MODEM_HANDLES; i++) {
        modem_handles[i].data    = NULL;
        modem_handles[i].dataLen = 0;
    }

    modem_spi_set_rx_cb(spi_rx_app_cb, NULL);
    manual_isr_setup();

    if (gpio_pin_configure_dt(&spiDataReady, GPIO_INPUT) != 0) {
        LOG_ERR("Error: failed to configure %s pin %d\n", spiDataReady.port->name, spiDataReady.pin);
        return -1;
    }
    // enable interrupt on button for rising edge
    if (gpio_pin_interrupt_configure_dt(&spiDataReady, GPIO_INT_EDGE_TO_ACTIVE) != 0) {
        LOG_ERR("Error: failed to configure interrupt on %s pin %d\n", spiDataReady.port->name, spiDataReady.pin);
        return -1;
    }
    // initialize callback structure for button interrupt
    gpio_init_callback(&spi_data_ready_cb, dataready_int_handler, BIT(spiDataReady.pin));

    // attach callback function to button interrupt
    gpio_add_callback(spiDataReady.port, &spi_data_ready_cb);

    k_work_queue_init(&modemSpi_recv_work_q);
    struct k_work_queue_config modemSpi_recv_work_q_cfg = {
        .name     = "modemSpi_recv_work_q",
        .no_yield = 0,
    };
    k_work_queue_start(
        &modemSpi_recv_work_q, modemSpi_stack_area, K_THREAD_STACK_SIZEOF(modemSpi_stack_area), 3, &modemSpi_recv_work_q_cfg);


    k_work_queue_init(&modemSpi_Int_work_q);
    struct k_work_queue_config modemSpi_Int_work_q_cfg = {
        .name     = "modemSpi_Int_work_q",
        .no_yield = 0,
    };
    k_work_queue_start(
        &modemSpi_Int_work_q,
        modemSpi_Int_stack_area,
        K_THREAD_STACK_SIZEOF(modemSpi_Int_stack_area),
        1,
        &modemSpi_Int_work_q_cfg);
    k_work_init(&modemSpi_Int_work, dataready_int_work_handler);

    k_work_queue_init(&modemSpi_send_work_q);
    struct k_work_queue_config modemSpi_send_work_q_cfg = {
        .name     = "modemSpi_send_work_q",
        .no_yield = 0,
    };
    k_work_queue_start(
        &modemSpi_send_work_q,
        modemSpi_send_stack_area,
        K_THREAD_STACK_SIZEOF(modemSpi_send_stack_area),
        2,
        &modemSpi_send_work_q_cfg);

    k_work_queue_init(&modemSpi_utility_work_q);
    struct k_work_queue_config modemSpi_utility_work_q_cfg = {
        .name     = "modemSpi_utility_work_q",
        .no_yield = 0,
    };
    k_work_queue_start(
        &modemSpi_utility_work_q,
        modemSpi_utility_stack_area,
        K_THREAD_STACK_SIZEOF(modemSpi_utility_stack_area),
        2,
        &modemSpi_utility_work_q_cfg);


    modem_power_on();

    // send the first request to start them communicating, based on reply various things will kick off, like mqtt initialization
    LOG_WRN("Request 9160 status");
    send_status_no_reply();
    send_version_no_reply();
    k_timer_start(&alive_check_work_timer, K_MSEC(10000), K_MSEC(5000));    // wait 10 sec, then check every 5 sec
    return 0;
}


///////////////////////////////
///
///     modem_spi_send
///
static int modem_spi_send(uint8_t *buf, uint16_t len, uint8_t *buf2)
{
    int ret = 0;

    if (modem_is_powered_on() == false) {
        LOG_ERR("Modem is not powered on, cannot send message");
        //TODO: // need to build a response to the message handle here, if it was not a 255
        return -1;
    }

    memset(m_rx_buf, 0, SPIM_RX_BUFF_SIZE);
    nrfx_spim_xfer_desc_t xfer_desc = NRFX_SPIM_XFER_TRX(buf, len, m_rx_buf, SPIM_RX_BUFF_SIZE);

    int        retries = 0;
    nrfx_err_t err_code;

    do {
        gpio_pin_set_dt(&spi4cs, 0);
        k_sleep(K_USEC(20));
        err_code = nrfx_spim_xfer(&spim, &xfer_desc, 0);
        if (err_code == NRFX_ERROR_BUSY) {
            LOG_ERR("SPI busy");
            k_sleep(K_USEC(100));
            continue;
        } else if (err_code != NRFX_SUCCESS) {
            LOG_ERR("Error code = 0x%x\n", err_code);
            ret = -EIO;
            break;
        } else if (err_code == NRFX_SUCCESS) {
            if (m_rx_buf[0] == NRF9160_NOT_READY && m_rx_buf[1] == NRF9160_NOT_READY) {
                LOG_ERR("SPIM retrying ... not ready");
                gpio_pin_set_dt(&spi4cs, 1);
                k_sleep(K_USEC(100));
                err_code = NRFX_ERROR_INVALID_STATE;
                continue;
            }
            ret = 0;
        }
    } while (err_code != NRFX_SUCCESS && retries++ < 5);

    nrfx_spim_abort(&spim);    //go to low power spi mode
    gpio_pin_set_dt(&spi4cs, 1);
    if ((m_rx_buf[0] != NRF9160_UNKNOWN) && (m_rx_buf[0] != NRF9160_OFFLINE) && (m_rx_buf[0] != NRF9160_NOT_READY)) {
        // there is some response data ... handle it

        message_command_v1_t *cmd = (message_command_v1_t *)m_rx_buf;
        if (cmd->dataLen > SPIM_RX_BUFF_SIZE) {
            LOG_ERR("spim_recv_action_work_handler: SPIS overread - dataLen > SPIM_RX_BUFF_SIZE");
            return -1;
        }
        //LOG_DBG("modem_spi_send incoming data: messageHandle: %d, messageType: %d, dataLen: %d", cmd->messageHandle, cmd->messageType, cmd->dataLen);

        spi_send_message_work_t *rx_data = k_malloc(sizeof(spi_send_message_work_t));
        if (rx_data == NULL) {
            LOG_ERR("k_malloc failed 1");
            return -1;
        }

        rx_data->work = wr_get(rx_data, __LINE__);
        if (rx_data->work == NULL) {
            LOG_ERR("Out of work items for %s", __func__);
            k_free(rx_data);
            return -1;
        }
        k_work_init(&rx_data->work->work, spim_recv_action_work_handler);
        rx_data->data = k_malloc(cmd->dataLen);
        if (rx_data->data == NULL) {
            k_free(rx_data);
            LOG_ERR("k_malloc failed 2 - len = %d", cmd->dataLen);
            return -1;
        }
        rx_data->cmd.version       = cmd->version;
        rx_data->cmd.messageType   = cmd->messageType;
        rx_data->cmd.messageHandle = cmd->messageHandle;
        rx_data->cmd.dataLen       = cmd->dataLen;
        rx_data->cmd.chunkNum      = cmd->chunkNum;
        rx_data->cmd.chunkTotal    = cmd->chunkTotal;

        memcpy(rx_data->data, m_rx_buf + sizeof(message_command_v1_t), cmd->dataLen);

        rx_data->dataLen       = cmd->dataLen;
        last_msg_received_time = k_uptime_get();
        ret                    = k_work_submit_to_queue(&modemSpi_recv_work_q, &rx_data->work->work);
        if (ret <= 0) {
            LOG_ERR("Could not submit receive work: %d", ret);
            k_free(rx_data->data);
            k_free(rx_data);
        } else {
            ret = 0;
        }
    }
    if (buf2 != NULL) {
        memcpy(buf2, m_rx_buf, len);
    }

    return ret;
}

///////////////////////////////
///
///     modem_spi_recv
///
int modem_spi_recv(char *buf, k_timeout_t timeout)
{
    return 0;
}


///////////////////////////////
///
///     get_next_handle_id
///
int get_next_handle_id()
{
    for (int i = 1; i < MAX_MODEM_HANDLES; i++) {
        if (modem_handles[i].handle_used == false) {
            modem_handles[i].handle_used = true;
            return i;
        }
    }
    return -EADDRNOTAVAIL;
}


///////////////////////////////
///
///     modem_spi_free_reply_data
///
int modem_spi_free_reply_data(int handle)
{
    if (handle < MAX_MODEM_HANDLES) {
        modem_handles[handle].handle_used = false;
        if (modem_handles[handle].data != NULL) {
            k_free(modem_handles[handle].data);
            modem_handles[handle].data = NULL;
        }
        modem_handles[handle].dataLen = 0;
        return 0;
    }
    return -1;
}


///////////////////////////////
///
///     modem_spi_free_reply_data_but_not_handle
///
int modem_spi_free_reply_data_but_not_handle(int handle)
{
    if (handle < MAX_MODEM_HANDLES) {
        if (modem_handles[handle].data != NULL) {
            k_free(modem_handles[handle].data);
            modem_handles[handle].data = NULL;
        }
        modem_handles[handle].dataLen = 0;
        return 0;
    }
    return -1;
}

///////////////////////////////
///
///     modem_spi_send_command
///
int modem_spi_send_command(modem_message_type_t type, uint8_t *data, uint16_t dataLen, bool reply_requested)
{
    int newHandle = 255;
    int ret       = 0;

    if (reply_requested) {
        newHandle = get_next_handle_id();
        if (newHandle < 0) {
            LOG_ERR("no available handles");
            return -newHandle;
        }
    }

    uint8_t *txmsg = k_malloc(dataLen + sizeof(message_command_v1_t));    // free'd in send work handler, when xfer is done
    if (!txmsg) {
        LOG_ERR("k_malloc failed  - %d", dataLen + sizeof(message_command_v1_t));
        return -3;
    }

    message_command_v1_t *cmd = (message_command_v1_t *)txmsg;
    cmd->version              = 0x01;
    cmd->messageType          = type;
    cmd->messageHandle        = newHandle;
    cmd->dataLen              = dataLen;
    cmd->chunkNum             = 0;
    cmd->chunkTotal           = 1;

    //LOG_ERR("modem_spi_send_command: %d %d %d", cmd->messageType, cmd->messageHandle, cmd->dataLen);

    for (int i = 0; i < dataLen; i++) {
        txmsg[i + sizeof(message_command_v1_t)] = data[i];
    }

    spi_send_message_work_t *tx_data = k_malloc(sizeof(spi_send_message_work_t));
    if (!tx_data) {
        k_free(txmsg);
        LOG_ERR("k_malloc failed 4");
        return -ENOMEM;
    }

    tx_data->work = wr_get(tx_data, __LINE__);
    if (tx_data->work == NULL) {
        k_free(txmsg);
        k_free(tx_data);
        LOG_ERR("No work items");
        return -ENOMEM;
    }
    k_work_init(&tx_data->work->work, spi_send_work_handler);
    tx_data->data    = txmsg;
    tx_data->dataLen = dataLen + sizeof(message_command_v1_t);

    ret = k_work_submit_to_queue(&modemSpi_send_work_q, &tx_data->work->work);
    if (ret <= 0) {
        LOG_ERR("Failed to queue send work: %d", ret);
        ret = 0;
        k_free(txmsg);
        k_free(tx_data);
    }

    if (reply_requested) {
        return newHandle;
    }
    return 0;
}


int modem_spi_recv_resp(uint8_t handle, uint8_t *data, uint16_t *dataLen, int timeout)
{
    uint16_t maxlen = *dataLen;
    //k_sleep(K_MSEC(50));

    // check if response to handle is ready
    int mutex_ret = k_mutex_lock(&spi_reply_mutex, K_MSEC(timeout));
    if (mutex_ret != 0) {
        LOG_DBG("spim mutex lock failed try again\n");
        return -1;
    }

    if (modem_handles[handle].data != NULL) {
        if (maxlen < modem_handles[handle].dataLen) {
            LOG_ERR("data buffer %d too small for %d", maxlen, modem_handles[handle].dataLen);
            k_mutex_unlock(&spi_reply_mutex);
            return -ENOMEM;
        }
        memcpy(data, modem_handles[handle].data, modem_handles[handle].dataLen);
        *dataLen = modem_handles[handle].dataLen;
        k_mutex_unlock(&spi_reply_mutex);
        return 0;
    }
    k_mutex_unlock(&spi_reply_mutex);

    if (timeout > 0) {
        uint64_t startTime = k_uptime_get();
        while ((k_uptime_get() - startTime) < timeout) {
            // printf("\n Try to get data \n");
            //char no_op_data[2];

            // send empty spi tx
            //modem_spi_send_command(MESSAGE_TYPE_NO_OP, NULL, 0, false);

            // check response
            k_mutex_lock(&spi_reply_mutex, K_FOREVER);
            if (modem_handles[handle].data != NULL) {
                if (maxlen < modem_handles[handle].dataLen) {
                    LOG_ERR("data buffer %d too small for %d", maxlen, modem_handles[handle].dataLen);
                    k_mutex_unlock(&spi_reply_mutex);
                    return -ENOMEM;
                }
                memcpy(data, modem_handles[handle].data, modem_handles[handle].dataLen);
                *dataLen = modem_handles[handle].dataLen;
                k_mutex_unlock(&spi_reply_mutex);
                return 0;
            }
            k_mutex_unlock(&spi_reply_mutex);

            // pause briefly?
            k_sleep(K_MSEC(1));
        }
    }
    // return -1 on invalid handle
    return -1;
}


// Example "get one value from the 9160" function
static int modem_spi_get_one_value(char *cmd, uint8_t *buf, uint16_t len)
{
    if (!modem_is_powered_on()) {
        LOG_ERR("Modem is not powered on");
        return -ENOTCONN;
    }
    int ret = 0;

    // modem_send_command, takes cmd type, in this case AT, a cmd buffer, and a len.
    // it also takes a bool that says whether we care about the reply.
    // If you dont care, it doesn't setup the handle/reply system for this and just sends it.
    // Since you care in this case, it will return a handle that you can use to get the reply later.
    int my_handle = modem_send_command(MESSAGE_TYPE_AT, cmd, strlen(cmd), true);

    // now we ask for the response, if we didnt say true above, this would return -1
    // if you specify 0 as the timeout it will return immediately and you can call this in a loop perhaps.
    if (modem_recv_resp(my_handle, buf, &len, 10000) == 0) {    // 10 sec timeout, thats a looooong time
        // Modem returned valid response
        ret = 0;
    } else {
        //Modem response timed out
        ret = -1;
    }
    buf[len] = '\0';

    // due to the nature of the buffers returned and being all kinds of different sizes, this return is k_malloc'd
    // so you MUST free it when you are done with it.
    modem_free_reply_data(my_handle);
    return ret;
}

int modem_spi_get_IMEI(uint8_t *buf, uint16_t len)
{
    // TODO: cache this to a file or something
    char *cmd = "AT+CGSN\0";
    return (modem_spi_get_one_value(cmd, buf, len));
}

int modem_spi_get_ICCID(uint8_t *buf, uint16_t len)
{
    // TODO: cache this to a file or something
    char *cmd = "AT\%XICCID\0";
    modem_send_command(MESSAGE_TYPE_AT, "AT+CFUN=1\0", strlen("AT+CFUN=1\0"), false);
    k_sleep(K_MSEC(1000));
    int ret = modem_spi_get_one_value(cmd, buf, len);
    k_sleep(K_MSEC(500));
    modem_send_command(MESSAGE_TYPE_AT, "AT+CFUN=0\0", strlen("AT+CFUN=0\0"), false);
    // k_sleep(K_MSEC(300));
    return ret;
}

int modem_spi_get_VERSION(uint8_t *buf, uint16_t len)
{
    char *cmd = "AT+CGMR\0";
    return (modem_spi_get_one_value(cmd, buf, len));
}

version_response_t *modem_spi_get_cached_version()
{
    return &shadow_version;
}
