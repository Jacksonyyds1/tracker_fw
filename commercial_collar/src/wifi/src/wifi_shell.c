/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
/****************************************************************
 *  wifi_shell.c
 *  SPDX-License-Identifier: LicenseRef-Proprietary
 *
 * The wifi code is layered as follows:
 *     wifi_spi.c or wifi_uart.c - Talked to hardware to transfer data to and from the DA1200
 *     wifi.c - HW independent layer to send adn receive messages to and from the DA1200
 *     wifi_at.c - AT command layer to send and receive AT commands to and from the DA1200
 *     net_mgr.c - The network (wifi and lte) api layer, manages the state of the da and
 *                 lte chip and network communication state machine and publishes zbus
 *                 message when revevent stuff happens
 *     wifi_shell.c - A collection of shell commands to test and debug during development
 */
#include "wifi.h"
#include "wifi_at.h"
#include "net_mgr.h"
#include "wifi_at.h"

#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/zbus/zbus.h>
#include <stdlib.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <strings.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include "pmic.h"
#include <cJSON_os.h>
#include "commMgr.h"
#include "radioMgr.h"
#include "fota.h"
#include "nrf53_upgrade.h"
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>

#define CHAR_1               0x18
#define CHAR_2               0x11
#define SHELL_BYPASS_BUF_LEN 3000
char     shell_bypass_buf[SHELL_BYPASS_BUF_LEN];
uint32_t shell_bypass_idx = 0;

#define shell_error_opt(sh, fmt, ...)                            \
    if (sh) {                                                    \
        shell_fprintf(sh, SHELL_ERROR, fmt "\n", ##__VA_ARGS__); \
    }
#define shell_print_opt(sh, fmt, ...)                             \
    if (sh) {                                                     \
        shell_fprintf(sh, SHELL_NORMAL, fmt "\n", ##__VA_ARGS__); \
    }

LOG_MODULE_REGISTER(wifi_shell);

bool bypass_in_use = false;
int  bypass_id     = 0;
void shell_print_ctl_n(const struct shell *sh, char *in_buf, int len, bool printlf);
void shell_print_ctl(const struct shell *sh, char *in_buf, bool printlf);

K_FIFO_DEFINE(ssids_fifo);

// This will be called whenever we are in bypass mode and
// we receive a message from the DA16200
void wifi_shell_on_tx_rx(wifi_msg_t *msg, void *user_data)
{
    const struct shell *sh = (const struct shell *)user_data;
    if (bypass_in_use == false) {
        return;
    }

    if (msg->incoming == 1) {
        // Print any messages already in the queue and
        // then print the new message
        if (uicr_shipping_flag_get()) {
            wifi_msg_t qmsg;
            while (wifi_recv(&qmsg, K_NO_WAIT) == 0) {
                // EAS Now that they are ref counted is this correct?
                if (uicr_shipping_flag_get()) {
                    shell_fprintf(sh, SHELL_NORMAL, "<<");
                }
                shell_print_ctl_n(sh, qmsg.data, qmsg.data_len, true);
            }
        }
        if (uicr_shipping_flag_get()) {
            shell_fprintf(sh, SHELL_NORMAL, "<<");
        }
        shell_print_ctl_n(sh, msg->data, msg->data_len, true);
    } else {
        if (uicr_shipping_flag_get()) {
            shell_fprintf(sh, SHELL_NORMAL, "\r\n>>");
        }
        shell_print_ctl_n(sh, msg->data, msg->data_len, true);
        shell_print(sh, "");
    }
}

int wifi_set_bypass(const struct shell *sh, shell_bypass_cb_t bypass)
{
    if (bypass && bypass_in_use) {
        shell_error(sh, "I have no idea how you got here.");
        return -EBUSY;
    }

    bypass_in_use = !bypass_in_use;
    if (bypass_in_use) {
        bypass_id = wifi_add_tx_rx_cb(wifi_shell_on_tx_rx, (void *)sh);

        // Insert a fake incoming message to trigger dumping any existing
        // messages in the queue
        wifi_msg_t fakemsg = { .incoming  = 1,
                               .timestamp = k_uptime_get(),
                               .data      = "\r\nBypass started, press ctrl-x ctrl-q to escape\r\n",
                               .data_len  = 40 };
        wifi_shell_on_tx_rx(&fakemsg, (void *)sh);
    }

    shell_set_bypass(sh, bypass);

    return 0;
}

void wifi_shell_bypass_cb(const struct shell *sh, uint8_t *data, size_t len)
{
    if (shell_bypass_idx == 0) {
        memset(shell_bypass_buf, 0, SHELL_BYPASS_BUF_LEN);
    }

    bool           wifi_string_complete = false;
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
        wifi_rem_tx_rx_cb(bypass_id);
        wifi_set_bypass(sh, NULL);
        shell_bypass_idx = 0;
        tail             = 0;
        return;
    }

    for (int i = 0; i < len; i++) {
        if (shell_bypass_idx < SHELL_BYPASS_BUF_LEN) {
            if (data[i] == '\n' || data[i] == '\r' || data[i] == '\0') {
                wifi_string_complete = true;
            } else {
                shell_bypass_buf[shell_bypass_idx] = data[i];
                shell_bypass_idx++;
            }
        }
    }
    /* Store last byte for escape sequence detection */
    tail = data[len - 1];

    if (wifi_string_complete && strlen(shell_bypass_buf) > 0) {
        wifi_send_timeout(shell_bypass_buf, K_MSEC(1000));
        shell_bypass_idx = 0;
        memset(shell_bypass_buf, 0, SHELL_BYPASS_BUF_LEN);
    }
}

void do_wifi_ATPassthru_mode(const struct shell *sh, size_t argc, char **argv)
{
#ifdef CONFIG_RELEASE_BUILD
    if (uicr_shipping_flag_get()) {
        // only allow passthru mode in factory
        return;
    }
#endif
    shell_print(sh, "------ AT Passthru mode ------\n");
    shell_print(sh, "reboot to get out of passthru\n");
    // press ctrl-x ctrl-q to escape
    shell_print(sh, "------------------------------\n");

    wifi_set_bypass(sh, wifi_shell_bypass_cb);
}

void do_wifi_reset(const struct shell *sh, size_t argc, char **argv)
{
    wifi_reset();
    shell_print(sh, "Wifi reset complete\n");
}

#define SET_1V8_PARAMS "<1|on|0|off>"
void do_wifi_set_1v8(const struct shell *sh, size_t argc, char **argv)
{
    int new_state = 1;

    if (argc != 2) {
        shell_error(sh, "Usage: %s " SET_1V8_PARAMS, argv[0]);
        return;
    }
    if (strcasecmp(argv[1], "off") == 0 || strcasecmp(argv[1], "0") == 0) {
        new_state = 0;
    }
    shell_print(sh, "Turning wifi 1.8v power %s...\n", new_state ? "on" : "off");
    if (new_state) {
        wifi_1v8_on();
    } else {
        wifi_1v8_off();
    }
}

#define POWER_KEY_PARAMS "<1|on|0|off>"
void do_wifi_power_key(const struct shell *sh, size_t argc, char **argv)
{
    int new_state = 1;

    if (argc < 2) {
        shell_error(sh, "Usage: %s " POWER_KEY_PARAMS, argv[0]);
        return;
    }
    if (strcasecmp(argv[1], "off") == 0 || strcasecmp(argv[1], "0") == 0) {
        new_state = 0;
    }
    shell_print(sh, "Turning wifi power key line to: %s\n", new_state ? "on" : "off");
    wifi_set_power_key(new_state);
    shell_print(sh, "wifi_get_power_key() returned %d\n", wifi_get_power_key());
}

#define SET_3V0_PARAMS "<1|on|0|off>"
void do_wifi_set_3v0(const struct shell *sh, size_t argc, char **argv)
{
    int new_state = 1;

    if (argc < 2) {
        shell_error(sh, "Usage: %s " SET_3V0_PARAMS, argv[0]);
        return;
    }
    if (strcasecmp(argv[1], "off") == 0 || strcasecmp(argv[1], "0") == 0) {
        new_state = 0;
    }
    shell_print(sh, "Wifi 3v3 Enable line set to: %s\n", new_state ? "on" : "off");
    wifi_set_3v0_enable(new_state);
}

#define WIFI_WAKEUP_PARAMS "<ms delay betwen edges (default to 5)>"
void do_wifi_wakeup(const struct shell *sh, size_t argc, char **argv)
{
    int      ms_delay = 5;
    uint64_t now      = k_uptime_get();

    if (argc == 2) {
        ms_delay = strtoul(argv[1], NULL, 10);
    }
    shell_print(sh, "Sent DA a wake up with %d ms delay. " WIFI_WAKEUP_PARAMS, ms_delay);

    if (now - g_last_sleep_time < CONFIG_WIFI_AFTER_SLEEP_WAIT_DEFAULT) {
        shell_error(sh, "Too soon after sleep");
        return;
    }

    wifi_wake_DA(ms_delay);
}

void shell_print_ctl_n(const struct shell *sh, char *in_buf, int len, bool printlf)
{
    int i;
    for (i = 0; i < len; i++) {
        if (in_buf[i] == 0) {
            break;
        }
        switch (in_buf[i]) {
        case '\n':
            if (printlf) {
                shell_fprintf(sh, SHELL_NORMAL, "\n");
            } else {
                shell_fprintf(sh, SHELL_NORMAL, "\\n");
            }
            continue;
        case '\r':
            if (printlf) {
                shell_fprintf(sh, SHELL_NORMAL, "\r");
            } else {
                shell_fprintf(sh, SHELL_NORMAL, "\\r");
            }
            continue;
        case '\t':
            if (printlf) {
                shell_fprintf(sh, SHELL_NORMAL, "\t");
            } else {
                shell_fprintf(sh, SHELL_NORMAL, "\\t");
            }
            continue;
        case '\e':
            shell_fprintf(sh, SHELL_NORMAL, "\\e");
            continue;
        default:
            break;
        }
        if (in_buf[i] >= 20) {
            shell_fprintf(sh, SHELL_NORMAL, "%c", in_buf[i]);
        } else {
            shell_fprintf(sh, SHELL_NORMAL, "\\0x%02x", in_buf[i]);
        }
    }
}
void shell_print_ctl(const struct shell *sh, char *in_buf, bool printlf)
{
    shell_print_ctl_n(sh, in_buf, strlen(in_buf), printlf);
}

///////////////////////////////////////////////////////
// clear_recv()
//  Clear the receive queue
//
//  @param sh - shell to print to
//  @param hide_ctl - true to hide control characters
//
//  @return - total number of bytes cleared
int clear_recv(const struct shell *sh, bool hide_ctl)
{
    int        ret, total = 0;
    bool       printed_hdr = false;
    wifi_msg_t msg;

    for (int i = 0; i < 300; i++) {
        ret = wifi_recv(&msg, K_USEC(10));
        if (ret != 0) {
            break;
        }
        if (sh != NULL && msg.data_len > 0) {
            if (printed_hdr == false) {
                shell_fprintf(sh, SHELL_NORMAL, "Clearing wifi buffers: |");
                printed_hdr = true;
            }
            if (hide_ctl) {
                shell_print_ctl_n(sh, msg.data, msg.data_len, false);
            } else {
                shell_fprintf(sh, SHELL_NORMAL, "%s", msg.data);
            }
        }
        total += msg.data_len;
        wifi_msg_free(&msg);
    }

    if (sh != NULL && printed_hdr) {
        shell_print(sh, "| len: %d", total);
    }
    return total;
}

#define SET_TIME_PARAMS "<YYYY> <MM> <DD> <HH> <mm>"
void do_set_time(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 6 || strlen(argv[1]) != 4 || strlen(argv[2]) != 2 || strlen(argv[3]) != 2 || strlen(argv[4]) != 2
        || strlen(argv[5]) != 2) {
        shell_error(sh, "Usage: %s " SET_TIME_PARAMS, argv[0]);
        return;
    }

    clear_recv(sh, true);

    char *year   = argv[1];
    char *month  = argv[2];
    char *day    = argv[3];
    char *hour   = argv[4];
    char *minute = argv[5];
    char  timecmd[50];
    snprintf(timecmd, 50, "AT+TIME=%s-%s-%s,%s:%s:00", year, month, day, hour, minute);

    if (wifi_send_ok_err_atcmd(timecmd, NULL, K_MSEC(1000)) != 0) {
        return;
    }
}


#define SLEEP_MODE_PARAMS \
    "<0|1|2|3 ms> - 0=Not Sleeping or DPM, 1=DPM,sleep, 2=DPM,awake, 3=RTC sleep, <ms to sleep for>"
void do_sleep_mode(const struct shell *sh, size_t argc, char **argv)
{
    int ret = 0, mode, dur = 1000;

    if (argc < 2 || argc > 3) {
        shell_error(sh, "Usage: %s " SLEEP_MODE_PARAMS, argv[0]);
        return;
    }

    mode = strtol(argv[1], NULL, 10);
    if (mode < 0 || mode > 3 || (mode == 3 && argc != 3)) {
        shell_error(sh, "Usage: %s " SLEEP_MODE_PARAMS, argv[0]);
    }

    if (argc == 3) {
        dur = strtol(argv[2], NULL, 10);
        if (dur < 0) {
            shell_error(sh, "Duration must be >= 0");
            return;
        }
    }
    switch (mode) {
    case 0:
        ret = wifi_set_sleep_mode(WIFI_SLEEP_NONE, 0);
        break;
    case 1:
        ret = wifi_set_sleep_mode(WIFI_SLEEP_DPM_ASLEEP, 0);
        break;
    case 2:
        ret = wifi_set_sleep_mode(WIFI_SLEEP_DPM_AWAKE, 0);
        break;
    case 3:
        ret = wifi_set_sleep_mode(WIFI_SLEEP_RTC_ASLEEP, dur);
        break;
    }
    if (ret != 0) {
        shell_error(sh, "'%s'(%d) when setting sleep mode", wstrerr(-ret), ret);
    } else {
        shell_print(sh, "Sleep mode set");
    }
}

//  @return : 0 when not in DPM mode
//			  1 when in DPM mode and asleep
//			  2 when in DPM mode and awake
//			  3 when in RTC sleep and now awake
void do_check_sleep_mode(const struct shell *sh, size_t argc, char **argv)
{
    int state = wifi_check_sleep_mode();
    switch (state) {
    case 0:
        shell_print(sh, "DA is not in DPM or RTC mode");
        break;
    case 1:
        shell_print(sh, "DA is in DPM mode and sleeping");
        break;
    case 2:
        shell_print(sh, "DA is in DPM mode and awake");
        break;
    case 3:
        shell_print(sh, "DA was in RTC mode and is now awake");
        break;
    default:
        shell_error(sh, "Unknown sleep state/error: '%s'(%d)", wstrerr(-state), state);
    }
}

#define OTA_START_PARAMS "<https://server:port/filename> <expected version A.B.C>"
void do_ota_start(const struct shell *sh, size_t argc, char **argv)
{
    int ret = 0;

    if (argc != 3) {
        shell_error(sh, "Usage: %s " OTA_START_PARAMS, argv[0]);
        return;
    }
    if (strlen(argv[2]) != 5 || argv[2][1] != '.' || argv[2][3] != '.') {
        shell_error(sh, "Expected version must be in the form A.B.C");
        return;
    }
    uint8_t ever[3] = { 0, 0, 0 };
    ever[0]         = argv[2][0] - '0';
    ever[1]         = argv[2][2] - '0';
    ever[2]         = argv[2][4] - '0';

    ret = net_start_ota(argv[1], ever);
    if (ret != 0) {
        shell_error(sh, "Failed to start OTA: %d", ret);
        return;
    }
}

void do_ota_stop(const struct shell *sh, size_t argc, char **argv)
{
    int ret = 0;

    ret = net_stop_ota();
    if (ret != 0) {
        shell_error(sh, "Failed to stop OTA: %d", ret);
        return;
    }
}

void do_flush(const struct shell *sh, size_t argc, char **argv)
{
    clear_recv(sh, true);    // Flush receive buffer
}

static int do_heap(const struct shell *sh, size_t argc, char **argv)
{
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);

#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
    extern struct k_heap    wifi_heap;
    int                     err;
    struct sys_memory_stats stats;

    err = sys_heap_runtime_stats_get(&wifi_heap.heap, &stats);
    if (err) {
        shell_error(sh, "Failed to read kernel system heap statistics (err %d)", err);
        return -ENOEXEC;
    }

    shell_print(sh, "free:           %zu", stats.free_bytes);
    shell_print(sh, "allocated:      %zu", stats.allocated_bytes);
    shell_print(sh, "max. allocated: %zu", stats.max_allocated_bytes);

    return 0;
#else
    shell_error(sh, "Unable to get heap statistics: enable CONFIG_SYS_HEAP_RUNTIME_STATS");
    return 1;
#endif
}

/////////////////////////////////////////////////////////
// do_atcmd()
// Execute an AT command and print the response. If the
// optional "display returns yes" is given, then print
// the response to the shell. Otherwise, just print
// "OK" or "ERROR" to the shell.
//
// This is similar wifi_send_ok_err_atcmd() but this
// diplays all responses until it gets OK or ERROR
//
// @param sh - shell to print to
// @param argc - number of arguments
// @param argv - array of arguments
#define ATCMD_PARAMS "<cmd> [display returns yes/no]"
void do_atcmd(const struct shell *sh, size_t argc, char **argv)
{
    int               ret       = 0;
    int               printctl  = 0;
    k_timeout_t       timeout   = K_MSEC(4000);
    k_timepoint_t     timepoint = sys_timepoint_calc(timeout);
    wifi_wait_array_t wait_msgs;
    char              errstr[100];

#ifdef CONFIG_RELEASE_BUILD
    if (uicr_shipping_flag_get()) {
        shell_error_opt(sh, "Not supported");
        return;
    }
#endif
    if (argc != 2 && argc != 3) {
        shell_error(sh, "Usage: %s " ATCMD_PARAMS, argv[0]);
        return;
    }
    if (argc == 3 && strcasecmp(argv[2], "yes") == 0) {
        printctl = 1;
    }

    clear_recv(sh, true);    // Flush receive buffer

    ret = rm_prepare_radio_for_use(COMM_DEVICE_DA16200, false, K_SECONDS(3));
    if (ret == false) {
        shell_error(sh, "Failed to prepare radio");
        return;
    }

    // Getting the mutex has to happen after the prepare_radio_for_use because
    // it causes a DA wakeup which needs to be handled by another thread
    if ((ret = wifi_get_mutex(timeout, __func__)) != 0) {
        shell_error(sh, "Failed to get mutex %d", ret);
        return;
    }

    ret = wifi_send_timeout(argv[1], timeout);
    if (ret != 0) {
        shell_error(sh, "Failed to send %d", ret);
        goto release_and_exit;
    }

    wait_msgs.num_msgs = 0;    // Initialize the structure
    wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
    wifi_add_wait_msg(&wait_msgs, "\r\nERROR%99s\r\n", true, 1, errstr);
    // We can use the bypass buffer since this cmd can't run in bypass mode
    wifi_add_wait_msg(&wait_msgs, "%s", true, 1, shell_bypass_buf);

    while (1) {
        shell_bypass_buf[0] = 0;
        memset(errstr, 0, sizeof(errstr));
        memset(shell_bypass_buf, 0, sizeof(shell_bypass_buf));
        timeout = sys_timepoint_timeout(timepoint);
        ret     = wifi_wait_for(&wait_msgs, timeout);
        if (ret < 0) {
            shell_error(sh, "Timeout to wait for OK or ERROR");
            goto release_and_exit;
        }
        if (ret == 0) {
            shell_print(sh, "OK");
            goto release_and_exit;
        }
        if (ret == 1) {
            shell_print(sh, "ERROR:%s", errstr);
            goto release_and_exit;
        } else {
            shell_print_ctl(sh, shell_bypass_buf, true);
            shell_print(sh, "");
        }
    }

release_and_exit:
    wifi_release_mutex();
    rm_done_with_radio(COMM_DEVICE_DA16200);
}

void do_DA_fw_version(const struct shell *sh, size_t argc, char **argv)
{
    char ver[60];
    int  ret = rm_prepare_radio_for_use(COMM_DEVICE_DA16200, false, K_SECONDS(3));
    if (ret == false) {
        shell_error(sh, "Failed to prepare radio");
        return;
    }

    ret = wifi_get_da_fw_ver(ver, 60, K_MSEC(1000));
    if (ret != 0) {
        shell_error(sh, "Failed to get DA16200 fw ver");
    } else {
        shell_print(sh, "DA16200 FW version is %s", ver);
    }
    rm_done_with_radio(COMM_DEVICE_DA16200);
}

void do_wfscan(const struct shell *sh, size_t argc, char **argv)
{
    int ret;
    int ms = 200;

    if (argc == 2) {
        ms = strtol(argv[1], NULL, 10);
    }

    ret = rm_prepare_radio_for_use(COMM_DEVICE_DA16200, false, K_SECONDS(3));
    if (ret == false) {
        shell_error(sh, "Failed to prepare radio");
        return;
    }

    ret = wifi_get_wfscan(shell_bypass_buf, 1500, K_MSEC(8000));
    if (ret != 0) {
        shell_error(sh, "'%s'(%d) from wifi_get_wfscan()", wstrerr(-ret), ret);
    } else {
        shell_bypass_buf[1500] = 0;
        shell_print(sh, "wfscan:\r\n%s", shell_bypass_buf);
    }
    rm_done_with_radio(COMM_DEVICE_DA16200);
}


#define CONNECT_SSID_PARAMS                                                                                                                   \
    "<ssid> <key> <sec> <keyidx> <enc> <hidden>\r\n"                                                                                          \
    "<ssid>: SSID. 1 ~ 32 characters are allowed\r\n"                                                                                         \
    "<key>: Passphrase. 8 ~ 63 characters are allowed or NULL if sec is 0 or 5\r\n"                                                           \
    "<sec>: Security protocol. 0 (OPEN), 1 (WEP), 2 (WPA), 3 (WPA2), 4 (WPA+WPA2) ), 5 (WPA3 OWE), 6 (WPA3 SAE), 7 (WPA2 RSN & WPA3 SAE)\r\n" \
    "<keyidx>: Key index for WEP. 0~3 ignored if sec is 0,2-7\r\n"                                                                            \
    "<enc>: Encryption. 0 (TKIP), 1 (AES), 2 (TKIP+AES)   ignored if sec is 0,1 or 5\r\n"                                                     \
    "<hidden>: 1 (<ssid> is hidden), 0 (<ssid> is NOT hidden)\r\n"
void do_connect_ssid(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 7) {
        shell_error(sh, "Usage: %s " CONNECT_SSID_PARAMS, argv[0]);
        return;
    }

    if (rm_is_enable() == true) {
        if (rm_prepare_radio_for_use(COMM_DEVICE_DA16200, false, K_SECONDS(3)) == false) {
            LOG_ERR("Failed to prepare radio");
            return;
        }
    }

    int ret = rm_connect_to_AP(
        argv[1], argv[2], strtoul(argv[3], NULL, 10), strtoul(argv[4], NULL, 10), strtoul(argv[5], NULL, 10));
    if (ret != 0) {
        shell_error(sh, "'%s'(%d) when sending connect cmd", wstrerr(-ret), ret);
        goto conn_exit;
    } else {
        shell_print(sh, "connection command sent");
    }

    if (rm_is_enable() == true) {
        uint64_t start = k_uptime_get();
        // We asked the DA to start trying to connect.  Once it starts
        // the radio manager will keep the DA awake, but there is a small window
        // where it might be slept if we let it.  So confirm the radio manager
        // knows we are trying to connect.S
        while (rm_wifi_is_connecting() == false && (k_uptime_get() - start) < 2) {
            k_sleep(K_MSEC(100));
        }

        if (rm_wifi_is_connecting() == false) {
            shell_error(sh, "Radio manager should have starting switching to Wifi but didn't");
        }
    }

conn_exit:
    if (rm_is_enable() == true) {
        rm_done_with_radio(COMM_DEVICE_DA16200);
    }
}

#define LEVEL_SHIFTER_PARAMS "<on|off>"
void do_level_shifter(const struct shell *sh, size_t argc, char **argv)
{
    int new_state = 1;

    if (argc != 2) {
        shell_error(sh, "Usage: %s " LEVEL_SHIFTER_PARAMS, argv[0]);
        return;
    }
    if (strcasecmp(argv[1], "off") == 0) {
        new_state = 0;
    }
    wifi_set_level_shifter(new_state);
}

void cert_upload_cb(const struct shell *sh, uint8_t *data, size_t len)
{
    int            i = 0;
    static uint8_t tail;
    char           errorstr[100];
    bool           escape = false;

    /* Check if escape criteria is met. */
    if (tail == CHAR_1 && data[0] == CHAR_2) {
        escape = true;
        // We already put the ctrl-x in the buffer, so remove it
        shell_bypass_buf[--shell_bypass_idx] = 0;
    } else {
        for (i = 0; ((i < (len - 1)) && (shell_bypass_idx < SHELL_BYPASS_BUF_LEN - 1)); i++) {
            if (data[i] == CHAR_1 && data[i + 1] == CHAR_2) {
                escape = true;
                break;
            } else {
                shell_bypass_buf[shell_bypass_idx++] = data[i];
            }
        }
        tail                                 = data[i];
        shell_bypass_buf[shell_bypass_idx++] = data[i++];
    }

    if (escape) {
        shell_bypass_buf[shell_bypass_idx++] = '\003';
        shell_bypass_buf[shell_bypass_idx++] = 0;
        shell_print(sh, "Cert Upload complete");
        shell_print_ctl_n(sh, shell_bypass_buf, shell_bypass_idx, false);
        shell_print(sh, "\n\rSending cert to DA");
        if (wifi_send_ok_err_atcmd(shell_bypass_buf, errorstr, K_MSEC(1000)) != 0) {
            shell_error(sh, "Error sending cert to DA: %s", errorstr);
        }
        shell_set_bypass(sh, NULL);
        tail = 0;
        return;
    }
}

void do_upload_cert(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 2) {
        shell_error(sh, "Usage: %s <type, 0=CA, 1=cert, 2=priv_key>", argv[0]);
        return;
    }
    int type = strtoul(argv[1], NULL, 10);
    if (type < 0 || type > 3) {
        shell_error(sh, "Invalid type");
        return;
    }
    shell_bypass_buf[0] = 0x1B;
    shell_bypass_buf[1] = 'C';
    shell_bypass_buf[2] = argv[1][0];
    shell_bypass_buf[3] = ',';
    shell_bypass_buf[4] = 0;

    shell_bypass_idx = 4;

    shell_print(sh, "------ Certficate Upload mode, press ctrl-x ctrl-q to end ------\n");

    shell_set_bypass(sh, cert_upload_cb);
}

#define GET_OTP_PARAMS "<[0x]register addr> <size of data>"
void do_get_otp_reg(const struct shell *sh, size_t argc, char **argv)
{
    int addr, len;

    if (argc != 3) {
        shell_error(sh, "Usage: %s " GET_OTP_PARAMS, argv[0]);
        return;
    }
    if (argv[1][0] == '0' && argv[1][1] == 'x') {
        argv[1] += 2;
        addr = strtoul(argv[1], NULL, 16);
    } else {
        addr = strtoul(argv[1], NULL, 10);
    }
    len = strtoul(argv[2], NULL, 10);
    if (len > 4 || len < 1) {
        shell_error(sh, "Invalid len");
        return;
    }
    int ret = wifi_get_otp_register(addr, len, K_MSEC(2000));
    if (ret < 0) {
        shell_error(sh, "Failed to get otp register");
    } else {
        shell_print(sh, "otp register is %d", ret);
    }
}

#define SET_OTP_PARAMS "<[0x]register addr> <size of data> <[0x]data>"
void do_set_otp_reg(const struct shell *sh, size_t argc, char **argv)
{
    int addr, len, data;

    if (argc != 4) {
        shell_error(sh, "Usage: %s " SET_OTP_PARAMS, argv[0]);
        return;
    }
    if (argv[1][0] == '0' && argv[1][1] == 'x') {
        argv[1] += 2;
        addr = strtoul(argv[1], NULL, 16);
    } else {
        addr = strtoul(argv[1], NULL, 10);
    }
    len = strtoul(argv[2], NULL, 10);
    if (len > 4 || len < 1) {
        shell_error(sh, "Invalid len");
        return;
    }

    if (argv[3][0] == '0' && argv[3][1] == 'x') {
        argv[3] += 2;
        data = strtoul(argv[3], NULL, 16);
    } else {
        data = strtoul(argv[3], NULL, 10);
    }

    int ret = wifi_set_otp_register(addr, len, data, K_MSEC(2000));
    if (ret < 0) {
        shell_error(sh, "Failed to set otp register");
    } else {
        shell_print(sh, "otp register set");
    }
}

void do_get_xtal(const struct shell *sh, size_t argc, char **argv)
{
    int ret = wifi_get_xtal(K_MSEC(2000));
    if (ret < 0) {
        shell_error(sh, "Failed to get xtal");
    } else {
        shell_print(sh, "xtal is %d", ret);
    }
}

void do_start_rf_test_mode(const struct shell *sh, size_t argc, char **argv)
{
    int ret = wifi_start_XTAL_test();
    if (ret < 0) {
        shell_error(sh, "Failed to start rf test mode");
    } else {
        shell_print(sh, "rf test mode started");
    }
}

void do_stop_rf_test_mode(const struct shell *sh, size_t argc, char **argv)
{
    wifi_stop_XTAL_test();
    shell_print(sh, "set normal mode");
}

#define NVRAM_GET_PARAMS "0x<addr> <len> {uicrbu=0x3AD000 96}"
void do_nvram_get(const struct shell *sh, size_t argc, char **argv)
{
    static uint8_t ram_buf[256];
    if (argc != 3) {
        shell_error(sh, "Usage: %s " NVRAM_GET_PARAMS, argv[0]);
        return;
    }
    uint32_t addr = strtoul(argv[1], NULL, 16);
    int      len  = strtoul(argv[2], NULL, 10);
    if (len > 256) {
        shell_error(sh, "len too big");
        return;
    }
    shell_print(sh, "Getting nvram from 0x%x, len %d", addr, len);
    clear_recv(sh, true);    // Flush receive buffer
    int ret = wifi_get_nvram(addr, ram_buf, len, K_MSEC(1000));
    if (ret < 0) {
        shell_error(sh, "Failed to get nvram %d", ret);
    } else {
        shell_hexdump(sh, ram_buf, len);
    }
}

#define NVRAM_PUT_PARAMS "0x<addr> <hexa string>"
void do_nvram_put(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 3) {
        shell_error(sh, "Usage: %s " NVRAM_PUT_PARAMS, argv[0]);
        return;
    }
    uint32_t addr = strtoul(argv[1], NULL, 16);
    int      len  = strlen(argv[2]);
    if ((len & 0x01) != 0) {
        shell_error(sh, "len of data must be even (i.e. 01FF20)");
        return;
    }
    int ret = wifi_put_nvram(addr, argv[2], K_MSEC(1000));
    if (ret < 0) {
        shell_error(sh, "Failed to put data to nvram");
    }
}

void do_show_status(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "DA is%s initialized", da_state.initialized ? "" : " not");
    if (da_state.ap_connected == 1) {
        shell_print(sh, "DA is connected to AP %s with IP %s", da_state.ap_name, da_state.ip_address);
    } else if (da_state.ap_connected == -1) {
        shell_print(sh, "Unknown if DA connected to an AP");
    } else {
        shell_print(sh, "DA is not connected to AP");
    }

    if (da_state.ap_safe == 1) {
        shell_print(sh, "AP is a safe zone");
    } else if (da_state.ap_safe == -1) {
        shell_print(sh, "Unknown if AP is a safe zone");
    } else {
        shell_print(sh, "AP is not a safe zone");
    }

    if (da_state.ntp_server_set == 1) {
        shell_print(sh, "DA has NTP server set");
    } else if (da_state.ntp_server_set == -1) {
        shell_print(sh, "Unknown if DA has NTP server set");
    } else {
        shell_print(sh, "DA does not have NTP server set");
    }

    if (da_state.dpm_mode == 1) {
        shell_print(sh, "DA is in DPM mode");
    } else if (da_state.dpm_mode == -1) {
        shell_print(sh, "Unknown if DA is in DPM mode");
    } else {
        shell_print(sh, "DA is not in DPM mode");
    }
    if (da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        shell_print(sh, "	DA is sleeping");
    } else if (da_state.is_sleeping == -1) {
        shell_print(sh, "	Unknown if DA is sleeping");
    } else {
        shell_print(sh, "	DA is not sleeping");
    }

    shell_print(sh, "MQTT client id: %s", da_state.mqtt_client_id);
    if (da_state.mqtt_broker_connected == 1) {
        shell_print(sh, "MQTT client is connected to broker");
    } else if (da_state.mqtt_broker_connected == -1) {
        shell_print(sh, "Unknown if MQTT client is connected to broker");
    } else {
        shell_print(sh, "MQTT client is not connected to broker");
    }
    shell_print(sh, "Last MQTT message sent at %lld", da_state.mqtt_last_msg_time);

    if (da_state.mqtt_certs_installed == 1) {
        shell_print(sh, "MQTT certs are installed");
    } else if (da_state.mqtt_certs_installed == -1) {
        shell_print(sh, "Unknown if MQTT certs are installed");
    } else {
        shell_print(sh, "MQTT certs are not installed");
    }

    if (da_state.mqtt_enabled == 1) {
        shell_print(sh, "MQTT is enabled");
    } else if (da_state.mqtt_on_boot == -1) {
        shell_print(sh, "Unknown if MQTT is enabled");
    } else {
        shell_print(sh, "MQTT is not enabled");
    }

    if (da_state.mqtt_on_boot == 1) {
        shell_print(sh, "MQTT will start on boot");
    } else if (da_state.mqtt_on_boot == -1) {
        shell_print(sh, "Unknown if MQTT will start on boot");
    } else {
        shell_print(sh, "MQTT will not start on boot");
    }

    shell_print(sh, "MQTT num topics: %d", da_state.mqtt_sub_topic_count);
    for (int t = 0; t < da_state.mqtt_sub_topic_count; t++) {
        if (da_state.mqtt_sub_topics[t][0] != 0) {
            shell_print(sh, "MQTT topic %d:%s", t, da_state.mqtt_sub_topics[t]);
        }
    }

    if (da_state.uicr_bu_status == 1) {
        shell_print(sh, "UICR backup exists");
        shell_hexdump(sh, da_state.uicr_bu, DA_UICR_BACKUP_SIZE);
    } else if (da_state.uicr_bu_status == -1) {
        shell_print(sh, "Unknown if UICR Backup was made");
    } else {
        shell_print(sh, "UICR backup doesn't exist");
    }

    if (da_state.dhcp_client_name_set == 1) {
        shell_print(sh, "DHCP client host name is set to %s", da_state.dhcp_client_name);
    } else if (da_state.dhcp_client_name_set == -1) {
        shell_print(sh, "Unknown if DHCP client host name is set");
    } else {
        shell_print(sh, "DHCP client host name is not set");
    }

    if (da_state.mac_set == 1) {
        char *mac = uicr_wifi_mac_address_get();
        shell_print(sh, "DA MAC addr is set to %s", mac);
    } else if (da_state.mac_set == -1) {
        shell_print(sh, "Unknown DA MAC addr is set");
    } else {
        shell_print(sh, "DA MAC addr is not set");
    }

    if (da_state.xtal_set == 1) {
        int xtal = uicr_wifi_tuning_value_get();
        shell_print(sh, "DA XTAL is set to %d", xtal);
    } else if (da_state.xtal_set == -1) {
        shell_print(sh, "Unknown DA XTAL is set");
    } else {
        shell_print(sh, "DA XTAL is not set");
    }

    if (da_state.onboarded == 1) {
        shell_print(sh, "This D1 is onboarded");
    } else if (da_state.onboarded == -1) {
        shell_print(sh, "Unknown if this D1 is onboarded");
    } else {
        shell_print(sh, "This D1 is not onboarded");
    }

    shell_print(sh, "ota_progress: %s", net_ota_progress_str(da_state.ota_progress));

    shell_print(sh, "DA software version: %d.%d.%d", da_state.version[0], da_state.version[1], da_state.version[2]);
    shell_print(sh, "wifi queue, size: %d", wifi_msg_cnt());
    wifi_msg_t msg;
    for (int j = 0; j < wifi_msg_cnt(); j++) {
        int ret = wifi_peek_msg(&msg, j);
        if (ret == 0) {
            shell_print(sh, "            msg size %d: %10s", msg.data_len, msg.data);
        } else {
            shell_error(sh, "Failed to peek at msg %d", j);
        }
    }
}

void do_json_print(const struct shell *sh, size_t argc, char **argv)
{
    wifi_arr_t       *list = wifi_get_last_ssid_list();
    fuel_gauge_info_t fuel;
    char             *machine_id = uicr_serial_number_get();
    char             *json       = NULL;

    if (fuel_gauge_get_latest(&fuel) != 0) {
        shell_error(sh, "Failed to get fuel gauge info");
        fuel.soc = 0;
    }

    if (list->count > 0) {
        for (int i = 0; i < list->count; i++) {
            k_fifo_put(&ssids_fifo, &(list->wifi[i]));
        }

        int loop_count = 0;
        int ret        = 1;    // just wait, you'll see
        while (ret > 0) {
            ret = json_telemetry(
                &json,
                machine_id,
                fuel,
                get_charging_active(),
                RADIO_TYPE_WIFI,
                da_state.version,
                da_state.ap_name,
                da_state.ap_safe == 1,
                false,
                &ssids_fifo,
                loop_count,
                -1);
            loop_count++;
            if (json != NULL) {
                shell_print(sh, "telemetry json is %s", json);
            } else {
                shell_error(sh, "Failed to create json");
            }
            if (ret > 0) {
                // need to loop here
            }
        }
    } else {
        shell_error(sh, "No SSIDs found");
    }
    shell_print(sh, "\r\n");
    json = json_connectivity_msg(machine_id, 234);
    shell_print(sh, "connectivity json is %s", json);

    shell_print(sh, "\r\n");
    json = json_shadow_report(machine_id, &shadow_doc, NULL);
    shell_print(sh, "shadow json is %s", json);
}

void test_topic(const struct shell *sh, char *new_topics[])
{
    int  ret = 0, num_topics = 0;
    bool was_connected = false;

    if (da_state.mqtt_broker_connected == 1) {
        was_connected = true;
        ret           = wifi_set_mqtt_state(0, K_MSEC(1500));
        if (ret != 0) {
            shell_error(sh, "Failed to disconnect from mqtt broker");
            return;
        }
    }
    shell_print(sh, "Setting topics");
    for (int t = 0; t < CONFIG_IOT_MAX_TOPIC_NUM && new_topics[t] != 0; t++) {
        num_topics++;
        shell_fprintf(sh, SHELL_NORMAL, "topic %d: |", t);
        shell_print_ctl_n(sh, new_topics[t], strlen(new_topics[t]), false);
        shell_fprintf(sh, SHELL_NORMAL, "|\r\n");
    }
    ret = wifi_insure_mqtt_sub_topics(new_topics, K_MSEC(500));
    if (ret != 0) {
        shell_error(sh, "Failed to set mqtt sub topics");
    } else {
        shell_print(sh, "mqtt sub topics set");
    }

    if (was_connected) {
        ret = wifi_set_mqtt_state(1, K_MSEC(1500));
        if (ret != 0) {
            shell_error(sh, "Failed to re-connect to mqtt broker");
            return;
        }
    }
}

#define TOPICSET_PARAMS "<type_num> [type_num ...]"
void do_topicset(const struct shell *sh, size_t argc, char **argv)
{
    char *new_topics[CONFIG_IOT_MAX_TOPIC_NUM + 1];
    int   i;

    if (argc < 2 || argc > CONFIG_IOT_MAX_TOPIC_NUM + 1) {
        shell_error(sh, "Usage: %s " TOPICSET_PARAMS, argv[0]);
        return;
    }

    // gonna use the shell bypass buffer to hold the topics cause
    // it is currenly not used

    for (i = 0; i < (argc - 1); i++) {
        int type = atoi(argv[i + 1]);
        if (type < 0 || type > 30) {
            shell_error(sh, "Invalid type %d", type);
            return;
        }
        snprintf(
            &(shell_bypass_buf[i * 64]), 64, "messages/%d/%d/%s/c2d", CONFIG_IOT_MQTT_BRAND_ID, type, da_state.mqtt_client_id);
        new_topics[i] = &(shell_bypass_buf[i * 64]);
        shell_print(sh, "topic %d: %s", i, new_topics[i]);
    }
    new_topics[argc - 1] = 0;

    if (rm_prepare_radio_for_use(COMM_DEVICE_DA16200, false, K_SECONDS(3)) == false) {
        shell_error(sh, "failed to prepare DA");
        return;
    }

    int ret = wifi_set_mqtt_sub_topics(new_topics, K_MSEC(100));
    if (ret == 0) {
        shell_print(sh, "mqtt sub topics set");
    } else {
        shell_error(sh, "Failed to set mqtt sub topics");
    }
    rm_done_with_radio(COMM_DEVICE_DA16200);
}

#define CHANGE_SN_PARAMS "<12 character new serial>"
void do_change_serial(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 2 || strlen(argv[1]) != 12) {
        shell_error(sh, "Usage: %s " CHANGE_SN_PARAMS, argv[0]);
        return;
    }
    shell_print(sh, "Changing serial number to %s", argv[1]);
    uicr_serial_number_set_override(argv[1]);
}

void do_get_rssi(const struct shell *sh, size_t argc, char **argv)
{
    if (rm_prepare_radio_for_use(COMM_DEVICE_DA16200, false, K_SECONDS(3)) == false) {
        shell_error(sh, "failed to prepare DA");
        return;
    }
    int rssi;
    int ret = wifi_get_rssi(&rssi, K_MSEC(500));
    if (ret != 0) {
        shell_error(sh, "'%s'(%d) when getting RSSI", wstrerr(-ret), ret);
    } else {
        shell_print(sh, "Reported RSSI is %d", rssi);
    }
    rm_done_with_radio(COMM_DEVICE_DA16200);
}

#define HTTP_GET_PARAMS "<url> <filename> [skip headers <true>|false]"
void do_http_get(const struct shell *sh, size_t argc, char **argv)
{
    bool skip_headers = true;
    if (argc != 3 && argc != 4) {
        shell_error(sh, "Usage: %s" HTTP_GET_PARAMS, argv[0]);
        return;
    }
    if (argc == 4 && strcasecmp(argv[3], "false") == 0) {
        skip_headers = false;
    }

    if (rm_prepare_radio_for_use(COMM_DEVICE_DA16200, false, K_SECONDS(3)) == false) {
        shell_error(sh, "failed to prepare DA");
        return;
    }

    int ret = wifi_http_get(argv[1], argv[2], skip_headers, K_MSEC(600));
    if (ret == 0) {
        shell_print(sh, "HTTP get succeeded");
    } else {
        shell_print(sh, "HTTP get failed %d", ret);
    }
    rm_done_with_radio(COMM_DEVICE_DA16200);
}

void do_http_progress(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Download has received %ld bytes so far", http_amt_written);
}

#define MQTT_PARAMS "<1|on|off|0>"
void do_enable_mqtt(const struct shell *sh, size_t argc, char **argv)
{
    int ret, new_state = 1;

    if (da_state.dpm_mode == DA_STATE_KNOWN_TRUE && da_state.is_sleeping == DA_STATE_KNOWN_TRUE) {
        LOG_ERR("DPM is on and sleeping, so we can't change mqtt state");
        LOG_ERR("Setting dpm_mode causes a reboot which causes mqtt to stop,");
        LOG_ERR("so to get both on you need to set mqqt to auto start using ");
        LOG_ERR("\"da16200 mqtt_on_boot 1\" first and then");
        LOG_ERR("use \"da16200 dpm_mode 1\" to set reboot into dpm mode");
        return;
    }

    if (argc != 2) {
        shell_error(sh, "Usage: %s " MQTT_PARAMS, argv[0]);
        return;
    }
    if (argv[1][0] == '0' || strcasecmp(argv[1], "off") == 0) {
        new_state = 0;
    }

    if (rm_prepare_radio_for_use(COMM_DEVICE_DA16200, false, K_SECONDS(3)) == false) {
        shell_error(sh, "failed to prepare DA");
        return;
    }
    ret = wifi_set_mqtt_state(new_state, K_SECONDS(1));
    if (ret != 0) {
        shell_error(sh, "'%s'(%d) when setting mqtt state", wstrerr(-ret), ret);
    }
    rm_done_with_radio(COMM_DEVICE_DA16200);
}

#define MQTT_ON_BOOT_PARAMS "<on|1|off|0>"
void do_mqtt_on_boot(const struct shell *sh, size_t argc, char **argv)
{
    int new_state = 1;

    if (argc != 2) {
        shell_error(sh, "Usage: %s " MQTT_ON_BOOT_PARAMS, argv[0]);
        return;
    }
    if (argv[1][0] == '0' || strcasecmp(argv[1], "off") == 0) {
        new_state = 0;
    }

    if (rm_prepare_radio_for_use(COMM_DEVICE_DA16200, false, K_SECONDS(3)) == false) {
        shell_error(sh, "failed to prepare DA");
        return;
    }
    if (wifi_set_mqtt_boot_state(new_state, K_MSEC(600)) != 0) {
        shell_error(sh, "Failed to set mqtt broker");
    }
    rm_done_with_radio(COMM_DEVICE_DA16200);
}

#define SAVED_SSID_PARAMS "[add idx ssid password sec keyidx enc hidden safe] | [del idx]"
void do_saved_ssid_file(const struct shell *sh, size_t argc, char **argv)
{
    int ret, op = 0;    // default to show
    int idx = 0;
    if (argc != 1 && argc != 3 && argc != 10) {
        shell_error(sh, "Usage: %s " SAVED_SSID_PARAMS, argv[0]);
        return;
    }
    if (argc == 10) {
        if (wifi_num_saved_ssids() >= MAX_SAVED_SSIDS) {
            shell_error(sh, "Max saved ssids reached");
            return;
        }
        op = 1;
    }
    if (argc == 3) {
        if (wifi_num_saved_ssids() == 0) {
            shell_error(sh, "No saved ssids to delete");
            return;
        }
        op  = 2;
        idx = strtoul(argv[2], NULL, 10);
        if (idx < 0 || idx >= MAX_SAVED_SSIDS) {
            shell_error(sh, "Invalid index");
            return;
        }
    }

    if (rm_prepare_radio_for_use(COMM_DEVICE_DA16200, false, K_SECONDS(3)) == false) {
        shell_error(sh, "failed to prepare DA, stopping ssid delete");
        return;
    }
    if (op == 1) {
        idx     = strtoul(argv[2], NULL, 10);
        int ret = wifi_saved_ssids_add(
            idx,
            argv[3],
            argv[4],
            strtoul(argv[5], NULL, 10),
            strtoul(argv[6], NULL, 10),
            strtoul(argv[7], NULL, 10),
            strtoul(argv[8], NULL, 10),
            strtoul(argv[9], NULL, 10),
            K_MSEC(1000));
        if (ret < 0) {
            shell_error(sh, "Failed to add ssid to saved list, %s (%d)", wstrerr(-ret), ret);
            rm_done_with_radio(COMM_DEVICE_DA16200);
            return;
        }
        shell_print(sh, "Saved ssid added");
    } else if (op == 2) {
        // deleting the currently connected AP will cause a problem with the radio manager
        // so we disconnect first
        if (da_state.ap_connected == DA_STATE_KNOWN_TRUE && wifi_find_saved_ssid(da_state.ap_name) == idx) {
            shell_print(sh, "We are connected to the ssid we are deleting, we need to disconnect first");

            shell_print(sh, "Telling DA to disconnect from AP");
            ret = wifi_send_ok_err_atcmd("AT+WFQAP", NULL, K_MSEC(1000));
            if (ret != 0) {
                LOG_ERR("'%s'(%d) disconnecting from AP, continuing", wstrerr(-ret), ret);
            }

            if (rm_get_active_mqtt_radio() == COMM_DEVICE_DA16200) {
                shell_print(sh, "Radio manager was using Wifi, switching");
                rm_switch_to(COMM_DEVICE_NRF9160, false, false);
            }
        }

        int ret = wifi_saved_ssids_del(idx, K_MSEC(1000));
        if (ret < 0) {
            shell_error(sh, "Failed to delete, %s (%d)", wstrerr(-ret), ret);
            rm_done_with_radio(COMM_DEVICE_DA16200);
            return;
        }
        shell_print(sh, "Saved ssid deleted");
    }

    shadow_zone_t zones[MAX_SAVED_SSIDS];
    ret = wifi_get_ap_list(zones, K_SECONDS(3));
    if (ret < 0) {
        shell_error(sh, "Failed to get AP list from DA");
    } else {
        shell_print(sh, "From DA: ");
        for (int i = 0; i < MAX_SAVED_SSIDS; i++) {
            shell_print(sh, "SSID %d: %s, safe:%d", i, zones[i].ssid, zones[i].safe);
        }
    }
    rm_done_with_radio(COMM_DEVICE_DA16200);
}

void do_wfscan_last(const struct shell *sh, size_t argc, char **argv)
{
    if (g_last_ssid_list.count > 0) {
        for (int i = 0; i < g_last_ssid_list.count; i++) {
            shell_print(sh, "SSID %d: %s", i, g_last_ssid_list.wifi[i].ssid);
        }
    } else {
        shell_error(sh, "No SSIDs found");
    }
}

void do_backup_erase(const struct shell *sh, size_t argc, char **argv)
{
    char buf[(DA_UICR_BACKUP_SIZE * 2) + 1];
    memset(buf, 'F', DA_UICR_BACKUP_SIZE * 2);
    buf[DA_UICR_BACKUP_SIZE * 2] = 0;
    int ret                      = wifi_put_nvram(DA_UICR_BACKUP_FLAG, "FFFFFFFF", K_MSEC(1000));
    if (ret < 0) {
        shell_error(sh, "Failed to erase backup flag");
        return;
    }
    ret = wifi_put_nvram(DA_UICR_BACKUP_ADDR, buf, K_MSEC(1000));
    if (ret < 0) {
        shell_error(sh, "Failed to erase backup");
    }
}

void do_backup_replace(const struct shell *sh, size_t argc, char **argv)
{
    // There isn't a backup, make one
    if (uicr_verify() != 0) {
        shell_error(sh, "UICR is not valid, can't back it up");
        return;
    }

    uicr_export((uint32_t *)(da_state.uicr_bu));
    int ret = write_uicr_backup();
    if (ret != 0) {
        shell_error(sh, "'%s'(%d) writing UICR backup", wstrerr(-ret), ret);
        return;
    }
    shell_print(sh, "UICR back up replaced");
}

void do_log_test(const struct shell *sh, size_t argc, char **argv)
{
    LOG_ERR("Testing log message");
}
void do_mutex_count(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "wifi mutex count: %d", wifi_get_mutex_count());
}

#define CRMGR_ENABLER_PARAMS "<'off'|0|'on'|1>"
void do_crmgr_enable(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 2) {
        shell_error(sh, "Usage: %s " CRMGR_ENABLER_PARAMS, argv[0]);
        return;
    }
    if (strcmp("on", argv[1]) == 0 || strcmp("1", argv[1]) == 0) {
        commMgr_enable_S_work(true);
        commMgr_enable_Q_work(true);
        rm_enable(true);
    } else {
        commMgr_enable_S_work(false);
        commMgr_enable_Q_work(false);
        rm_enable(false);
    }
}

#define LOG_FILE_PARAMS "[line len] [num lines]"
void do_log_fill(const struct shell *sh, size_t argc, char **argv)
{
    int llen = 20;
    int lnum = 3;

    if (argc >= 2) {
        llen = strtoul(argv[1], NULL, 10);
    }
    char *line = (char *)k_malloc(llen + 1);
    if (line == NULL) {
        shell_error(sh, "Failed to allocate memory");
        return;
    }
    line[llen] = 0;
    if (argc == 3) {
        lnum = strtoul(argv[2], NULL, 10);
    }

    memset(line, 'X', llen);
    line[llen] = 0;
    for (int i = 0; i < lnum; i++) {
        LOG_WRN("%s", line);
    }
    k_free(line);
}

void do_log_process(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "log_process() returned %d", log_process());
}

//#pragma GCC optimize ("O0")
static void set_wo(int idx, char *ssid, char *mac, float rssi, char *flags, int8_t channel)
{
    if (idx >= MAX_WIFI_OBJS) {
        return;
    }
    strncpy(g_last_ssid_list.wifi[idx].ssid, ssid, 32);
    strncpy(g_last_ssid_list.wifi[idx].macstr, mac, 20);
    g_last_ssid_list.wifi[idx].rssi = rssi;
    strncpy(g_last_ssid_list.wifi[idx].flags, flags, 100);
    g_last_ssid_list.wifi[idx].channel = channel;
}
static void print_wo_list(const struct shell *sh, char *msg)
{
    shell_print(sh, "%s", msg);
    for (int i = 0; i < g_last_ssid_list.count; i++) {
        shell_print(
            sh,
            "SSID %d: %s, %s, %f, %s, %d",
            i,
            g_last_ssid_list.wifi[i].ssid,
            g_last_ssid_list.wifi[i].macstr,
            g_last_ssid_list.wifi[i].rssi,
            g_last_ssid_list.wifi[i].flags,
            g_last_ssid_list.wifi[i].channel);
    }
}
void do_sort_test(const struct shell *sh, size_t argc, char **argv)
{
    shadow_zone_t zones[MAX_SAVED_SSIDS];
    int           ret = wifi_get_ap_list(zones, K_SECONDS(3));
    if (ret < 0) {
        shell_error(sh, "Failed to get AP list from DA");
    } else {
        shell_print(sh, "From DA: ");
        for (int i = 0; i < MAX_SAVED_SSIDS; i++) {
            shell_print(sh, "SSID %d: %s, safe:%d", i, zones[i].ssid, zones[i].safe);
        }
    }

    set_wo(0, "SSID1", "M1", -50.0, "F", 4);
    set_wo(1, "SSID2", "M2", -60.0, "F", 4);
    set_wo(2, "SSID3", "M3", -70.0, "F", 4);
    set_wo(3, "SSID4", "M4", -80.0, "F", 4);
    set_wo(4, "SSID5", "M5", -90.0, "F", 4);
    set_wo(5, "SSID6", "M6", -100.0, "F", 4);
    set_wo(6, "SSID7", "M7", -110.0, "F", 4);
    set_wo(7, "SSID8", "M8", -120.0, "F", 4);
    g_last_ssid_list.count = 8;
    print_wo_list(sh, "Scan results 1");
    ret = wifi_check_for_known_ssid();
    if (ret != 0) {
        shell_error(sh, "'%s'(%d) returned from wifi_check_for_known_ssid()", wstrerr(-ret), ret);
    } else {
        shell_print(sh, "wifi_check_for_known_ssid() returned SSID idx %d", ret);
    }
}
// #pragma GCC reset_options

SHELL_CMD_REGISTER(fill_log_buffer, NULL, "Fill the log buffer to avoid bug DOGG-451. " LOG_FILE_PARAMS, do_log_fill);
SHELL_CMD_REGISTER(log_process, NULL, "call log_process() ", do_log_process);

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_wf,
    SHELL_CMD(atcmd, NULL, "Send an at cmd to the da. " ATCMD_PARAMS, do_atcmd),
    SHELL_CMD(backup_erase, NULL, "Remove the DA's UICR backup. ", do_backup_erase),
    SHELL_CMD(backup_replace, NULL, "Replace the DA's UICR backup with current UICR. ", do_backup_replace),
    SHELL_CMD(change_serial, NULL, "Change (temp) the serial number used for MQTT. " CHANGE_SN_PARAMS, do_change_serial),
    SHELL_CMD(connect_ssid, NULL, "connect to an AP. " CONNECT_SSID_PARAMS, do_connect_ssid),
    SHELL_CMD(crmgr_enable, NULL, "Enable or Disable the Comm and Radio Manager. " CRMGR_ENABLER_PARAMS, do_crmgr_enable),
    SHELL_CMD(flush, NULL, "Flush received message in the wifi queue", do_flush),
    SHELL_CMD(heap, NULL, "Show wifi heap usage", do_heap),
    SHELL_CMD(http_get, NULL, "Http Get into a lfs file. " HTTP_GET_PARAMS, do_http_get),
    SHELL_CMD(http_progress, NULL, "Show the number of bytes downloaded so far", do_http_progress),
    SHELL_CMD(json_print, NULL, "print various json strings we use", do_json_print),
    SHELL_CMD(log_test, NULL, "test to see if logs are working. ", do_log_test),
    SHELL_CMD(mqtt, NULL, "set mqtt broker state. " MQTT_PARAMS, do_enable_mqtt),
    SHELL_CMD(mqtt_on_boot, NULL, "set mqtt connect on boot. " MQTT_ON_BOOT_PARAMS, do_mqtt_on_boot),
    SHELL_CMD(mutex_count, NULL, "show the current wfii mutex count", do_mutex_count),
    SHELL_CMD(nvram_get, NULL, "Get data from nvram. " NVRAM_GET_PARAMS, do_nvram_get),
    SHELL_CMD(nvram_put, NULL, "Put data into nvram. " NVRAM_PUT_PARAMS, do_nvram_put),
    SHELL_CMD(ota_start, NULL, "start a OTA update. " OTA_START_PARAMS, do_ota_start),
    SHELL_CMD(ota_stop, NULL, "stop a OTA update. ", do_ota_stop),
    SHELL_CMD(otp_get, NULL, "Get the value of a DA otp register. " GET_OTP_PARAMS, do_get_otp_reg),
    SHELL_CMD(otp_set, NULL, "Set the value of a DA otp register. " SET_OTP_PARAMS, do_set_otp_reg),
    SHELL_CMD(power_enable_1v8, NULL, "set wifi 1v8 power. " SET_1V8_PARAMS, do_wifi_set_1v8),
    SHELL_CMD(power_enable_3v0, NULL, "set wifi 3v0 power. " SET_3V0_PARAMS, do_wifi_set_3v0),
    SHELL_CMD(power_key, NULL, "set wifi power key line. " POWER_KEY_PARAMS, do_wifi_power_key),
    SHELL_CMD(power_ls, NULL, "Set the level shifter power. " LEVEL_SHIFTER_PARAMS, do_level_shifter),
    SHELL_CMD(reset, NULL, "reset the wifi", do_wifi_reset),
    SHELL_CMD(rf_test_mode_start, NULL, "Reboot the DA into RF Test mode", do_start_rf_test_mode),
    SHELL_CMD(rf_test_mode_stop, NULL, "Reboot the DA into Normal mode", do_stop_rf_test_mode),
    SHELL_CMD(rssi, NULL, "Get the RSSI of the current AP", do_get_rssi),
    SHELL_CMD(saved_ssid_file, NULL, "Manage the saved ssid list. " SAVED_SSID_PARAMS, do_saved_ssid_file),
    SHELL_CMD(set_time, NULL, "Set the time on the DA, set_time " SET_TIME_PARAMS, do_set_time),
    SHELL_CMD(sleep_check, NULL, "Check if the da sleep mode is set", do_check_sleep_mode),
    SHELL_CMD(sleep_mode, NULL, "set sleep mode. " SLEEP_MODE_PARAMS, do_sleep_mode),
    SHELL_CMD(sort_test, NULL, "test the wifi_check_for_known_ssid(). ", do_sort_test),
    SHELL_CMD(status, NULL, "Show the DA's current status", do_show_status),
    SHELL_CMD(topic_set, NULL, "Set the topic list via purina msg types. " TOPICSET_PARAMS, do_topicset),
    SHELL_CMD(upload_cert, NULL, "Upload certificate to DA", do_upload_cert),
    SHELL_CMD(ver, NULL, "get the DA16200 fw version", do_DA_fw_version),
    SHELL_CMD(wakeup, NULL, "Wake up the DA. " WIFI_WAKEUP_PARAMS, do_wifi_wakeup),
    SHELL_CMD(wfscan, NULL, "Do a raw wfscan", do_wfscan),
    SHELL_CMD(wfscan_last, NULL, "Show the the last wfscan results", do_wfscan_last),
    SHELL_CMD(wifi_at_passthru, NULL, "enable AT passthru mode to the DA16200", do_wifi_ATPassthru_mode),
    SHELL_CMD(xtal_get, NULL, "Get the current value of the XTAL", do_get_xtal),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(wf, &sub_wf, "Commands to control the DA16200", NULL);
