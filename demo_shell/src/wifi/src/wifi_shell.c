#include "wifi.h"
#include "wifi_uart.h"

#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <string.h>
#include "modem.h"

#define CHAR_1 0x18
#define CHAR_2 0x11
#define SEND_BUF_LEN 1024
char wifi_send_buf[SEND_BUF_LEN];
uint8_t wifi_send_buf_ptr = 0;
int manu_send_cmd_to_da(char *cmd, char *recv_buf, int maxlen);

void wifi_rx_callback(uint8_t *data, size_t len, void *user_data)
{
    //struct shell *sh = (struct shell *)user_data;
    for(int i=0;i<len;i++) {
        //shell_fprintf(sh, SHELL_INFO, "%c", data[i]);  // causes race condition and hangs MCU - SMR
        printk("%c", data[i]);
    }
}


int wifi_set_bypass(const struct shell *sh, shell_bypass_cb_t bypass)
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


void wifi_shell_bypass_cb(const struct shell *sh, uint8_t *data, size_t len)
{
    uint8_t rbuf[SEND_BUF_LEN];
    if (wifi_send_buf_ptr == 0) {
        memset(wifi_send_buf, 0, SEND_BUF_LEN);
    }

    bool wifi_string_complete = false;
    static uint8_t tail;
    bool escape = false;

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
        wifi_set_bypass(sh, NULL);
        tail = 0;
        return;
    }

    for (int i=0;i<len;i++) {
        if (wifi_send_buf_ptr < SEND_BUF_LEN) {
            if (data[i] == '\n' || data[i] == '\r' || data[i] == '\0') {
                wifi_send_buf[wifi_send_buf_ptr] = '\r';
                wifi_send_buf_ptr++;
                wifi_string_complete = true;
            }
            else {
                wifi_send_buf[wifi_send_buf_ptr] = data[i];
                wifi_send_buf_ptr++;
            }
        }
    }
    /* Store last byte for escape sequence detection */
    tail = data[len - 1];



    if (wifi_string_complete) {
        wifi_send(wifi_send_buf);
        wifi_send_buf_ptr = 0;
        memset(wifi_send_buf, 0, SEND_BUF_LEN);
        while (wifi_recv(rbuf, K_MSEC(1000)) == 0) {
            shell_fprintf(sh, SHELL_INFO, "%s", rbuf);
        }
    }

}



void do_wifi_ATPassthru_mode(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "------ AT Passthru mode ------\n");
    shell_print(sh, "reboot to get out of passthru\n");
    // press ctrl-x ctrl-q to escape
    shell_print(sh, "------------------------------\n");

    wifi_set_bypass(sh, wifi_shell_bypass_cb);
}


void do_wifi_reset(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Resetting wifi...\n");
    wifi_power_off();
    wifi_set_power_key(0);
    wifi_set_3v3_enable(0);
    k_msleep(1000);
    wifi_power_on();
    wifi_set_power_key(0);
    wifi_set_3v3_enable(0);
    k_msleep(1000);
    shell_print(sh, "Wifi reset complete\n");
}

void do_wifi_turn_off(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Turning off wifi...\n");
    wifi_power_off();
    shell_print(sh, "Wifi turned off\n");
}

void do_wifi_turn_on(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "Turning on wifi...\n");
    wifi_power_on();
    shell_print(sh, "Wifi turned on\n");
}

void do_wifi_power_key(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) { 
        shell_print(sh, "Usage: wifi power_key <1|0>\n");
        return;
    }
    int newState = atoi(argv[1]);
    wifi_set_power_key(newState);
    shell_print(sh, "Wifi Power Key line set to: %d\n", newState);
}

void do_wifi_set_3v3_enable(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) { 
        shell_print(sh, "Usage: wifi enable_3v3 <1|0>\n");
        return;
    }
    int newState = atoi(argv[1]);
    wifi_set_3v3_enable(newState);
    shell_print(sh, "Wifi 3v3 Enable line set to: %d\n", newState);
}
uint8_t full_ssid_buf[5000]={0};
int full_ssid_buf_index= 0;
uint8_t ssid_rx_buf[1000]={0};
char packet_to_send[2000];
int packet_to_send_index= 0;
static void parse_ssids(const char* buf)
{
    #define TAB 9
    #define EOL 10
    printf("%s\n",buf);
    enum states {MAC, CHANNEL,RSSI, CREDS,SSID};
    enum states sm_state = MAC;
    char* match = strstr(&buf[2],"+WFSCAN");
    if( match == NULL){
        printf("this is NOT a wifi packet, abort\n");
        return;
    }else{
        if( match == &buf[2]){
            printf("this is a TRULY  wifi packet\n");
        }else {
            printf("this is a STILL NOT a wifi packet\n");
            return;
        }
    }
    memset(packet_to_send,0,2000);
    packet_to_send_index=0;
    for(int i = 10 ;i <strlen(buf);i++){
        switch(buf[i]){
        case TAB:
            printf(" +  ");
            switch(sm_state){
                case MAC:
                    packet_to_send[packet_to_send_index++]= ',';
                    sm_state  = CHANNEL;
                    break;
                case CHANNEL:
                    sm_state  = RSSI;
                    break;
                case RSSI:
                    sm_state  = CREDS;
                    break;
                case CREDS:
                    sm_state  = SSID;
                    break;
                default:
                    break;
            }
            break;
        case EOL:
            printf("+");
            packet_to_send[packet_to_send_index++]= '\n';
            sm_state = MAC;
            break;
        default:
            if( sm_state == RSSI ||
                sm_state == MAC ){
                printf("%c",buf[i]);
                packet_to_send[packet_to_send_index++]= buf[i];
            }
        }
    }
    packet_to_send[packet_to_send_index]= 0;
    printf("[%d]%s\n",packet_to_send_index, packet_to_send);
    modem_send_command(MESSAGE_TYPE_SSIDS, 0x1, packet_to_send, packet_to_send_index);
}
void do_wifi_scan_ssids(const struct shell *sh, size_t argc, char **argv)
{
    char* my_string = "AT+WFSCAN\r";
    full_ssid_buf_index =0;
    memset(full_ssid_buf,0,sizeof(full_ssid_buf));
    if (true) {
        wifi_send(my_string);
        // shell_fprintf(sh, SHELL_INFO, ">>>%s\n", ssid_rx_buf);
        while (wifi_recv_line(ssid_rx_buf, K_MSEC(1000)) == 0) {
            int rx_cnt = strlen(ssid_rx_buf);
            // printf("**1*[%d](%d){%d}\n",rx_cnt,full_ssid_buf_index,strlen(full_ssid_buf));
            strcpy(&full_ssid_buf[full_ssid_buf_index],&ssid_rx_buf[0]);
            full_ssid_buf_index += rx_cnt;
            memset(ssid_rx_buf, 0,1000);
            // printf("**2*[%d](%d){%d}\n",rx_cnt,full_ssid_buf_index,strlen(full_ssid_buf));
        }
        // printf("\n\n");
    }
    // printf("####[%d]%d####",full_ssid_buf_index, strlen(full_ssid_buf));
    parse_ssids(full_ssid_buf);
}
void wifi_sleep(int millisec)
{
    char sleep_string[200];
    char resp[100];
    snprintf(sleep_string,200,"AT+SLEEPMS=%d\r\n",millisec);
    manu_send_cmd_to_da(sleep_string, resp, 100);
}

// Send a command to the DA16200 and wait for a response
// Returns the length of the response
// incoming must be at least WIFI_MSG_SIZE in length
int send_cmd_to_da(const struct shell *sh, char *cmd, char *recv_buf, int maxlen)
{
    int len;
    char incoming[WIFI_MSG_SIZE+1];

    wifi_uart_send(cmd);

    int rlen=0;
    memset(incoming, 0, WIFI_MSG_SIZE+1);
    memset(recv_buf, 0, maxlen);
    wifi_uart_recv(incoming, K_MSEC(1000));

    for (int i=0;i<5;i++) {
        len = strlen(incoming);
        // copy non-control characters to gathered
        for (int i=0; i<len; i++) {
            if (incoming[i] > 31) {
                recv_buf[rlen] = incoming[i];
                rlen++;
                if (rlen == maxlen) {
                    return rlen;
                }
            }
        }
        // Read a little longer in case it is still tramsmitting
        memset(incoming, 0, WIFI_MSG_SIZE+1);
        wifi_uart_recv(incoming, K_MSEC(1));
    }

    return rlen;
}


// Send a command to the DA16200 and wait for a response
// Returns the length of the response
// incoming must be at least WIFI_MSG_SIZE in length
int manu_send_cmd_to_da(char *cmd, char *recv_buf, int maxlen)
{
    int len;
    char incoming[WIFI_MSG_SIZE+1];

    wifi_uart_send(cmd);

    int rlen=0;
    memset(incoming, 0, WIFI_MSG_SIZE+1);
    memset(recv_buf, 0, maxlen);
    wifi_uart_recv(incoming, K_MSEC(1000));

    for (int i=0;i<5;i++) {
        len = strlen(incoming);
        // copy non-control characters to gathered
        for (int i=0; i<len; i++) {
            if (incoming[i] > 31) {
                recv_buf[rlen] = incoming[i];
                rlen++;
                if (rlen == maxlen) {
                    return rlen;
                }
            }
        }
        // Read a little longer in case it is still tramsmitting
        memset(incoming, 0, WIFI_MSG_SIZE+1);
        wifi_uart_recv(incoming, K_MSEC(1));
    }

    return rlen;
}

int clear_at_responses(void)
{
    int len, total = 0;
    char incoming[WIFI_MSG_SIZE+1];

    for (int i=0;i<300;i++) {
        memset(incoming, 0, WIFI_MSG_SIZE+1);
        wifi_uart_recv(incoming, K_MSEC(1));
        len = strlen(incoming);
        total += len;
        if (len == 0) {
            break;
        }
    }

    return total;
}


// Send an 'AT' to the DA16200 and wait for an 'OK'
// This is tried a few times cause there can be 
// some garbage in the UART buffer
bool get_ok_response(const struct shell *sh)
{
    char incoming[WIFI_MSG_SIZE+1];
    char at_cmd[] = "at\r";
    bool got_ok = false;

    int left = clear_at_responses();
    shell_print(sh, "Flushed %d bytes, Sending an 'AT' to get an 'OK'\r\n", left);

    for (int o=0; o<2; o++) {
        memset(incoming, 0, WIFI_MSG_SIZE+1);
        if (send_cmd_to_da(sh, at_cmd, incoming, WIFI_MSG_SIZE)) {
            shell_print(sh, "DA16200 at response: |%s|\r\n", incoming);
            if (strcmp(incoming, "OK") == 0) {
                got_ok = true;
                break;
            }
        }
    }
    return got_ok;
}


int safe_send_cmd(const struct shell *sh, char *cmd, char *incoming, int inlen)
{
    if (get_ok_response(sh) == false) {
        return 0;
    }

    incoming[inlen-1] = 0;
    shell_print(sh, "sending DA16200 |%s|\r\n", cmd);
    int len = send_cmd_to_da(sh, cmd, incoming, inlen-1);
    if (len > 0) {
        shell_print(sh, "DA16200 responded with %s\r\n", incoming);
    } else {
        shell_print(sh, "DA16200 response timed out\r\n");
    }
    return len;
}


char mos_CA[] = "\eC0,"
"-----BEGIN CERTIFICATE-----\r"
"MIIEAzCCAuugAwIBAgIUBY1hlCGvdj4NhBXkZ/uLUZNILAwwDQYJKoZIhvcNAQEL\r"
"BQAwgZAxCzAJBgNVBAYTAkdCMRcwFQYDVQQIDA5Vbml0ZWQgS2luZ2RvbTEOMAwG\r"
"A1UEBwwFRGVyYnkxEjAQBgNVBAoMCU1vc3F1aXR0bzELMAkGA1UECwwCQ0ExFjAU\r"
"BgNVBAMMDW1vc3F1aXR0by5vcmcxHzAdBgkqhkiG9w0BCQEWEHJvZ2VyQGF0Y2hv\r"
"by5vcmcwHhcNMjAwNjA5MTEwNjM5WhcNMzAwNjA3MTEwNjM5WjCBkDELMAkGA1UE\r"
"BhMCR0IxFzAVBgNVBAgMDlVuaXRlZCBLaW5nZG9tMQ4wDAYDVQQHDAVEZXJieTES\r"
"MBAGA1UECgwJTW9zcXVpdHRvMQswCQYDVQQLDAJDQTEWMBQGA1UEAwwNbW9zcXVp\r"
"dHRvLm9yZzEfMB0GCSqGSIb3DQEJARYQcm9nZXJAYXRjaG9vLm9yZzCCASIwDQYJ\r"
"KoZIhvcNAQEBBQADggEPADCCAQoCggEBAME0HKmIzfTOwkKLT3THHe+ObdizamPg\r"
"UZmD64Tf3zJdNeYGYn4CEXbyP6fy3tWc8S2boW6dzrH8SdFf9uo320GJA9B7U1FW\r"
"Te3xda/Lm3JFfaHjkWw7jBwcauQZjpGINHapHRlpiCZsquAthOgxW9SgDgYlGzEA\r"
"s06pkEFiMw+qDfLo/sxFKB6vQlFekMeCymjLCbNwPJyqyhFmPWwio/PDMruBTzPH\r"
"3cioBnrJWKXc3OjXdLGFJOfj7pP0j/dr2LH72eSvv3PQQFl90CZPFhrCUcRHSSxo\r"
"E6yjGOdnz7f6PveLIB574kQORwt8ePn0yidrTC1ictikED3nHYhMUOUCAwEAAaNT\r"
"MFEwHQYDVR0OBBYEFPVV6xBUFPiGKDyo5V3+Hbh4N9YSMB8GA1UdIwQYMBaAFPVV\r"
"6xBUFPiGKDyo5V3+Hbh4N9YSMA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQEL\r"
"BQADggEBAGa9kS21N70ThM6/Hj9D7mbVxKLBjVWe2TPsGfbl3rEDfZ+OKRZ2j6AC\r"
"6r7jb4TZO3dzF2p6dgbrlU71Y/4K0TdzIjRj3cQ3KSm41JvUQ0hZ/c04iGDg/xWf\r"
"+pp58nfPAYwuerruPNWmlStWAXf0UTqRtg4hQDWBuUFDJTuWuuBvEXudz74eh/wK\r"
"sMwfu1HFvjy5Z0iMDU8PUDepjVolOCue9ashlS4EB5IECdSR2TItnAIiIwimx839\r"
"LdUdRudafMu5T5Xma182OC0/u/xRlEm+tvKGGmfFcN0piqVl8OrSPBgIlb+1IKJE\r"
"m/XriWr/Cq4h/JfB7NTsezVslgkBaoU=\r"
"-----END CERTIFICATE-----\r"
"\003";

char mos_crt[] = "\eC1,"
"-----BEGIN CERTIFICATE-----\r"
"MIIDwzCCAqugAwIBAgIBADANBgkqhkiG9w0BAQsFADCBkDELMAkGA1UEBhMCR0Ix\r"
"FzAVBgNVBAgMDlVuaXRlZCBLaW5nZG9tMQ4wDAYDVQQHDAVEZXJieTESMBAGA1UE\r"
"CgwJTW9zcXVpdHRvMQswCQYDVQQLDAJDQTEWMBQGA1UEAwwNbW9zcXVpdHRvLm9y\r"
"ZzEfMB0GCSqGSIb3DQEJARYQcm9nZXJAYXRjaG9vLm9yZzAeFw0yMzExMDMxOTE4\r"
"MjlaFw0yNDAyMDExOTE4MjlaMIGcMQswCQYDVQQGEwJVUzETMBEGA1UECAwKQ2Fs\r"
"aWZvcm5pYTEQMA4GA1UEBwwHT2FrbGFuZDEQMA4GA1UECgwHQ3VsdmVydDEVMBMG\r"
"A1UECwwMUHJvdG9Tb3JjZXJ5MREwDwYDVQQDDAhQcm90b09hazEqMCgGCSqGSIb3\r"
"DQEJARYbZXJpa0BjdWx2ZXJ0ZW5naW5lZXJpbmcuY29tMIIBIjANBgkqhkiG9w0B\r"
"AQEFAAOCAQ8AMIIBCgKCAQEArFy44XWp2i6pIwQSk0GczPnJYE0rM+oj8aZGWjPL\r"
"K2iw7e7C4THW9rnkrUpUDY91ZfVGuoxp0ZC//sDkZDOsbSvdEGzcbF16hioWaWuV\r"
"9IdXaj83U0rsdOO8umEpLrnS5Ri++LwqYGjzRVgYGb3HD9p1ak1KjLJessquG72n\r"
"aOwWGxJxVY4a2YN1XjwW6kaBCMSGVHRSNnm8blhuYVqI5sfsYTX/DQrq9rcR/ENJ\r"
"QBp2YMlK1kgNsdKnOFi2JPnyHb9j5r7PKLgplxsb7aE+V/viin0jcjW6NbPnJ7vR\r"
"VkpicoIrZ21xlTCuj2df1Kxe7+PtWNw9M1N1hGkXLaPx0QIDAQABoxowGDAJBgNV\r"
"HRMEAjAAMAsGA1UdDwQEAwIF4DANBgkqhkiG9w0BAQsFAAOCAQEAfDG3hSECmhzt\r"
"f1tIYPjFi5UfiS6Tvx7tTaOEmaQ3F5pyHLT3Xgun1ZqqoK9HRTy81AHKe4MxdC8M\r"
"wP0FHxGyTx76MMuZBHBjfu/GgdjUIini/yPiHGaujKCGvqjEPvutkFQ569ht4gdu\r"
"HUeJVK+yp/AYojrpgMmauTClm8DE5FoGw7ZuUaQFcpsXfg2Yt7Uc5zA/xXYFWOY4\r"
"f+FkcwujriRHw8Imtsqc5jbOsEe2LfCGXfOzObJrGleoZGeWT2rN2OkbHhsUAOLo\r"
"2MuLZzsgagsMsWYq04MlLMeeXDH3IpaORdoknNuo7JRzroJYbpBm45cNQX4ysf46\r"
"KVQVSPdmLQ==\r"
"-----END CERTIFICATE-----\r"
"\003";

char mos_pri[] = "\eC2,"
"-----BEGIN PRIVATE KEY-----\r"
"MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQCsXLjhdanaLqkj\r"
"BBKTQZzM+clgTSsz6iPxpkZaM8sraLDt7sLhMdb2ueStSlQNj3Vl9Ua6jGnRkL/+\r"
"wORkM6xtK90QbNxsXXqGKhZpa5X0h1dqPzdTSux047y6YSkuudLlGL74vCpgaPNF\r"
"WBgZvccP2nVqTUqMsl6yyq4bvado7BYbEnFVjhrZg3VePBbqRoEIxIZUdFI2ebxu\r"
"WG5hWojmx+xhNf8NCur2txH8Q0lAGnZgyUrWSA2x0qc4WLYk+fIdv2Pmvs8ouCmX\r"
"GxvtoT5X++KKfSNyNbo1s+cnu9FWSmJygitnbXGVMK6PZ1/UrF7v4+1Y3D0zU3WE\r"
"aRcto/HRAgMBAAECggEAE9dyBP+e4kSgKILDLKZ59B1bxt/l9Z+iLE2VfbQb/aRF\r"
"8dicIIEJ8PRsqbI2biqHO4nPxDvTwVHQzZ/LlZQJPmFf8mXFvh0eCgHK+1nCNRhm\r"
"F2StlsNiPavVwagxAyBVxxUL2ZBiWcowbwQkH2/O/Dk2w6+gFwWVjIQeejJhP4eg\r"
"EdaZ0LuCpZQPc581uAqj2lJJGSQ+Pmr/mepjtHaf1XAORF/cs/cDB+kJhriW2I1v\r"
"QSk3sYxSJnh6geFVEvNBbWS4nK5xoUT7RyV71MGNa9ItkTaqKmkMn5s3hxnGGXut\r"
"fk1HsppMEfyiJ7oWf3oatRy0jv1JqdlbsdZ26E/OrQKBgQDlDpkcpo+bz8bUTMNw\r"
"5++GwBMl0xaDvHl4xDmpumu1PSzusCSZZrEUbgBIXtVYZJ7xL6OgLvb4qq6TikKh\r"
"irvYy4bGTmoYbndpAVnSUGBSAc3c30BV/Oop85+bEFO5YshI93YOTgWP+8lgTHy+\r"
"4/g2A4JyJrMKdxSkhoAXuTRAlwKBgQDAouua/gMwT6AomFqpuM5+DrVbAUrqYXtn\r"
"1vrHRRf3QYDlOQ4DMbMmMCt6kjdnKiyiGAJzCND8I79dfoSZ27ALKRbxXIFtNHsH\r"
"ue94hhk0XPrcan9MQyhrWdlUMMoVpXht84cQOFdg3xD0DlAANEnmGjijKvMkzpRj\r"
"PEAib49F1wKBgGHmkmSfgCPdc6MLyED6sPLMJ6L0DNxzcwu9+tNjfWOyaQD/wjTa\r"
"oncT6QUFm3QzVYfKj8oIKMDx2rnuzznSXSV1H/6kR0538Iut6yEr/28tnDp6JTpb\r"
"Zg5WNXKGUPKcmPQu6IOGr3Px7wk8x9ijAVS8vUVi6wVfDjCf2CHLo9yzAoGAcmfP\r"
"2WsGZcjEa5egMLArIr6FgpjP70cZzV/l7DbittvWO0yZP9hid0mgaNkxwjlP7Kyp\r"
"t7wCsdxhKJudEOtiMB6lG48+5qaGct5AlKm/ilO2QPWWyKoR9T+VTOT0/8oYLeS1\r"
"0DJF4qhYHzno1VY4lUn5XR6C7NcrVYxQ4qKyyl0CgYBAxjkp5Nsxa14RMFyfEJOs\r"
"PBOxRocaFV+fKwRKOlYh6drc9bffhlNTHdUnwLgM8pyYvA9/RdT0kdMHrH9NTKse\r"
"9RbVYcL3gMOmPynT3M9Jvv3XsUoOqebSVOgw7Zo4fKPtskikjMHweZK4yLyDtjfl\r"
"+1IM6leVevuqFHeX38DaQw==\r"
"-----END PRIVATE KEY-----\r"
"\003";

char sim_CA[] = "\eC0,"
"-----BEGIN CERTIFICATE-----\r"
"MIIE0zCCA7ugAwIBAgIQGNrRniZ96LtKIVjNzGs7SjANBgkqhkiG9w0BAQUFADCB\r"
"yjELMAkGA1UEBhMCVVMxFzAVBgNVBAoTDlZlcmlTaWduLCBJbmMuMR8wHQYDVQQL\r"
"ExZWZXJpU2lnbiBUcnVzdCBOZXR3b3JrMTowOAYDVQQLEzEoYykgMjAwNiBWZXJp\r"
"U2lnbiwgSW5jLiAtIEZvciBhdXRob3JpemVkIHVzZSBvbmx5MUUwQwYDVQQDEzxW\r"
"ZXJpU2lnbiBDbGFzcyAzIFB1YmxpYyBQcmltYXJ5IENlcnRpZmljYXRpb24gQXV0\r"
"aG9yaXR5IC0gRzUwHhcNMDYxMTA4MDAwMDAwWhcNMzYwNzE2MjM1OTU5WjCByjEL\r"
"MAkGA1UEBhMCVVMxFzAVBgNVBAoTDlZlcmlTaWduLCBJbmMuMR8wHQYDVQQLExZW\r"
"ZXJpU2lnbiBUcnVzdCBOZXR3b3JrMTowOAYDVQQLEzEoYykgMjAwNiBWZXJpU2ln\r"
"biwgSW5jLiAtIEZvciBhdXRob3JpemVkIHVzZSBvbmx5MUUwQwYDVQQDEzxWZXJp\r"
"U2lnbiBDbGFzcyAzIFB1YmxpYyBQcmltYXJ5IENlcnRpZmljYXRpb24gQXV0aG9y\r"
"aXR5IC0gRzUwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQCvJAgIKXo1\r"
"nmAMqudLO07cfLw8RRy7K+D+KQL5VwijZIUVJ/XxrcgxiV0i6CqqpkKzj/i5Vbex\r"
"t0uz/o9+B1fs70PbZmIVYc9gDaTY3vjgw2IIPVQT60nKWVSFJuUrjxuf6/WhkcIz\r"
"SdhDY2pSS9KP6HBRTdGJaXvHcPaz3BJ023tdS1bTlr8Vd6Gw9KIl8q8ckmcY5fQG\r"
"BO+QueQA5N06tRn/Arr0PO7gi+s3i+z016zy9vA9r911kTMZHRxAy3QkGSGT2RT+\r"
"rCpSx4/VBEnkjWNHiDxpg8v+R70rfk/Fla4OndTRQ8Bnc+MUCH7lP59zuDMKz10/\r"
"NIeWiu5T6CUVAgMBAAGjgbIwga8wDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8E\r"
"BAMCAQYwbQYIKwYBBQUHAQwEYTBfoV2gWzBZMFcwVRYJaW1hZ2UvZ2lmMCEwHzAH\r"
"BgUrDgMCGgQUj+XTGoasjY5rw8+AatRIGCx7GS4wJRYjaHR0cDovL2xvZ28udmVy\r"
"aXNpZ24uY29tL3ZzbG9nby5naWYwHQYDVR0OBBYEFH/TZafC3ey78DAJ80M5+gKv\r"
"MzEzMA0GCSqGSIb3DQEBBQUAA4IBAQCTJEowX2LP2BqYLz3q3JktvXf2pXkiOOzE\r"
"p6B4Eq1iDkVwZMXnl2YtmAl+X6/WzChl8gGqCBpH3vn5fJJaCGkgDdk+bW48DW7Y\r"
"5gaRQBi5+MHt39tBquCWIMnNZBU4gcmU7qKEKQsTb47bDN0lAtukixlE0kF6BWlK\r"
"WE9gyn6CagsCqiUXObXbf+eEZSqVir2G3l6BFoMtEMze/aiCKm0oHw0LxOXnGiYZ\r"
"4fQRbxC1lfznQgUy286dUV4otp6F01vvpX1FQHKOtw5rDgb7MzVIcbidJ4vEZV8N\r"
"hnacRHr2lVz2XTIIM6RUthg/aFzyQkqFOFSDX9HoLPKsEdao7WNq\r"
"-----END CERTIFICATE-----\r"
"\003";
#if 0
char sim_CA[] = "\eC0,"
"-----BEGIN CERTIFICATE-----\r"
"MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\r"
"ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6\r"
"b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL\r"
"MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv\r"
"b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj\r"
"ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM\r"
"9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw\r"
"IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6\r"
"VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L\r"
"93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm\r"
"jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\r"
"AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA\r"
"A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI\r"
"U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs\r"
"N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv\r"
"o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU\r"
"5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy\r"
"rqXRfboQnoZsG4q5WTP468SQvvG5\r"
"-----END CERTIFICATE-----\r"
"-----BEGIN CERTIFICATE-----\r"
"MIIE0zCCA7ugAwIBAgIQGNrRniZ96LtKIVjNzGs7SjANBgkqhkiG9w0BAQUFADCB\r"
"yjELMAkGA1UEBhMCVVMxFzAVBgNVBAoTDlZlcmlTaWduLCBJbmMuMR8wHQYDVQQL\r"
"ExZWZXJpU2lnbiBUcnVzdCBOZXR3b3JrMTowOAYDVQQLEzEoYykgMjAwNiBWZXJp\r"
"U2lnbiwgSW5jLiAtIEZvciBhdXRob3JpemVkIHVzZSBvbmx5MUUwQwYDVQQDEzxW\r"
"ZXJpU2lnbiBDbGFzcyAzIFB1YmxpYyBQcmltYXJ5IENlcnRpZmljYXRpb24gQXV0\r"
"aG9yaXR5IC0gRzUwHhcNMDYxMTA4MDAwMDAwWhcNMzYwNzE2MjM1OTU5WjCByjEL\r"
"MAkGA1UEBhMCVVMxFzAVBgNVBAoTDlZlcmlTaWduLCBJbmMuMR8wHQYDVQQLExZW\r"
"ZXJpU2lnbiBUcnVzdCBOZXR3b3JrMTowOAYDVQQLEzEoYykgMjAwNiBWZXJpU2ln\r"
"biwgSW5jLiAtIEZvciBhdXRob3JpemVkIHVzZSBvbmx5MUUwQwYDVQQDEzxWZXJp\r"
"U2lnbiBDbGFzcyAzIFB1YmxpYyBQcmltYXJ5IENlcnRpZmljYXRpb24gQXV0aG9y\r"
"aXR5IC0gRzUwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQCvJAgIKXo1\r"
"nmAMqudLO07cfLw8RRy7K+D+KQL5VwijZIUVJ/XxrcgxiV0i6CqqpkKzj/i5Vbex\r"
"t0uz/o9+B1fs70PbZmIVYc9gDaTY3vjgw2IIPVQT60nKWVSFJuUrjxuf6/WhkcIz\r"
"SdhDY2pSS9KP6HBRTdGJaXvHcPaz3BJ023tdS1bTlr8Vd6Gw9KIl8q8ckmcY5fQG\r"
"BO+QueQA5N06tRn/Arr0PO7gi+s3i+z016zy9vA9r911kTMZHRxAy3QkGSGT2RT+\r"
"rCpSx4/VBEnkjWNHiDxpg8v+R70rfk/Fla4OndTRQ8Bnc+MUCH7lP59zuDMKz10/\r"
"NIeWiu5T6CUVAgMBAAGjgbIwga8wDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8E\r"
"BAMCAQYwbQYIKwYBBQUHAQwEYTBfoV2gWzBZMFcwVRYJaW1hZ2UvZ2lmMCEwHzAH\r"
"BgUrDgMCGgQUj+XTGoasjY5rw8+AatRIGCx7GS4wJRYjaHR0cDovL2xvZ28udmVy\r"
"aXNpZ24uY29tL3ZzbG9nby5naWYwHQYDVR0OBBYEFH/TZafC3ey78DAJ80M5+gKv\r"
"MzEzMA0GCSqGSIb3DQEBBQUAA4IBAQCTJEowX2LP2BqYLz3q3JktvXf2pXkiOOzE\r"
"p6B4Eq1iDkVwZMXnl2YtmAl+X6/WzChl8gGqCBpH3vn5fJJaCGkgDdk+bW48DW7Y\r"
"5gaRQBi5+MHt39tBquCWIMnNZBU4gcmU7qKEKQsTb47bDN0lAtukixlE0kF6BWlK\r"
"WE9gyn6CagsCqiUXObXbf+eEZSqVir2G3l6BFoMtEMze/aiCKm0oHw0LxOXnGiYZ\r"
"4fQRbxC1lfznQgUy286dUV4otp6F01vvpX1FQHKOtw5rDgb7MzVIcbidJ4vEZV8N\r"
"hnacRHr2lVz2XTIIM6RUthg/aFzyQkqFOFSDX9HoLPKsEdao7WNq\r"
"-----END CERTIFICATE-----\r"
"\003";
#endif

char sim_crt[] = "\eC1,"
"-----BEGIN CERTIFICATE-----\r"
"MIIDWTCCAkGgAwIBAgIUPW8wOE/NoIv+GXxNqkxUNSYjs34wDQYJKoZIhvcNAQEL\r"
"BQAwTTFLMEkGA1UECwxCQW1hem9uIFdlYiBTZXJ2aWNlcyBPPUFtYXpvbi5jb20g\r"
"SW5jLiBMPVNlYXR0bGUgU1Q9V2FzaGluZ3RvbiBDPVVTMB4XDTIzMDUyNTA2MjI0\r"
"OFoXDTQ5MTIzMTIzNTk1OVowHjEcMBoGA1UEAwwTQVdTIElvVCBDZXJ0aWZpY2F0\r"
"ZTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALda8ZlSyFmT/s5hX0Iv\r"
"soeOsiXMhjY94M2BJn/IDIUtpUJJoocWhCMml2e7e94PPNZXSMvDfVR9YxiVb2My\r"
"QBjcXO5Nrim2nrTpkFIarCxSESD9k8/YzTkjB2Rn4XCkJX6xa1kT4cXTYVK049SS\r"
"E/53u59fFQ4AsHkJzQy0aX68MlGgmkMcr+5eazgvSaULR4hzyZgReiGt4jPhtPog\r"
"tFJNPxQjflt6DjK/QO/aeWAtkNTHBA/RYVD0kmALJJi1lnX07q+d7F6P9yCG/W4C\r"
"IJNXSsWT9IOklNn8bTTFseskXZpI3JcWc1YSRlLmjgh2dlS65jjL4392oNZ7MZ9r\r"
"LBcCAwEAAaNgMF4wHwYDVR0jBBgwFoAUyI/4x7eLGEToVgJKsiHjniv3lGMwHQYD\r"
"VR0OBBYEFLgGn2DBCRh4W9UDVnqLYVTG+cVSMAwGA1UdEwEB/wQCMAAwDgYDVR0P\r"
"AQH/BAQDAgeAMA0GCSqGSIb3DQEBCwUAA4IBAQCdWfxLCgzD7/iVpri5RTcgq1PE\r"
"PDNKPQm8xDqsj3yfjnOpBeROyO3/ozwkEP9KhqjC8KBP8LMvh0Am+EWrXDi9Nbe3\r"
"J3t5YmYLpjqR1BvMLYJxydKRax/BnB1aVHlScT9HG6qk2+lhXsTdf7YfinXPBDw/\r"
"DA57xADGxxocxLyFzRMnIPpPdZ/3RYqoUHyxAM+wJPcOxaWuZqPDBSYXvNFRFy+s\r"
"K3hRpw4AHlOCojA7n9f2PDFpuBiUfLkoRkDS3s1F9C2maUtHnYZIfU3hf85VmRui\r"
"S2cUjRk76GDSyvUpr2bM4JLicTosvBHiJtChwoubwYPtiEY0ifkYx0hYe3Zw\r"
"-----END CERTIFICATE-----\r"
"\003";

char sim_pri[] = "\eC2,"
"-----BEGIN RSA PRIVATE KEY-----\r"
"MIIEpQIBAAKCAQEAt1rxmVLIWZP+zmFfQi+yh46yJcyGNj3gzYEmf8gMhS2lQkmi\r"
"hxaEIyaXZ7t73g881ldIy8N9VH1jGJVvYzJAGNxc7k2uKbaetOmQUhqsLFIRIP2T\r"
"z9jNOSMHZGfhcKQlfrFrWRPhxdNhUrTj1JIT/ne7n18VDgCweQnNDLRpfrwyUaCa\r"
"Qxyv7l5rOC9JpQtHiHPJmBF6Ia3iM+G0+iC0Uk0/FCN+W3oOMr9A79p5YC2Q1McE\r"
"D9FhUPSSYAskmLWWdfTur53sXo/3IIb9bgIgk1dKxZP0g6SU2fxtNMWx6yRdmkjc\r"
"lxZzVhJGUuaOCHZ2VLrmOMvjf3ag1nsxn2ssFwIDAQABAoIBAQCnj+EC8XhPBMTz\r"
"7mCTp+tLnsiHaqWspFfw9noshLGMc+526bwyIA2Z4gazsc69XMeISjQoovrCX+RT\r"
"7xzgVmflUF1NGohzboUTZ++QWPfHeShWMecHJ2ZFNRHoXFbWDeyGH7WurlDB7S8f\r"
"2lfrR6QmBV3dg5NGPLMJqj9NwQI34k+diVmJDXhRV4vbr+ONdVK8sjLXxOk40t5Y\r"
"nSaw/s7hPuaxf8+7e4zLxlWEbwGwToKVTSeFYsebLqKFbZDTf3IEJaF5NI8KYzoz\r"
"6gUIwsucfhr7yh/uFvqlGkntkMloaFMA2DLhf5Hx+Ijs6PYplDYtGIi7sww45sx5\r"
"PkEVlvqhAoGBAO2Q3xjXysEeyoPg1aVvxFqrO3mhX0Uldda8PsYcej/xgE8tyVCe\r"
"9pGPpgqhG5xAUJk3rJhfkLbOrolI1bNpdXSXwBjwgDGoVYYrDjhhFHyccbvqMvLP\r"
"FbpyhFx7175ZSXnKX/OIB3Y2SYHxoRYYGZWUwuIwii/JYgC5iZ5qgd1bAoGBAMWV\r"
"NF7An5nF89URPBDszBitHvEU38jWJH0mguDalVcxS9WnTaQWgCJzc54uCTHCfkCN\r"
"W4qSnOcK48CLwvtycTrMPQ1ItSkLzvJ1ZNR+pOOny+5jrM0ClAwxCzombVCOwMR8\r"
"ET6SrmherNWoyCpSoPYDdTrG+sPZ+OeUh1JSkzz1AoGBALGe569DaK0LwI7pw9N1\r"
"xXGlJUrDhN/GKlzrUmP9VsoIXs7UhPhqYiBjLtozqtkgnSJxpfInQaPs1EKA2obS\r"
"Cqep7k63QqHeIlO2TWOJ8i9ZKRA/AujYPH6ysJQVZDFFwNH2pdcHlcykukEV0EMc\r"
"scRM/YjwkeE4yLWSA3sWVxKRAoGBAKkDqgnHon8LCzpfBM/BkBEnvkkhvxBwxkPc\r"
"NqabtJYikClSdSMBMFjIA8XywWC0bAVSJlVSdy9YbFyf8YngaqWOYkdDw9w5wqw6\r"
"6aawMuKe/d6Nmxq/st7+8QisKGR5yMILE0FAfjq/if824wr5JcFsUdKWtZnlknqe\r"
"3mb4RgUlAoGAdXj1ePrO5BqkY79v1alIloB2a86bAXZBn0dP37u81rPGnxZMqFZ/\r"
"juvtbIaF6dIuoxoW2OswfJB5p/u0bZfnB/ju8xEcqFub71DtY1kYpshPEsZNMYGR\r"
"I4Kdg9PMeQ7AKSiVlF366pZWr0J0uIuYrFn1jAhIHNcGkgKJs4g+fTM=\r"
"-----END RSA PRIVATE KEY-----\r"
"\003";

char *certs[2][3] = {
    {mos_CA, mos_crt, mos_pri},
    {sim_CA, sim_crt, sim_pri}
};

int g_current_cert = 0;

void do_insert_certs(const struct shell *sh, size_t argc, char **argv)
{
    int cert = 0;
    char resp[100];

    if (argc > 1) {
        cert = strtoul(argv[1], NULL, 10);
    }
    if (cert>2 || argc != 2) {
        shell_error(sh, "Usage: %s <0-2>,  0=none (unencrypted/remove certs), 1= mosquitto.org, 2=aws staging", argv[0]);
        return;
    }

    g_current_cert = cert;
    if (cert == 0) {
        shell_print(sh, "Removing MQTT certs\r\n");
        safe_send_cmd(sh, "\eCERT,0,0,1\r\n", resp, 100);
        safe_send_cmd(sh, "\eCERT,0,1,1\r\n", resp, 100);
        safe_send_cmd(sh, "\eCERT,0,2,1\r\n", resp, 100);
        return;
    }

    cert -= 1;
    if (cert == 0) {
        shell_print(sh, "Inserting mosquitto.org certs\r\n");
    } else {
        shell_print(sh, "Inserting AWS staging certs\r\n");
    }
    for (int i=0;i<3;i++) {
        shell_print(sh, "Sending %25.25s\r\n", certs[cert][i]+1);
        safe_send_cmd(sh, certs[cert][i], resp, 100);
    }
}

bool g_time_set = false;
void do_set_time(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 6 || strlen(argv[1]) != 4 || strlen(argv[2]) != 2 || strlen(argv[3]) != 2 || strlen(argv[4]) != 2 || strlen(argv[5]) != 2) {
        shell_error(sh, "Usage: %s <YYYY> <MM> <DD> <HH> <mm>", argv[0]);
        return;
    }
    char *year = argv[1];
    char *month = argv[2];
    char *day = argv[3];
    char *hour = argv[4];
    char *minute = argv[5];
    char response[40];
    char timecmd[50];
    sprintf(timecmd, "AT+TIME=%s-%s-%s,%s:%s:00\r\n", year, month, day, hour, minute);

    int ret = safe_send_cmd(sh, timecmd, response, 40);
    if (ret < 2 || strncmp(response, "OK", 6) != 0) {
        shell_error(sh, "Error setting time from DA, response: %s", response);
    } else {
        g_time_set = true;
    }
}
void do_wifi_sleep(const struct shell *sh, size_t argc, char **argv)
{
    if( argc<2){
        shell_error(sh, "Usage: %s <millis>", argv[0]);
        return;
    }
    int millis = atoi(argv[1]);
    printf("sending command to sleep for %d milliseconds\n",millis);
    wifi_sleep(millis);
}
void do_send_mqtt_msg(const struct shell *sh, size_t argc, char **argv)
{
    char resp[100];
    char *broker_cmd, *tls_cmd; 
// EAS XXX check if connected

    if (g_time_set == false && g_current_cert != 0 ) {
        shell_error(sh, "encrypted mqtt certs need the time set, use 'da16200 set_time' first");
        return;
    }
    switch (g_current_cert) {
    case 0:
        broker_cmd = "AT+NWMQBR=test.mosquitto.org,1883\r\n";
        tls_cmd = "AT+NWMQTLS=0\r\n";
        break;
    case 1:
        broker_cmd = "AT+NWMQBR=test.mosquitto.org,8883\r\n";
        tls_cmd = "AT+NWMQTLS=1\r\n";
        break;
    case 2:
    default:
        broker_cmd = "AT+NWMQBR=a3hoon64f0fuap-ats.iot.eu-west-1.amazonaws.com,8883\r\n";
        tls_cmd = "AT+NWMQTLS=1\r\n";
        break;
    }

// alternate telemetry canned data
// {"P":4,"MID":"eas-mpb-test-001","MK":"US","B":35,"T":1,"M":{"TLM":[{"TID":50,"TS":1699488529,"TVI":24,"UUID":"a90d67a3-90a8-4c45-90b6-4c3558a6cee6"}]}}

        safe_send_cmd(sh, "AT+NWMQCL=0\r\n", resp, 100);
        safe_send_cmd(sh, broker_cmd, resp, 100);
        safe_send_cmd(sh, "AT+NWMQTP=messages/35/1/35_eas-mpb-test-001/d2c\r\n", resp, 100);
        safe_send_cmd(sh, "AT+NWMQCID=35_eas-mpb-test-001\r\n", resp, 100);
        safe_send_cmd(sh, tls_cmd, resp, 100);
        safe_send_cmd(sh, "AT+NWMQCL=1\r\n", resp, 100);
        k_busy_wait(2000000);  // Wait for the DA to establish the connection
        safe_send_cmd(sh, "AT+NWMQMSG='{\"P\":1,\"MID\":\"eas-mpb-test-001\",\"MK\":\"US\",\"B\":35,\"T\":22,\"M\":{\"N\":6030}}',messages/35/1/35_eas-mpb-test-001/d2c\r\n", resp, 100);
}

#if (CONFIG_USE_UART_TO_DA16200)
SHELL_CMD_REGISTER(wifi_at_passthru, NULL, "enable AT passthru mode to the DA16200 (deprecated, use 'da16200 uart_passthru')", do_wifi_ATPassthru_mode);
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_da16200,
    SHELL_CMD(reset, NULL, "reset the wifi", do_wifi_reset),
    SHELL_CMD(turn_off, NULL, "turn off the wifi", do_wifi_turn_off),
    SHELL_CMD(turn_on, NULL, "turn on the wifi", do_wifi_turn_on),
    SHELL_CMD(power_key, NULL, "set wifi power key line", do_wifi_power_key),
    SHELL_CMD(enable_3v3, NULL, "set wifi 3v3 enable line", do_wifi_set_3v3_enable),
    SHELL_CMD(get_ssids, NULL, "Get the SSIDS", do_wifi_scan_ssids),
    SHELL_CMD(insert_certs, NULL, "Send the CA,crt and private key in '<ESC>C' format to the DA, insert_certs <0-2>,  0=none (unencrypted/remove certs), 1= mosquitto.org, 2=aws staging", do_insert_certs),
    SHELL_CMD(set_time, NULL, "Set the time on the DA, needed for checking certs, set_time <YYYY> <MM> <DD> <HH> <mm>", do_set_time),
    SHELL_CMD(send_mqtt_msg, NULL, "Send a canned message a mqtt broker (based on which certs are installed)", do_send_mqtt_msg),
    SHELL_CMD(sleep, NULL, "Send a canned message a mqtt broker (based on which certs are installed)", do_wifi_sleep),
#if (CONFIG_USE_UART_TO_DA16200)
    SHELL_CMD(uart_passthru, NULL, "enable AT passthru mode to the DA16200", do_wifi_ATPassthru_mode),
#endif
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(da16200, &sub_da16200, "Commands to control the DA16200", NULL);
