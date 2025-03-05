/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#include "modem.h"
#include <zephyr/logging/log.h>

#if (CONFIG_USE_UART_TO_NRF9160)
#include "modem_uart.h"
#else
#include "modem_spi.h"
#endif
#include <cJSON.h>
#include <zephyr/zbus/zbus.h>
#include "d1_zbus.h"
#include <zephyr/kernel.h>
#include "pmic.h"
#include "modem_interface_types.h"
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/crc.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/sys/crc.h>
#include <zephyr/settings/settings.h>

#include "utils.h"

LOG_MODULE_REGISTER(modem, LOG_LEVEL_DBG);

extern modem_status_t modem_status_shadow;
bool                  nrf9160_is_powered_on = false;

#define IMEI_SETTINGS_PATH  "nrf9160/imei"
#define ICCID_SETTINGS_PATH "nrf9160/iccid"
#define IMEI_SETTINGS_SIZE  15
#define ICCID_SETTINGS_SIZE 20

int modem_init()
{
    modem_power_on();
    return modem_spi_init();
}

bool modem_is_powered_on()
{
    PMIC_LTE_POWER_STATE_t state = pmic_is_9160_powered();
    if (state == PMIC_LTE_POWER_ON) {
        return true;
    } else {
        return false;
    }
}

int modem_recv(uint8_t *buf, k_timeout_t timeout)
{
    return modem_spi_recv(buf, timeout);
}

void modem_set_rx_cb(void (*cb)(uint8_t *data, size_t len, void *user_data), void *user_data)
{
    modem_spi_set_rx_cb(cb, user_data);
}

int modem_send_command(modem_message_type_t type, uint8_t *data, uint16_t dataLen, bool reply_requested)
{
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }
    return modem_spi_send_command(type, data, dataLen, reply_requested);
}

int modem_recv_resp(uint8_t handle, uint8_t *data, uint16_t *dataLen, int timeout)
{
    return modem_spi_recv_resp(handle, data, dataLen, timeout);
}

int modem_free_reply_data(int handle)
{
    return modem_spi_free_reply_data(handle);
}

int modem_stop_waiting_for_resp(int handle)
{
    return modem_spi_stop_waiting_for_resp(handle);
}

int modem_get_VERSION(uint8_t *buf, uint16_t len)
{
    return modem_spi_get_VERSION(buf, len);
}

// This assumes the buffer passed is going to be big enough!
int modem_get_IMEI(uint8_t *buf, uint16_t len)
{
    int ret;
    if ((ret = utils_load_setting(IMEI_SETTINGS_PATH, buf, IMEI_SETTINGS_SIZE)) != 0) {
        if ((ret = modem_spi_get_IMEI(buf, len)) == 0) {
            ret = settings_save_one(IMEI_SETTINGS_PATH, buf, IMEI_SETTINGS_SIZE);
        }
    }
    return ret;
}

int modem_get_ICCID(uint8_t *buf, uint16_t len)
{
    int ret = 0;
    if ((ret = utils_load_setting(ICCID_SETTINGS_PATH, buf, ICCID_SETTINGS_SIZE)) != 0
        || strlen(buf) != ICCID_SETTINGS_SIZE) {
        LOG_WRN("Fetch and store ICCID from 9160 (len %d)", strlen(buf));
        if ((ret = modem_spi_get_ICCID(buf, len)) == 0) {
            ret = settings_save_one(ICCID_SETTINGS_PATH, &buf[9], ICCID_SETTINGS_SIZE);
            memmove(buf, &buf[9], ICCID_SETTINGS_SIZE);
            buf[ICCID_SETTINGS_SIZE] = 0;
        }
    }
    return ret;
}

////////////////////////////////////////////////////
// modem_send_mqtt()
// Send a message to the modem to send a message to the MQTT broker
//
// Arguments:
//  topic: the topic to send the message to
//  topic_length: the length of the topic
//  message: the message to send
//  message_length: the length of the message
//  qos: the quality of service to use
//  timeout_in_ms: the timeout in milliseconds to wait for a response
//
// Returns:
//  0: success
//  -ENOTCONN: if mqtt is not connected
//  -ENODEV: if the modem is not powered on
//  -ENOMEM: if memory allocation fails
//  -ETIMEDOUT: if a response is requested and the response times out
//  -EINVAL: if the topic or message length is less than 1
int modem_send_mqtt(
    char *topic, uint16_t topic_length, char *message, uint16_t message_length, uint8_t qos, uint16_t timeout_in_ms)
{
    int ret = 0;
    if (topic_length < 1 || message_length < 1) {
        return -EINVAL;
    }

    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }

    if (!(modem_status_shadow.status_flags & (1 << STATUS_MQTT_CONNECTED))) {
        LOG_ERR("9160 not connected to MQTT, not sending command");
        return -ENOTCONN;
    }

    uint8_t *data = k_malloc(sizeof(spi_mqtt_t) + topic_length + message_length);
    if (data == NULL) {
        LOG_ERR("Failed to allocate memory for mqtt message");
        return -ENOMEM;
    }
    memset(data, 0, sizeof(spi_mqtt_t) + topic_length + message_length);

    spi_mqtt_t *mqtt_msg   = (spi_mqtt_t *)data;
    mqtt_msg->qos          = qos;
    mqtt_msg->topic_length = topic_length;
    mqtt_msg->msg_length   = message_length;
    memcpy(data + sizeof(spi_mqtt_t), topic, mqtt_msg->topic_length);
    memcpy(data + sizeof(spi_mqtt_t) + mqtt_msg->topic_length, message, mqtt_msg->msg_length);

    // LOG_DBG("Sending to MQTT modem: topic_len=%d  payload_len=%d", mqtt_msg->topic_length,
    // mqtt_msg->msg_length);
    bool reply_requested = false;
    if (timeout_in_ms != 0) {
        reply_requested = true;
    }

    int my_handle = modem_send_command(
        MESSAGE_TYPE_MQTT, data, sizeof(spi_mqtt_t) + mqtt_msg->topic_length + mqtt_msg->msg_length, reply_requested);
    if (my_handle < 0) {
        LOG_WRN("Failed to send command");
        k_free(data);
        return -ENOTCONN;
    }
    if (reply_requested) {
        uint8_t  buf[32];
        uint16_t len = 32;
        if (modem_recv_resp(my_handle, buf, &len, timeout_in_ms) == 0) {
            // LOG_HEXDUMP_ERR(buf, len, "Received from modem");
            int8_t *incoming_status = (int8_t *)(&buf[0]);
            modem_free_reply_data(my_handle);
            ret = *incoming_status;
        } else {
            modem_stop_waiting_for_resp(my_handle);
            ret = -ETIMEDOUT;
        }
    } else {
        ret = my_handle;
    }
    k_free(data);
    return ret;
}

int modem_get_status(modem_status_t *status)
{
    if (status == NULL) {
        return -1;
    }
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        copy_modem_status(&modem_status_shadow, status);    // return the cached status for anyone that cares
        return -ENODEV;
    }
    int my_handle = modem_send_command(MESSAGE_TYPE_DEVICE_STATUS, NULL, 0, true);
    if (my_handle < 0) {
        LOG_WRN("Failed to send command");
        // get cached stats
        copy_modem_status(&modem_status_shadow, status);    // return the cached status for anyone that cares
        return my_handle;
    }
    uint8_t  buf[1024];
    uint16_t len = 1024;
    if (modem_recv_resp(my_handle, buf, &len, 3000) == 0) {
        modem_status_t *incoming_status = (modem_status_t *)(&buf[0]);
        status->status_flags            = incoming_status->status_flags;
        memcpy(status->timestamp, incoming_status->timestamp, 32);
        status->rssi            = incoming_status->rssi;
        status->fota_state      = incoming_status->fota_state;
        status->fota_percentage = incoming_status->fota_percentage;
        status->uptime          = incoming_status->uptime;

        return modem_free_reply_data(my_handle);
    } else {
        modem_stop_waiting_for_resp(my_handle);
        return -1;
    }
}

int modem_get_info(modem_info_t *info)
{
    if (info == NULL) {
        return -1;
    }
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }
    int my_handle = modem_send_command(MESSAGE_TYPE_DEVICE_INFO, NULL, 0, true);
    if (my_handle < 0) {
        LOG_WRN("Failed to send command: try cached IMEI and ICCID");
        int ret;

        if ((ret = modem_get_IMEI(info->imei, sizeof(info->imei))) == 0) {
            char iccid[50];
            ret       = modem_get_ICCID(iccid, sizeof(iccid));
            iccid[19] = 0;
            strcpy(info->iccid, iccid);
            sprintf(info->operator, "N/C");
            sprintf(info->cellID, "N/C");
            sprintf(info->ap, "N/C");
            sprintf(info->subscriber, "N/C");
        }

        return ret;
    }
    uint8_t  buf[1024];
    uint16_t len = 1024;
    if (modem_recv_resp(my_handle, buf, &len, 1000) == 0) {
        modem_info_t *incoming_info = (modem_info_t *)(&buf[0]);
        memcpy(info->imei, incoming_info->imei, 16);
        memcpy(info->iccid, incoming_info->iccid, 32);
        memcpy(info->operator, incoming_info->operator, 16);
        memcpy(info->cellID, incoming_info->cellID, 16);
        memcpy(info->ap, incoming_info->ap, 16);
        memcpy(info->subscriber, incoming_info->subscriber, 32);
        return modem_free_reply_data(my_handle);
    } else {
        modem_stop_waiting_for_resp(my_handle);
        return -1;
    }
}

int modem_get_version(version_response_t *ver)
{
    bool reply_requested = true;
    if (ver == NULL) {
        reply_requested = false;
    }

    if (ver) {
        version_response_t *tempVer = get_cached_version_info();
        if (tempVer->major > 0) {
            // seems valid, lets just use this.
            ver->major = tempVer->major;
            ver->minor = tempVer->minor;
            ver->patch = tempVer->patch;
            memcpy(ver->githash, tempVer->githash, 16);
            memcpy(ver->build_date, tempVer->build_date, 32);
            memcpy(ver->build_machine, tempVer->build_machine, 16);
            memcpy(ver->modem_fw, tempVer->modem_fw, 32);
            return 0;
        }
    }

    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }
    uint8_t cmd       = COMMAND_GET_VERSION;
    int     my_handle = modem_send_command(MESSAGE_TYPE_COMMAND, &cmd, 1, reply_requested);
    if (my_handle < 0) {
        LOG_WRN("Failed to send command");
        return -1;
    }
    if (!reply_requested) {
        return 0;
    }
    // LOG_DBG("Sent get version command");
    uint8_t  buf[1024];
    uint16_t len = 1024;
    if (modem_recv_resp(my_handle, buf, &len, 1000) == 0) {
        buf[len] = '\0';
        if (buf[0] == COMMAND_GET_VERSION) {
            version_response_t *version = (version_response_t *)&buf[1];
            ver->major                  = version->major;
            ver->minor                  = version->minor;
            ver->patch                  = version->patch;
            memcpy(ver->githash, version->githash, 16);
            memcpy(ver->build_date, version->build_date, 32);
            memcpy(ver->build_machine, version->build_machine, 16);
            memcpy(ver->modem_fw, version->modem_fw, 32);
        }
        return modem_free_reply_data(my_handle);
    } else {
        modem_stop_waiting_for_resp(my_handle);
        return -1;
    }
}

int modem_get_time(char *buf, uint16_t len)
{
    // uint8_t buf[1024];
    // uint16_t len = 1024;
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }
    int my_handle = modem_send_command(MESSAGE_TYPE_GET_TIME, NULL, 0, true);
    if (my_handle < 0) {
        LOG_WRN("Failed to send command");
        return -1;
    }
    if (modem_recv_resp(my_handle, buf, &len, 1000) == 0) {
        return 0;
    } else {
        return -1;
    }
}

int modem_cancel_current_download()
{
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }
    uint8_t cmd       = 0;
    int     my_handle = modem_send_command(MESSAGE_TYPE_DOWNLOAD_FROM_HTTPS, &cmd, 1, false);
    if (my_handle < 0) {
        LOG_WRN("Failed to send command");
        return -1;
    }
    return 0;
}

int modem_download_file_from_url(char *url, char *new_file_name)
{
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }
    if (strlen(url) < 10) {
        return -EINVAL;
    }
    if (strlen(new_file_name) < 1) {
        return -EINVAL;
    }
    static uint32_t   crc32            = 0;
    bool              download_started = false;
    char              new_file_name_with_path[128];
    struct fs_file_t  file;
    struct fs_statvfs vfsbuf;

    int my_handle = modem_send_command(MESSAGE_TYPE_DOWNLOAD_FROM_HTTPS, url, strlen(url), true);
    if (my_handle < 0) {
        LOG_WRN("Failed to send command");
        return my_handle;
    }

    // open file for storing download

    while (1) {
        uint8_t  buf[CONFIG_SPI_RECEIVE_BUFFER_MAX_SIZE];
        uint16_t len = CONFIG_SPI_RECEIVE_BUFFER_MAX_SIZE;
        int      ret = modem_recv_resp(my_handle, buf, &len, 10000);
        if (ret == 0) {
            download_response_t *resp = (download_response_t *)(&buf[0]);
            // LOG_DBG("Received download response, status: %d", resp->status);
            switch (resp->status) {
            case DOWNLOAD_COMPLETE:
                // download is complete
                // close the file
                LOG_INF("Download complete, local CRC32: 0x%08x    9160 crc: 0x%08x", crc32, resp->crc);
                modem_spi_stop_waiting_for_resp(my_handle);
                fs_close(&file);
                crc32 = 0;
                return 0;
            case DOWNLOAD_ERROR:
                // download error
                LOG_ERR("Unknown download error");
                modem_spi_stop_waiting_for_resp(my_handle);
                fs_close(&file);
                crc32 = 0;
                return -EINVAL;
            case DOWNLOAD_INPROGRESS:
                if (!download_started) {
                    // download has started
                    // open the file
                    download_started = true;
                    LOG_INF("Download started, total size: %lld bytes", resp->total_size);

                    ret = fs_statvfs("/lfs1", &vfsbuf);
                    if (ret != 0) {
                        LOG_ERR("Error getting fs stats");
                        modem_spi_stop_waiting_for_resp(my_handle);
                        return ret;
                    }
                    // if ((vfsbuf.f_bsize * vfsbuf.f_bfree) < resp->total_size) {
                    //     LOG_ERR("File system is full! need %lld bytes, have %ld bytes", resp->total_size, vfsbuf.f_bsize * vfsbuf.f_bfree);
                    //     modem_spi_stop_waiting_for_resp(my_handle);
                    //     return -ENOSPC;
                    // }

                    snprintf(new_file_name_with_path, 128, "/lfs1/%s", new_file_name);
                    fs_file_t_init(&file);
                    ret = fs_open(&file, new_file_name_with_path, FS_O_CREATE | FS_O_WRITE);
                    if (ret != 0) {
                        LOG_ERR("Error opening file: %d", ret);
                        modem_spi_stop_waiting_for_resp(my_handle);
                        return -EIO;
                    }
                }
                // download is still in progress
                // update the progress
                // add to the file
                uint8_t *filedata = (uint8_t *)resp + sizeof(download_response_t);
                crc32             = crc32_ieee_update(crc32, filedata, resp->current_payload_size);
                if (fs_write(&file, filedata, resp->current_payload_size) != resp->current_payload_size) {
                    LOG_ERR("Error writing to file");
                    fs_close(&file);
                    modem_spi_stop_waiting_for_resp(my_handle);
                    return -EIO;
                }

                if (resp->total_size == resp->progress_bytes) {
                    LOG_INF("Download complete, local CRC32: 0x%08x    9160 crc: 0x%08x", crc32, resp->crc);
                    modem_spi_stop_waiting_for_resp(my_handle);
                    fs_close(&file);
                    crc32 = 0;
                    return 0;
                } else {
                    LOG_INF("Download progress: %d%%, %lld bytes", resp->progress_percent, resp->progress_bytes);
                }

                break;
            default:
                LOG_ERR("Download error: %d", resp->status);
                modem_spi_stop_waiting_for_resp(my_handle);
                break;
            }
            modem_spi_free_reply_data_but_not_handle(my_handle);
        } else {
            return ret;
        }
    }
}

int modem_fota_from_https(char *url, uint16_t url_length)
{
    if (url_length < 1) {
        return -1;
    }
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }
    int my_handle = modem_send_command(MESSAGE_TYPE_FOTA_FROM_HTTPS, url, url_length, false);
    if (my_handle < 0) {
        LOG_WRN("Failed to send command");
        return -1;
    }
    return 0;
}

void call_pmic_modem_powerdown(struct k_work *work)
{
    set_switch_state(PMIC_SWITCH_VSYS, false);
}
K_WORK_DEFINE(modem_powerdown_work, call_pmic_modem_powerdown);
void modem_powerdown_work_timer_handler(struct k_timer *dummy)
{

    k_work_submit(&modem_powerdown_work);
}
K_TIMER_DEFINE(modem_powerdown_timer, modem_powerdown_work_timer_handler, NULL);

int modem_power_on()
{
    set_modem_power_state(true);
    return 0;
}

int modem_power_off()
{
    extern bool g_bt_connected;
    extern bool g_usb_connected;
    if (g_bt_connected || g_usb_connected) {
        LOG_ERR("NOT TURNING OFF THE 9160 WHEN ON POWER");
        return 0;
    }
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is already powered off");
        return -ENODEV;
    }
    // if (modem_spi_get_cached_version()->major >= 1 || ( modem_spi_get_cached_version()->major
    // == 1 && modem_spi_get_cached_version()->minor > 0)) {
    LOG_DBG("Powering off 9160");
    uint8_t cmd       = COMMAND_HARD_STOP;
    int     my_handle = modem_send_command(MESSAGE_TYPE_COMMAND, &cmd, 1, true);
    if (my_handle < 0) {
        LOG_WRN("Failed to send command");
        return -1;
    }
    uint8_t  response[64];
    uint16_t response_length = sizeof(response);
    if (modem_recv_resp(my_handle, response, &response_length, 3000) == 0) {
        if (response_length == 1) {
            if (response[0] == 0) {
                LOG_DBG("Received response from modem, Its powering off now");
                set_modem_power_state(false);
                return 0;
            } else {
                LOG_ERR("Failed to power off modem - busy");
                modem_stop_waiting_for_resp(my_handle);
                return -EAGAIN;
            }
        } else {
            // old 9160 (longer response, with no feedback)
            set_modem_power_state(false);
        }

        return 0;
    } else {
        LOG_ERR("Failed to power off modem - busy");
        modem_stop_waiting_for_resp(my_handle);
        return -EAGAIN;
    }
    return 0;
}

int modem_send_at_command(char *cmd, uint16_t cmd_length, char *response, uint16_t *response_length)
{
    if (cmd_length < 1) {
        return -1;
    }
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }
    // send to spi
    int my_handle = modem_send_command(MESSAGE_TYPE_AT, cmd, cmd_length, true);
    if (my_handle < 0) {
        LOG_WRN("Failed to send command");
        return -1;
    }
    if (modem_recv_resp(my_handle, response, response_length, 3000) == 0) {
        response[*response_length] = '\0';
        return modem_free_reply_data(my_handle);
    } else {
        modem_stop_waiting_for_resp(my_handle);
        return -1;
    }
}

int modem_set_mqtt_params(char *host, uint16_t host_length, uint16_t port, char *client_id, uint16_t client_id_length)
{
    if (host_length < 1 || client_id_length < 1) {
        LOG_ERR("Invalid host or client_id");
        return -1;
    }
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }
    uint8_t data[1024];
    data[0]                = COMMAND_SET_MQTT_PARAMS;
    char mqtt_params_str[] = "{\"host\": \"%s\",\"port\": %d,\"client_id\": \"%s\"}\0";
    int  len               = snprintk(data + 1, 1023, mqtt_params_str, host, port, client_id);
    if ((len < 0) || (len >= 1023)) {
        LOG_ERR("Failed to construct message, error: %d", len);
        return -1;
    }

    LOG_DBG("Sending MQTT Server info to LTE");
    return modem_send_command(MESSAGE_TYPE_COMMAND, data, len + 1, false);
}

int modem_set_mqtt_subscriptions(char *topics[], int qos[], uint16_t topics_length)
{
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }
    if (topics_length < 1) {
        return -1;
    }
    int  ret;
    char data[1024] = { 0 };
    data[0]         = COMMAND_SET_MQTT_SUBSCRIPTIONS;
    strcat(data + 1, "{\"topics\": [");
    // char subsc_str[128] = "";
    // char mqtt_params_str[] = "\"%s\",";
    // char* tempptr = data+1;

    cJSON *topic;
    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_CreateArray();

    cJSON_AddItemToObject(root, "topics", list);

    for (int i = 0; i < topics_length; i++) {
        cJSON_AddItemToArray(list, topic = cJSON_CreateObject());
        cJSON_AddItemToObject(topic, "topic", cJSON_CreateString(topics[i]));
        cJSON_AddItemToObject(topic, "qos", cJSON_CreateNumber(qos[i]));
    }
    char *out = cJSON_PrintUnformatted(root);
    strcpy(data + 1, out);
    cJSON_free(out);

    log_panic();
    // LOG_DBG("Sending to modem: %s", data+1);
    LOG_DBG("Sending MQTT Subscriptions to LTE");
    ret = modem_send_command(MESSAGE_TYPE_COMMAND, data, strlen(data + 1) + 1, false);
    if (ret < 0) {
        LOG_WRN("Failed to send command");
    }
    cJSON_Delete(root);
    return ret;
}

int modem_start_mqtt()
{
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }
    LOG_INF("Starting MQTT on LTE");
    uint8_t data[2];
    data[0] = COMMAND_SET_MQTT_CONNECT;
    data[1] = 1;
    return modem_send_command(MESSAGE_TYPE_COMMAND, data, 2, false);
}

int modem_stop_mqtt()
{
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }
    LOG_DBG("Stopping MQTT");
    uint8_t data[2];
    data[0] = COMMAND_SET_MQTT_DISCONNECT;
    data[1] = 0;
    return modem_send_command(MESSAGE_TYPE_COMMAND, data, 2, false);
}

int modem_send_reboot()
{
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }
    LOG_DBG("Rebooting 9160");
    uint8_t data[2];
    data[0] = COMMAND_REBOOT;
    data[1] = 1;
    int ret = modem_send_command(MESSAGE_TYPE_COMMAND, data, 2, false);
    return ret;
}

int modem_send_gps_fakedata_enable()
{
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }
    LOG_DBG("Enabling GPS Fake Data");
    uint8_t data[3];
    data[0] = COMMAND_GPS_START;
    data[1] = 2;
    data[2] = 1;
    int ret = modem_send_command(MESSAGE_TYPE_COMMAND, data, 3, false);
    if (ret < 0) {
        LOG_WRN("Failed to send command");
        return -1;
    }
    return 0;
}

int modem_send_gps_fakedata_disable()
{
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }
    LOG_DBG("Disabling GPS Fake Data");
    uint8_t data[3];
    data[0] = COMMAND_GPS_START;
    data[1] = 3;
    data[2] = 0;
    int ret = modem_send_command(MESSAGE_TYPE_COMMAND, data, 3, false);
    if (ret < 0) {
        LOG_WRN("Failed to send command");
        return -1;
    }
    return 0;
}

int modem_send_gps_enable()
{
    // CONFIG_GPS_DATA_PERIOD
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }
    LOG_DBG("Enabling GPS");
    uint8_t data[3];
    data[0] = COMMAND_GPS_START;
    data[1] = 1;
    data[2] = CONFIG_GPS_DATA_PERIOD;
    int ret = modem_send_command(MESSAGE_TYPE_COMMAND, data, 3, false);
    if (ret < 0) {
        LOG_WRN("Failed to send command");
        return -1;
    }
    return 0;
}

int modem_send_gps_disable()
{
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }
    LOG_DBG("Disabling GPS");
    uint8_t data[3];
    data[0] = COMMAND_GPS_START;
    data[1] = 0;
    data[2] = 0;
    int ret = modem_send_command(MESSAGE_TYPE_COMMAND, data, 3, false);
    if (ret < 0) {
        LOG_WRN("Failed to send command");
        return -1;
    }
    return 0;
}

int modem_set_airplane_mode(bool on)
{
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }
    uint8_t data[2];
    data[0] = COMMAND_SET_AIRPLANE_MODE;
    data[1] = on;
    LOG_DBG("Sending to modem: %d", data[1]);
    int ret = modem_send_command(MESSAGE_TYPE_COMMAND, data, 2, false);
    if (ret < 0) {
        LOG_WRN("Failed to send command");
        return -1;
    }
    return 0;
}

version_response_t *get_cached_version_info()
{
    return modem_spi_get_cached_version();
}

int modem_get_rssi()
{
    return modem_status_shadow.rssi;
}

int modem_enable()
{
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }
    uint8_t cmd = COMMAND_START;
    return modem_send_command(MESSAGE_TYPE_COMMAND, &cmd, 1, false);
}

void set_modem_power_state(bool on)
{
    nrf9160_is_powered_on = on;
    if (!on) {
        pmic_set_modem_low_power_state();
        // update the status and publish so everyone knows whats up
        LOG_DBG("Modem powering off - updating status");
        modem_status_t new_status;
        new_status.status_flags    = 0;
        new_status.rssi            = 0;
        new_status.fota_state      = 0;
        new_status.fota_percentage = 0;

        // also zero out the current_cell_info, so no one gets confused.
        clear_cell_info();

        modem_status_update_t modem_status_update;
        modem_status_update.status      = new_status;
        modem_status_update.change_bits = merge_shadow_status(&new_status);
        int err                         = zbus_chan_pub(&LTE_STATUS_UPDATE, &modem_status_update, K_MSEC(1000));
        if (err) {
            LOG_ERR("zbus_chan_pub, error: %d", err);
            return;
        }
    } else {
        pmic_power_on_modem();
        // update the status and publish so everyone knows whats up
        LOG_DBG("Modem powered on - waiting for 9160 status");
        // modem_status_t new_status;
        // new_status.status_flags |= (1 << STATUS_POWERED_ON);

        // modem_status_update_t modem_status_update;
        // modem_status_update.status = new_status;
        // modem_status_update.change_bits = merge_shadow_status(&new_status);
        // int err = zbus_chan_pub(&LTE_STATUS_UPDATE, &modem_status_update, K_MSEC(1000));
        // if (err) {
        //     LOG_ERR("zbus_chan_pub, error: %d", err);
        //     return;
        // }
    }
}

cell_info_t modem_getCellInfo()
{
    return modem_spi_getCellInfo();
}

int modem_send_modemCellInfo(char *response, uint16_t *response_length)
{
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }
    // send to spi
    uint8_t data      = 1;
    int     my_handle = modem_send_command(MESSAGE_TYPE_CELL_INFO, &data, 1, true);
    if (my_handle < 0) {
        LOG_WRN("Failed to send command");
        return -1;
    }
    if (modem_recv_resp(my_handle, response, response_length, 3000) == 0) {
        response[*response_length] = '\0';
        return modem_free_reply_data(my_handle);
    } else {
        return -1;
    }
}

int modem_upload_fw(char *fw_file)
{
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }
    // open file
    struct fs_file_t file;
    fs_file_t_init(&file);
    int ret = fs_open(&file, fw_file, FS_O_READ);
    if (ret != 0) {
        LOG_ERR("Error opening file: (%d) %s", ret, fw_file);
        return -1;
    }

    // // get file size
    // struct fs_dirent entry;
    // ret = fs_stat(fw_file, &entry);
    // if (ret != 0) {
    //     LOG_ERR("Error getting file size: %d", ret);
    //     fs_close(&file);
    //     return -1;
    // }
    // entry.size;

    int rc = fs_seek(&file, 0L, FS_SEEK_END);
    if (rc < 0) {
        LOG_ERR("FAIL: seek %s: %d", fw_file, rc);
        return rc;
    }
    uint32_t file_size = fs_tell(&file);
    LOG_DBG("File size: %d", file_size);

    rc = fs_seek(&file, 0, FS_SEEK_SET);
    if (rc < 0) {
        LOG_ERR("FAIL: seek %s: %d", fw_file, rc);
        return rc;
    }

    // read the file in chunks and send to modem in CONFIG_LTE_FOTA_CHUNK_SIZE_MAX chunks
    // wait for the response from the modem before sending the next chunk
    uint8_t  response[sizeof(firmware_upload_t)];
    uint16_t response_length = sizeof(response);

    // loop while reading the file in chunks
    int i = 0;
    for (;;) {
        static uint8_t block[CONFIG_LTE_FOTA_CHUNK_SIZE_MAX];
        rc = fs_read(&file, &block, CONFIG_LTE_FOTA_CHUNK_SIZE_MAX);
        if (rc >= 0) {
            // LOG_DBG("read %s: [rd:%d]", fw_file, rc);
            uint8_t *buf = k_malloc(CONFIG_LTE_FOTA_CHUNK_SIZE_MAX + sizeof(firmware_upload_t));
            if (buf == NULL) {
                LOG_ERR("Failed to allocate memory for file buffer");
                fs_close(&file);
                return -1;
            }
            uint32_t          crc32 = 0;
            firmware_upload_t data;
            data.chunk_total = (file_size / CONFIG_LTE_FOTA_CHUNK_SIZE_MAX);
            data.chunk_num   = i;
            data.data_len    = rc;
            data.crc         = crc32_ieee_update(crc32, block, rc);
            data.return_code = 0;

            memcpy(buf, &data, sizeof(firmware_upload_t));
            memcpy(buf + sizeof(firmware_upload_t), block, rc);

            LOG_DBG(
                "chunk_total: %d, chunk_num: %d, data_len: %d, crc: 0x%08x",
                data.chunk_total,
                data.chunk_num,
                data.data_len,
                data.crc);

            int my_handle = modem_send_command(
                MESSAGE_TYPE_FW_UPLOAD, (uint8_t *)buf, sizeof(firmware_upload_t) + data.data_len, true);
            if (my_handle < 0) {
                LOG_WRN("Failed to send command");
                return -1;
            }
            k_free(buf);
            if (modem_recv_resp(my_handle, response, &response_length, 10000) == 0) {
                firmware_upload_t *resp = (firmware_upload_t *)(&response[0]);
                if (resp->return_code != 0) {
                    LOG_ERR("Error uploading firmware: %d", resp->return_code);
                    modem_free_reply_data(my_handle);
                    fs_close(&file);
                    return -1;
                }
                // LOG_DBG("got reply: (%d): %d/%d", resp->return_code,
                // resp->chunk_num, resp->chunk_total);
                modem_free_reply_data(my_handle);
            } else {
                LOG_ERR("Error uploading firmware: no response");
                return -1;
            }
            i++;
            // LOG_DBG("sent block: %d", i);
            if (rc < CONFIG_LTE_FOTA_CHUNK_SIZE_MAX) {
                LOG_DBG("sent last block!");
                break;
            }
        } else {
            LOG_ERR("FAIL: read %s: [rd:%d]", fw_file, rc);
            break;
        }
    }

    rc = fs_close(&file);

    if (rc < 0) {
        LOG_ERR("FAIL: close %s: %d", fw_file, rc);
        return rc;
    }

    return 0;
}

int modem_set_dnsaddr(char *dns)
{
    if (!modem_is_powered_on()) {
        LOG_ERR("9160 is powered off, not sending command");
        return -ENODEV;
    }

    if (strlen(dns) < 7) {
        LOG_ERR("Invalid DNS address");
        return -EINVAL;
    }

    LOG_DBG("Setting DNS address to: (%d) %s", strlen(dns), dns);

    uint8_t data[256] = { 0 };
    data[0]           = COMMAND_SET_DNS;
    strcat(data + 1, dns);

    LOG_DBG("Sending DNS address to LTE");
    int my_handle = modem_send_command(MESSAGE_TYPE_COMMAND, data, strlen(dns) + 1, true);
    if (my_handle < 0) {
        LOG_WRN("Failed to send command");
        return -1;
    }
    uint16_t reply_size = 256;
    if (modem_recv_resp(my_handle, data, &reply_size, 3000) == 0) {
        modem_free_reply_data(my_handle);
        if (reply_size > 0) {
            return data[0];
        }
        return -1;
    } else {
        modem_stop_waiting_for_resp(my_handle);
        return -1;
    }
}