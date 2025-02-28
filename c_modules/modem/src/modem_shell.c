/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#include "modem.h"

#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <string.h>
#include "d1_json.h"
#include "fota.h"
#include "wifi.h"
#include "uicr.h"
#include <cJSON_os.h>
#include "commMgr.h"

#include "pmic.h"
#include "wifi_at.h"
#include "fota.h"

#define CHAR_1       0x18
#define CHAR_2       0x11
#define SEND_BUF_LEN 5120
char modem_send_buf[SEND_BUF_LEN];
int  modem_send_buf_length = 0;


void modem_rx_callback(uint8_t *data, size_t len, void *user_data)
{
    //struct shell *sh = (struct shell *)user_data;
    for (int i = 0; i < len; i++) {
        printk("%c", data[i]);
    }
}


int modem_set_bypass(const struct shell *sh, shell_bypass_cb_t bypass)
{
    static bool in_use;

    if (bypass && in_use) {
        shell_error(sh, "I have no idea how you got here.");
        return -EBUSY;
    }

    in_use = !in_use;
    if (in_use) {
        shell_print(sh, "Bypass started, press ctrl-x ctrl-q to escape");
        in_use = true;
    }

    shell_set_bypass(sh, bypass);

    return 0;
}


void modem_shell_bypass_cb(const struct shell *sh, uint8_t *data, size_t len)
{
    //uint8_t rbuf[SEND_BUF_LEN];
    if (modem_send_buf_length == 0) {
        memset(modem_send_buf, 0, SEND_BUF_LEN);
    }

    bool           modem_string_complete = false;
    static uint8_t tail;
    bool           escape = false;

    /* Check if escape criteria is met. */
    if (tail == CHAR_1 && data[0] == CHAR_2) {
        escape = true;
    } else {
        for (int i = 0; i < (len - 1); i++) {
            if (data[i] == CHAR_1 && data[i + 1] == CHAR_2) {
                escape = true;
                break;
            }
        }
    }

    if (escape) {
        shell_print(sh, "Exit bypass");
        modem_set_bypass(sh, NULL);
        tail = 0;
        return;
    }

    for (int i = 0; i < len; i++) {
        if (modem_send_buf_length < SEND_BUF_LEN) {
            if ((tail == '\r' && data[i] == '\n') || ((i > 0) && (data[i - 1] == '\r' && data[i] == '\n'))
                || data[i] == '\0') {
                modem_send_buf[modem_send_buf_length] = '\r';
                modem_send_buf_length++;
                modem_string_complete = true;
            } else {
                modem_send_buf[modem_send_buf_length] = data[i];
                modem_send_buf_length++;
            }
        }
    }
    /* Store last byte for escape sequence detection */
    tail = data[len - 1];

    if (modem_string_complete) {
        if (strlen(modem_send_buf) < 3) {
            // printf("This looks like an invalid command, not sending");
            modem_send_buf_length = 0;
            return;
        }
        // printf(">>>%d [%s]>>>",strlen(modem_send_buf),modem_send_buf);
        int      my_handle = modem_send_command(MESSAGE_TYPE_AT, modem_send_buf, modem_send_buf_length, true);
        uint8_t  buf[SEND_BUF_LEN];
        uint16_t len = sizeof(buf);
        k_sleep(K_MSEC(500));    //Wait a tiny bit so we have time for the data to go out and get acted on.
        if (modem_recv_resp(my_handle, buf, &len, 10000) == 0) {
            buf[len] = '\0';
            shell_print(sh, "%s\n", buf);
            modem_free_reply_data(my_handle);
        } else {
            shell_print(sh, "No response from modem\n");
            modem_stop_waiting_for_resp(my_handle);
        }
        modem_send_buf_length = 0;
    }
}


void do_ATPassthru_mode(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "------ AT Passthru mode ------\n");
    shell_print(sh, "reboot to get out of passthru\n");
    // press ctrl-x ctrl-q to escape
    shell_print(sh, "------------------------------\n");

    modem_set_bypass(sh, modem_shell_bypass_cb);
}

void do_modem_reset(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Resetting modem...\n");
    modem_power_off();
    k_msleep(6000);
    modem_power_on();
    k_msleep(1000);
    shell_print(sh, "Modem reset complete\n");
}

void do_modem_turn_off(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Turning off modem...\n");
    modem_power_off();
    shell_print(sh, "Modem turned off\n");
}

void do_modem_turn_on(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Turning on modem...\n");
    modem_power_on();
    shell_print(sh, "Modem turned on\n");
}


void do_modem_send_mqtt(const struct shell *sh, size_t argc, char **argv)
{
    if (argc <= 3) {
        shell_print(sh, "usage: send_json <topic> <message> <qos>\n");
        return;
    }

    int ret = modem_send_mqtt(argv[1], strlen(argv[1]), argv[2], strlen(argv[2]), atoi(argv[3]), 3000);
    shell_print(sh, "send_mqtt returned %d\n", ret);
}


void do_send_at(const struct shell *sh, size_t argc, char **argv)
{
    if (argc <= 1) {
        shell_print(sh, "usage: send_at <string_to_send> \n");
        return;
    }

    // send to spi
    //int my_handle = modem_send_command(MESSAGE_TYPE_AT, argv[1], strlen(argv[1]), true);
    shell_print(sh, "Sending AT command: %s\n", argv[1]);
    uint8_t  buf[1024];
    uint16_t len = 1024;

    if (modem_send_at_command(argv[1], strlen(argv[1]), buf, &len) == 0) {
        shell_print(sh, "Modem returned AT response(%d): %s\n", len, buf);
    } else {
        shell_print(sh, "Modem response timed out\n");
    }
}

void do_enable_modem(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Sending modem start command\n");
    modem_enable();
}

void do_gps_enable(const struct shell *sh, size_t argc, char **argv)
{
    if (argc <= 1) {
        shell_print(sh, "usage: gps_enable <0/1>\n");
        return;
    }

    if (strcmp(argv[1], "1") == 0) {
        shell_print(sh, "Starting rapid battery drain system, I mean, GPS.\n");
        modem_send_gps_enable();
    } else if (strcmp(argv[1], "0") == 0) {
        shell_print(sh, "Stopping GPS\n");
        modem_send_gps_disable();
    } else {
        shell_print(sh, "usage: gps_enable <0/1>\n");
    }
}


void do_gps_fake(const struct shell *sh, size_t argc, char **argv)
{
    if (argc <= 1) {
        shell_print(sh, "usage: gps_fake <0/1>\n");
        return;
    }

    if (strcmp(argv[1], "1") == 0) {
        shell_print(sh, "Starting GPS fake data\n");
        shell_print(sh, "Are you REALLY sure you know what you're doing?  This will send fake GPS data to the cloud\n");
        modem_send_gps_fakedata_enable();
    } else if (strcmp(argv[1], "0") == 0) {
        shell_print(sh, "Stopping GPS fake data\n");
        modem_send_gps_fakedata_disable();
    } else {
        shell_print(sh, "usage: gps_fake <0/1>  (This is probably a bad idea if you dont know what it is.)\n");
    }
}


void do_get_modem_version(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Getting modem version\n");

    version_response_t version;
    if (modem_get_version(&version) == 0) {
        shell_print(sh, "Modem version: %d.%d.%d (%s)", version.major, version.minor, version.patch, version.githash);
        shell_print(sh, "Modem Built on %s by %s", version.build_date, version.build_machine);
        shell_print(sh, "Modem FW ver: %s", version.modem_fw);
    } else {
        shell_print(sh, "Modem response timed out\n");
    }
}

bool do_simulate_fail;
void do_modem_simulate_fail(const struct shell *sh, size_t argc, char **argv)
{
    do_simulate_fail = true;
}

void do_get_modem_status(const struct shell *sh, size_t argc, char **argv)
{
    modem_status_t status;
    if (modem_get_status(&status) == 0) {
        shell_print(sh, "Modem status received\n");
    } else {
        shell_print(sh, "Modem response timed out, is it powered on?  Printing cached status\n");
    }
    bool is_on = modem_is_powered_on();

    shell_print(sh, "MODEM POWERED:                          %s\n", ((is_on)) ? "yes" : "NO");
    if (!is_on) {
        shell_print(sh, "CACHED STATUS!!!!\n");
    }
    shell_print(
        sh,
        "LTE Connected:                          %s",
        ((status.status_flags >> STATUS_LTE_CONNECTED) & 1) ? "yes" : "no");
    shell_print(
        sh, "LTE Enabled (factory test flag):        %s", ((status.status_flags >> STATUS_LTE_ENABLED) & 1) ? "yes" : "no");
    shell_print(
        sh, "LTE Working (connected at least once):  %s", ((status.status_flags >> STATUS_LTE_WORKING) & 1) ? "yes" : "no");
    shell_print(
        sh,
        "FOTA in progress:                       %s",
        ((status.status_flags >> STATUS_FOTA_IN_PROGRESS) & 1) ? "yes" : "no");
    shell_print(
        sh,
        "GPS Connected:                          %s",
        ((status.status_flags >> STATUS_GPS_CONNECTED) & 1) ? "yes" : "no");
    shell_print(
        sh, "GPS Enabled:                            %s", ((status.status_flags >> STATUS_GPS_ENABLED) & 1) ? "yes" : "no");
    shell_print(
        sh,
        "MQTT Connected:                         %s",
        ((status.status_flags >> STATUS_MQTT_CONNECTED) & 1) ? "yes" : "no");
    shell_print(
        sh,
        "MQTT Initialized:                       %s",
        ((status.status_flags >> STATUS_MQTT_INITIALIZED) & 1) ? "yes" : "no");
    shell_print(
        sh,
        "MQTT Enabled:                           %s",
        ((status.status_flags >> STATUS_MQTT_ENABLED) & 1) ? "yes" : "no");
    shell_print(
        sh,
        "MQTT Working (connected at least once): %s",
        ((status.status_flags >> STATUS_MQTT_WORKING) & 1) ? "yes" : "no");
    shell_print(
        sh, "Powered On:                             %s", ((status.status_flags >> STATUS_POWERED_ON) & 1) ? "yes" : "no");
    shell_print(
        sh,
        "Certs loaded:                           %s",
        ((status.status_flags >> STATUS_CERTS_LOADED) & 1) ? "yes" : "no");
    shell_print(
        sh,
        "Airplane mode:                          %s",
        ((status.status_flags >> STATUS_AIRPLANE_MODE) & 1) ? "yes" : "no");
    shell_print(sh, "status_flags:                           0x%x", status.status_flags);
    shell_print(sh, "timestamp:                              %s", status.timestamp);
    shell_print(sh, "rssi:                                   %d", status.rssi);
    shell_print(sh, "uptime:                                 %lld", status.uptime);
    shell_print(sh, "temp:                                   %d", status.temperature);

    if (!is_on) {
        // the cell info is invaid, so just exit
        return;
    }
    cell_info_t cell_info = modem_getCellInfo();

    shell_print(sh, "CellID:                                 %s", cell_info.cellID);
    //shell_print(sh, "RSSI: %d", cell_info.rssi);
    shell_print(sh, "Band:                                   %d", cell_info.lte_band);
    shell_print(sh, "MCC:                                    %d", cell_info.mcc);
    shell_print(sh, "MNC:                                    %d", cell_info.mnc);
    shell_print(sh, "Tracking Area (LAC):                    %d", cell_info.tracking_area);
    shell_print(
        sh,
        "IP:                                     %d.%d.%d.%d",
        cell_info.ip[0],
        cell_info.ip[1],
        cell_info.ip[2],
        cell_info.ip[3]);
}

void do_try_modem_fota(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Sending check-fota message\n");

    int  target    = COMM_DEVICE_NRF9160;
    bool do_update = false;
    if (argc > 1) {
        // get arg as bool
        if ((strcmp(argv[1], "1") == 0) || (strcmp(argv[1], "true") == 0)) {
            shell_print(sh, "Sending fota check with do_update flag\n");
            do_update = true;
        } else {
            shell_print(sh, "Sending fota check without do_update flag, this will just log whether there is an update\n");
            do_update = false;
        }
    }
    check_for_updates(do_update, target);
}

void do_url_fota(const struct shell *sh, size_t argc, char **argv)
{
    if (argc <= 2) {
        shell_print(sh, "usage: fota_url <url> <major>.<minor>.<patch>\n");
        shell_print(sh, "      for http urls, you MUST provide a port number, e.g. http://example.com:80/path/to/file\n");
        shell_print(sh, "      for https urls, port 443 is assumed unless you provide a different one.\n");
        return;
    }
    shell_print(sh, "Sending url-fota message\n");

    int major  = 0;
    int minor  = 0;
    int patch  = 0;
    int parsed = sscanf(argv[2], "%d.%d.%d", &major, &minor, &patch);
    if (parsed != 3) {
        shell_error(sh, "Expected version must be in the form A.B.C");
        return;
    }

    int ret = fota_start_9160_process(major, minor, patch);
    if (ret != 0) {
        shell_error(sh, "'%s'(%d) from fota_start_9160_process", wstrerr(-ret), ret);
        return;
    }
    modem_fota_from_https(argv[1], strlen(argv[1]));
}

void do_download_cancel(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Sending modem download cancel command\n");
    modem_cancel_current_download();
}

void do_url_download(const struct shell *sh, size_t argc, char **argv)
{
    if (argc <= 2) {
        shell_print(sh, "usage: download <url> <filename>\n");
        return;
    }
    shell_print(sh, "Downloading file %s\n", argv[1]);

    // download from url
    int ret = modem_download_file_from_url(argv[1], argv[2]);
    if (ret == 0) {
        shell_print(sh, "Download complete, file saved as %s\n", argv[2]);
    } else {
        shell_print(sh, "Download failed\n");
    }
}

#define BASE_MESSAGE_FOR_LARGE_TRANSFER_TEST_FORMAT_STRING \
    "{\"P\":2,\"MID\":\"%s\",\"MK\":\"US\",\"B\":35,\"T\":10,\"M\":{\"MSG\":\"%s\", \"MSG2\":\"%s\", \"MSG3\":\"%s\", \"MSG4\":\"%s\", \"MSG5\":\"%s\", \"S\":\"sField\"}}"
void do_send_large_mqtt(const struct shell *sh, size_t argc, char **argv)
{
    if (argc <= 1) {
        shell_print(sh, "usage: send_large_mqtt <size>\n");
        return;
    }
    int size = atoi(argv[1]);
    if (size <= 0) {
        shell_print(sh, "size must be > 0\n");
        return;
    }
    if (size > 5120) {
        shell_print(sh, "size must be <= 5000\n");
        return;
    }

    int   partial_msg_size = (size - strlen(BASE_MESSAGE_FOR_LARGE_TRANSFER_TEST_FORMAT_STRING)) / 5;
    char *partial_msg      = k_malloc(partial_msg_size);
    if (!partial_msg) {
        shell_print(sh, "allocation failed\n");
        return;
    }
    memset(partial_msg, 'a', partial_msg_size);
    partial_msg[partial_msg_size - 1] = '\0';

    char *machine_id = uicr_serial_number_get();
    char  thing_id[32];
    snprintf(thing_id, 32, "35_%s", machine_id);

    int   payload_size = size + strlen(BASE_MESSAGE_FOR_LARGE_TRANSFER_TEST_FORMAT_STRING);
    char *payload      = k_malloc(payload_size);
    if (!payload) {
        shell_print(sh, "allocation2 failed\n");
        return;
    }
    int len = snprintf(
        payload,
        payload_size,
        BASE_MESSAGE_FOR_LARGE_TRANSFER_TEST_FORMAT_STRING,
        machine_id,
        partial_msg,
        partial_msg,
        partial_msg,
        partial_msg,
        partial_msg);
    if ((len < 0) || (len >= payload_size)) {
        shell_print(sh, "Failed to construct message, error: %d", len);
        k_free(partial_msg);
        k_free(payload);
        return;
    }

    char topic[CONFIG_IOT_MAX_TOPIC_LENGTH];
    int  topicLen = snprintk(topic, CONFIG_IOT_MAX_TOPIC_LENGTH, "messages/35/10/%s/d2c", thing_id);
    int  ret      = modem_send_mqtt(topic, topicLen, payload, len, 0, 5000);
    shell_print(sh, "send_mqtt returned %d\n", ret);
    k_free(partial_msg);
    k_free(payload);
}


void do_send_get_info(const struct shell *sh, size_t argc, char **argv)
{
    modem_info_t info;
    if (modem_get_info(&info) == 0) {
        shell_print(sh, "Modem ICCID:         %s", info.iccid);
        shell_print(sh, "Modem IMEI:          %s", info.imei);
        shell_print(sh, "Modem Operator:      %s", info.operator);
        shell_print(sh, "Modem CellID:        %s", info.cellID);
        shell_print(sh, "Modem Access Point:  %s", info.ap);
        shell_print(sh, "Modem Subscriber:    %s", info.subscriber);
    } else {
        shell_print(sh, "Modem response timed out\n");
    }
}

void do_mqtt_client(const struct shell *sh, size_t argc, char **argv)
{

    if (argc <= 1) {
        shell_print(sh, "usage: mqtt_client <0/1>\n");
        return;
    }

    if (strcmp(argv[1], "1") == 0) {
        shell_print(sh, "Starting MQTT client\n");
        modem_start_mqtt();
    } else if (strcmp(argv[1], "0") == 0) {
        shell_print(sh, "Stopping MQTT client\n");
        modem_stop_mqtt();
    } else {
        shell_print(sh, "usage: mqtt_client <0/1>\n");
    }
}


void do_fw_upload(const struct shell *sh, size_t argc, char **argv)
{

    if (argc <= 1) {
        shell_print(sh, "usage: fw_upload <filename>\n");
        return;
    }

    modem_upload_fw(argv[1]);
}


void do_fota_cancel(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Sending modem fota cancel command\n");
    cancel_fota_download(COMM_DEVICE_NRF9160);
}

void do_airplane_mode(const struct shell *sh, size_t argc, char **argv)
{
    if (argc <= 1) {
        shell_print(sh, "usage: airplane_mode <0|1>\n");
        return;
    }
    int mode = atoi(argv[1]);
    if (mode != 0 && mode != 1) {
        shell_print(sh, "mode must be 0 or 1\n");
        return;
    }
    if (modem_set_airplane_mode(mode) == 0) {
        shell_print(sh, "Airplane mode set");
    } else {
        shell_print(sh, "Failed to set airplane mode");
    }
}


void do_send_modemCellInfo(const struct shell *sh, size_t argc, char **argv)
{
    uint8_t      buf[1024];
    uint16_t     len = 1024;
    cell_info_t *cell_info;
    if (modem_send_modemCellInfo(buf, &len) == 0) {
        cell_info = (cell_info_t *)buf;
        shell_print(sh, "Cell Info: CellID:        %s", cell_info->cellID);
        shell_print(sh, "Cell Info: RSSI:          %d", cell_info->rssi);
        shell_print(sh, "Cell Info: Band:          %d", cell_info->lte_band);
        shell_print(sh, "Cell Info: MCC:           %d", cell_info->mcc);
        shell_print(sh, "Cell Info: MNC:           %d", cell_info->mnc);
        shell_print(sh, "Cell Info: Tracking Area: %d", cell_info->tracking_area);
        shell_print(
            sh,
            "Cell Info: IP:            %d.%d.%d.%d",
            cell_info->ip[0],
            cell_info->ip[1],
            cell_info->ip[2],
            cell_info->ip[3]);
    } else {
        shell_print(sh, "Modem response timed out\n");
    }
}

void do_set_dns(const struct shell *sh, size_t argc, char **argv)
{
    if (argc <= 1) {
        shell_print(sh, "usage: dns <ip>\n");
        return;
    }
    shell_print(sh, "Setting DNS to %s\n", argv[1]);
    modem_set_dnsaddr(argv[1]);
}


SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_nrf9160,
    SHELL_CMD(airplane_mode, NULL, "set airplane mode", do_airplane_mode),
    SHELL_CMD(at_passthru, NULL, "enable AT passthru mode to the 9160", do_ATPassthru_mode),
    SHELL_CMD(cancel_download, NULL, "cancel any currently running download on 9160", do_download_cancel),
    SHELL_CMD(check_fota, NULL, "check for fota update", do_try_modem_fota),
    SHELL_CMD(cell_info, NULL, "get cell info", do_send_modemCellInfo),
    SHELL_CMD(dns, NULL, "set new DNS server", do_set_dns),
    SHELL_CMD(download, NULL, "download a file from a URL", do_url_download),
    SHELL_CMD(enable_modem, NULL, "send a command to enable the modem - factory command", do_enable_modem),
    SHELL_CMD(fota_url, NULL, "fota update from URL", do_url_fota),
    SHELL_CMD(fota_cancel, NULL, "cancel any currently running fota on 9160", do_fota_cancel),
    SHELL_CMD(fw_update, NULL, "send fw file to the 9160", do_fw_upload),
    SHELL_CMD(gps_enable, NULL, "enable GPS", do_gps_enable),
    SHELL_CMD(gps_fake, NULL, "use fake GPS data", do_gps_fake),
    SHELL_CMD(info, NULL, "print modem info", do_send_get_info),
    SHELL_CMD(mqtt_client, NULL, "turn on/off the mqtt client", do_mqtt_client),
    SHELL_CMD(reset, NULL, "reset the modem", do_modem_reset),
    SHELL_CMD(send_at, NULL, "send an AT string to the modem", do_send_at),
    SHELL_CMD(send_large_mqtt, NULL, "for Nick's testing, sends arbitrarily large mqtt message", do_send_large_mqtt),
    SHELL_CMD(send_mqtt, NULL, "send json string to the modem", do_modem_send_mqtt),
    SHELL_CMD(simulate_fail, NULL, "Pretened the modem has died", do_modem_simulate_fail),
    SHELL_CMD(status, NULL, "print modem status", do_get_modem_status),
    SHELL_CMD(turn_off, NULL, "turn off the modem", do_modem_turn_off),
    SHELL_CMD(turn_on, NULL, "turn on the modem", do_modem_turn_on),
    SHELL_CMD(ver, NULL, "print modem version", do_get_modem_version),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(nrf9160, &sub_nrf9160, "Commands to control the nrf9160", NULL);
