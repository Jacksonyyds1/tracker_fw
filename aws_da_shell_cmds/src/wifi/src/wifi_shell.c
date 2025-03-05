#include "wifi.h"

#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <strings.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>

#define CHAR_1 0x18
#define CHAR_2 0x11
#define SEND_BUF_LEN 1024
char wifi_send_buf[SEND_BUF_LEN];
uint8_t wifi_send_buf_ptr = 0;

#define MAX_AT_BUFFER_SIZE 3100

LOG_MODULE_REGISTER(wifi_shell);

bool bypass_in_use = false;
extern const struct device *gpio_p1;
extern const struct device *gpio_p0;


// This will be called whenever we are in bypass mode and
// we receive a message from the DA16200
void wifi_shell_on_rx(wifi_msg_t *msg, void *user_data)
{
    if (bypass_in_use) {
        // if we are in bypass mode, then printing is all that is
        // done with the message so remove it from the queue.
        // If not just print message and leave it to be processed 
        // by whatever code normally does that.
        wifi_msg_t qmsg;
        while (wifi_recv(&qmsg, K_NO_WAIT) == 0) {
            printk("%s", qmsg.data);
            wifi_msg_free(&qmsg);
        }
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
		shell_print(sh, "Bypass started, press ctrl-x ctrl-q to escape");
        wifi_set_rx_cb(wifi_shell_on_rx, NULL);
	}

	shell_set_bypass(sh, bypass);

	return 0;
}


void wifi_shell_bypass_cb(const struct shell *sh, uint8_t *data, size_t len)
{
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
        wifi_set_rx_cb(NULL, NULL);
		wifi_set_bypass(sh, NULL);
        wifi_send_buf_ptr = 0;
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

    if (wifi_string_complete && strlen(wifi_send_buf) > 0) {
        wifi_send_timeout(wifi_send_buf, K_MSEC(1000));
        wifi_send_buf_ptr = 0;
        memset(wifi_send_buf, 0, SEND_BUF_LEN);
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


void do_wifi_set_wakeup(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) { 
        shell_print(sh, "Usage: wifi set_wakeup <1|0>\n");
        return;
    }
    int newState = atoi(argv[1]);
    wifi_set_wakeup(newState);
    shell_print(sh, "Wifi set wakeup line to: %d\n", newState);
}


void shell_print_ctl_n(const struct shell *sh, char *in_buf, int len)
{
    int i;
    for (i=0; i<len; i++) {
        if (in_buf[i] == 0) { 
            break;
        }
        if (in_buf[i] >= 20) {
            shell_fprintf(sh, SHELL_NORMAL, "%c", in_buf[i]);
        } else {
            shell_fprintf(sh, SHELL_NORMAL, "\\%1x", in_buf[i]);
        }
    }
}
void shell_print_ctl(const struct shell *sh, char *in_buf) {
    shell_print_ctl_n(sh, in_buf, strlen(in_buf));
}
// The size of this buffer needs to be larger then the 
// largest response the DA could send us.
static char at_response_buffer[MAX_AT_BUFFER_SIZE];
static int at_response_buffer_len = 0;

// This function will look in the at_response_buffer for
// an the first valid response. Which are:
//   a) <CR><LF>OK<CR><LF>
//   b) ...<CR><LF>OK<CR><LF>
//   c) <CR><LF>ERROR:<error code><CR><LF>
//   d) <CR><LF>+...<CR><LF>
//
// Returns:
//    the length of the response
//    or -1 if no complete response is found
bool printscan = false;
int scan_for_response(const struct shell *sh) 
{
    int i;
    bool end_on_next_crlf = false;
    bool end_on_next_crlfokcrlf = false;

    // The smallest response is 6 bytes
    if (at_response_buffer_len < 6) {
        return -1;
    }
    if (printscan) {
        shell_fprintf(sh, SHELL_NORMAL, "scan_for_response: (len %d) |", at_response_buffer_len);
        shell_print_ctl_n(sh, at_response_buffer, at_response_buffer_len);
        shell_print(sh, "|");
    }

    for (i=0; i<at_response_buffer_len-1 && i<MAX_AT_BUFFER_SIZE-1; i++) {
        if (at_response_buffer[i] == '\r' && at_response_buffer[i+1] == '\n') {
            // The following strings preceeds a variable length response ending 
            // at the next \r\n or \r\nOK\r\n
            if (!end_on_next_crlf && !end_on_next_crlfokcrlf && (
                strncasecmp(at_response_buffer+i, "\r\nERROR", 7) == 0 ||
                strncasecmp(at_response_buffer+i, "\r\n+INIT:", 8) == 0 ||
                strncasecmp(at_response_buffer+i, "\r\n+NWMQCL:", 10) == 0 ||
                strncasecmp(at_response_buffer+i, "\r\n+WFJAP:", 9) == 0 ||
                strncasecmp(at_response_buffer+i, "\r\n+NWMQMSGSND:", 10) == 0)) {
                if (printscan) {
                    shell_print(sh, "found var start at %d", i);
                }
                end_on_next_crlf = true;
                continue;
            }
            // A \r\n+... is the start of a variable response that can include \r\n.
            // The response ends with \r\nOK\r\n
            if (!end_on_next_crlf && !end_on_next_crlfokcrlf && 
                 strncasecmp(at_response_buffer+i, "\r\n+", 3) == 0) {
                if (printscan) {
                    shell_print(sh, "found + var start at %d", i);
                }
                end_on_next_crlfokcrlf = true;
                continue;
            }

            if (!end_on_next_crlf && 
                 strncasecmp(at_response_buffer+i, "\r\nOK\r\n",6) == 0) {
                if (printscan) {
                    shell_print(sh, "Found <CR><LF>OK<CR><LF> at %d", i);
                }
                return i+6;
            }
            if (!end_on_next_crlfokcrlf && end_on_next_crlf) {
                if (printscan) {
                    shell_print(sh, "Found var end at %d", i+1);
                }
                return i+1;
            }
        }
    }
    return -1;
}


// Adjust the at_response_buffer to make room for more data
// and read more data from wifi
// Returns the length of the data read
int prep_buffer_and_read(const struct shell *sh, int maxwaitms)
{
    int ret, len;
    wifi_msg_t msg;

    int buf_left = MAX_AT_BUFFER_SIZE - at_response_buffer_len;
    if (buf_left < WIFI_MSG_SIZE) {
        // We have no room left in the buffer, which should
        // not happen.  Make room and note the error
        int shrink_by = WIFI_MSG_SIZE - buf_left;
        len = WIFI_MSG_SIZE;
        memcpy(at_response_buffer, at_response_buffer+shrink_by, at_response_buffer_len-shrink_by);
        at_response_buffer_len -= shrink_by;
        buf_left += shrink_by;
        shell_error(sh, "Buffer overflow, discarding data");
    }

    // Read more data to to get a full response
    ret = wifi_recv(&msg, K_MSEC(maxwaitms));
    if (ret == 0) { // We read something
        memcpy(at_response_buffer+at_response_buffer_len, msg.data, msg.data_len);
        at_response_buffer_len += msg.data_len;
        if (at_response_buffer_len < MAX_AT_BUFFER_SIZE-1) {
            at_response_buffer[at_response_buffer_len] = 0;
        }
        wifi_msg_free(&msg);
        return msg.data_len;
    } 
    return 0;
}

// Scan the at_response_buffer for a complete response and
// copy it to the resp_buff
//    length read if response is found
//    0 if no response is found
//    -1 if error
int scan_and_copy_response(const struct shell *sh, char *resp_buff, int resp_len)
{
    int len = scan_for_response(sh);
    if (len <= 0) {
        return 0;
    }

    if (len < resp_len - 1) {
        memcpy(resp_buff, at_response_buffer, len);
        resp_buff[len] = 0;

        // Shift the remaining data to the front of the buffer
        memmove(at_response_buffer, at_response_buffer+len, at_response_buffer_len-len);
        at_response_buffer_len -= len;
        return len;
    } else {
        if (sh) {
            shell_error(sh, "Response bigger then buffer");
        }
        return -1;
    }
    return 0;
}


// Read until we get a valid AT response from the DA or we timeout
// Returns
//    length read if response is found
//    0 if no response is found
//    -1 if error
int get_at_response(const struct shell *sh, char *resp_buff, int resp_len, int maxwaitms)
{
    int len, ret;
    uint64_t start = k_uptime_get();
    int left = maxwaitms - (k_uptime_get()-start);
 
    memset(resp_buff, 0, resp_len);

    // In case we already have a response
    len = scan_and_copy_response(sh, resp_buff, resp_len);
    if (len != 0) {
        return len;
    }

    // if not, read for a response
    ret = prep_buffer_and_read(sh, left);
    while (1) {
        if (ret > 0) { // We read something
            len = scan_and_copy_response(sh, resp_buff, resp_len);
            if (len != 0) {
                return len;
            }
        }
        left = maxwaitms - (k_uptime_get()-start);
        if (left <= 0) {
            break;
        }
        ret = prep_buffer_and_read(sh, left);
    }

    return 0;
}


// Send a command to the DA16200 and wait response
// Returns 
//    length read if response is found
//    0 if no response is found
//    -1 if error
//
// recv_buf must be at least WIFI_MSG_SIZE in length
int send_cmd_to_da(const struct shell *sh, char *cmd, char *recv_buf, int recv_len, int maxwaitms)
{
    uint64_t start = k_uptime_get();
    int left = maxwaitms - (k_uptime_get()-start);

    if (sh) {
        shell_fprintf(sh, SHELL_NORMAL, "sending DA16200 |");
        shell_print_ctl(sh, cmd);
        shell_print(sh, "|");
    }
    wifi_send_timeout(cmd, K_MSEC(left));
    left = maxwaitms - (k_uptime_get()-start);

    int ret = get_at_response(sh, recv_buf, recv_len, left);
    if (sh) {
        if (ret > 0) {
            shell_fprintf(sh, SHELL_NORMAL, "DA16200 response to 'AT': %d |",ret);
            shell_print_ctl(sh, recv_buf);
            shell_print(sh, "|");
        } else if (ret == 0){
            shell_error(sh, "timed out waiting for response");
        }
    }
    return ret;
}

int clear_recv(const struct shell *sh, bool hide_ctl)
{
    int ret, total = 0;
    wifi_msg_t msg;

    at_response_buffer_len = 0;
    if (sh != NULL) {
        shell_fprintf(sh, SHELL_NORMAL, "Clearing wifi buffers: |");
    }
    for (int i=0;i<300;i++) {
        ret = wifi_recv(&msg, K_USEC(10));
        if (ret != 0) {
            break;
        }
        if (sh != NULL) {
            if (hide_ctl) {
                shell_print_ctl_n(sh, msg.data, msg.data_len);
            } else {
                shell_fprintf(sh, SHELL_NORMAL, "%s", msg.data);
            }
        }
        total += msg.data_len;
        wifi_msg_free(&msg);
    }

    if (sh != NULL) {
        shell_print(sh, "| len: %d", total);
    }
    return total;
}


// Send a command to the DA16200 and response and check for 'OK'
// Returns 
//    length read if response is found
//    0 if no response is found
//    -1 if error
bool send_cmd_check_OK(const struct shell *sh, char *cmd, int maxwaitms)
{
    static char incoming[500];
    int len = send_cmd_to_da(sh, cmd, incoming, 500, maxwaitms);
    if (len > 0 && strcmp(incoming+len-6, "\r\nOK\r\n") == 0) {
        return true;
    }
    return false;
}

// Send an 'AT' to the DA16200 and return if the response was 'OK'
bool check_for_at_response(const struct shell *sh, int maxwaitms)
{
    return send_cmd_check_OK(sh, "at", maxwaitms);
}



char mos_CA[] = "\eC0,"
"-----BEGIN CERTIFICATE-----\r\n"
"MIIEAzCCAuugAwIBAgIUBY1hlCGvdj4NhBXkZ/uLUZNILAwwDQYJKoZIhvcNAQEL\r\n"
"BQAwgZAxCzAJBgNVBAYTAkdCMRcwFQYDVQQIDA5Vbml0ZWQgS2luZ2RvbTEOMAwG\r\n"
"A1UEBwwFRGVyYnkxEjAQBgNVBAoMCU1vc3F1aXR0bzELMAkGA1UECwwCQ0ExFjAU\r\n"
"BgNVBAMMDW1vc3F1aXR0by5vcmcxHzAdBgkqhkiG9w0BCQEWEHJvZ2VyQGF0Y2hv\r\n"
"by5vcmcwHhcNMjAwNjA5MTEwNjM5WhcNMzAwNjA3MTEwNjM5WjCBkDELMAkGA1UE\r\n"
"BhMCR0IxFzAVBgNVBAgMDlVuaXRlZCBLaW5nZG9tMQ4wDAYDVQQHDAVEZXJieTES\r\n"
"MBAGA1UECgwJTW9zcXVpdHRvMQswCQYDVQQLDAJDQTEWMBQGA1UEAwwNbW9zcXVp\r\n"
"dHRvLm9yZzEfMB0GCSqGSIb3DQEJARYQcm9nZXJAYXRjaG9vLm9yZzCCASIwDQYJ\r\n"
"KoZIhvcNAQEBBQADggEPADCCAQoCggEBAME0HKmIzfTOwkKLT3THHe+ObdizamPg\r\n"
"UZmD64Tf3zJdNeYGYn4CEXbyP6fy3tWc8S2boW6dzrH8SdFf9uo320GJA9B7U1FW\r\n"
"Te3xda/Lm3JFfaHjkWw7jBwcauQZjpGINHapHRlpiCZsquAthOgxW9SgDgYlGzEA\r\n"
"s06pkEFiMw+qDfLo/sxFKB6vQlFekMeCymjLCbNwPJyqyhFmPWwio/PDMruBTzPH\r\n"
"3cioBnrJWKXc3OjXdLGFJOfj7pP0j/dr2LH72eSvv3PQQFl90CZPFhrCUcRHSSxo\r\n"
"E6yjGOdnz7f6PveLIB574kQORwt8ePn0yidrTC1ictikED3nHYhMUOUCAwEAAaNT\r\n"
"MFEwHQYDVR0OBBYEFPVV6xBUFPiGKDyo5V3+Hbh4N9YSMB8GA1UdIwQYMBaAFPVV\r\n"
"6xBUFPiGKDyo5V3+Hbh4N9YSMA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQEL\r\n"
"BQADggEBAGa9kS21N70ThM6/Hj9D7mbVxKLBjVWe2TPsGfbl3rEDfZ+OKRZ2j6AC\r\n"
"6r7jb4TZO3dzF2p6dgbrlU71Y/4K0TdzIjRj3cQ3KSm41JvUQ0hZ/c04iGDg/xWf\r\n"
"+pp58nfPAYwuerruPNWmlStWAXf0UTqRtg4hQDWBuUFDJTuWuuBvEXudz74eh/wK\r\n"
"sMwfu1HFvjy5Z0iMDU8PUDepjVolOCue9ashlS4EB5IECdSR2TItnAIiIwimx839\r\n"
"LdUdRudafMu5T5Xma182OC0/u/xRlEm+tvKGGmfFcN0piqVl8OrSPBgIlb+1IKJE\r\n"
"m/XriWr/Cq4h/JfB7NTsezVslgkBaoU=\r\n"
"-----END CERTIFICATE-----\r\n"
"\003";

char mos_crt[] = "\eC1,"
"-----BEGIN CERTIFICATE-----\r\n"
"MIIDwzCCAqugAwIBAgIBADANBgkqhkiG9w0BAQsFADCBkDELMAkGA1UEBhMCR0Ix\r\n"
"FzAVBgNVBAgMDlVuaXRlZCBLaW5nZG9tMQ4wDAYDVQQHDAVEZXJieTESMBAGA1UE\r\n"
"CgwJTW9zcXVpdHRvMQswCQYDVQQLDAJDQTEWMBQGA1UEAwwNbW9zcXVpdHRvLm9y\r\n"
"ZzEfMB0GCSqGSIb3DQEJARYQcm9nZXJAYXRjaG9vLm9yZzAeFw0yMzExMDMxOTE4\r\n"
"MjlaFw0yNDAyMDExOTE4MjlaMIGcMQswCQYDVQQGEwJVUzETMBEGA1UECAwKQ2Fs\r\n"
"aWZvcm5pYTEQMA4GA1UEBwwHT2FrbGFuZDEQMA4GA1UECgwHQ3VsdmVydDEVMBMG\r\n"
"A1UECwwMUHJvdG9Tb3JjZXJ5MREwDwYDVQQDDAhQcm90b09hazEqMCgGCSqGSIb3\r\n"
"DQEJARYbZXJpa0BjdWx2ZXJ0ZW5naW5lZXJpbmcuY29tMIIBIjANBgkqhkiG9w0B\r\n"
"AQEFAAOCAQ8AMIIBCgKCAQEArFy44XWp2i6pIwQSk0GczPnJYE0rM+oj8aZGWjPL\r\n"
"K2iw7e7C4THW9rnkrUpUDY91ZfVGuoxp0ZC//sDkZDOsbSvdEGzcbF16hioWaWuV\r\n"
"9IdXaj83U0rsdOO8umEpLrnS5Ri++LwqYGjzRVgYGb3HD9p1ak1KjLJessquG72n\r\n"
"aOwWGxJxVY4a2YN1XjwW6kaBCMSGVHRSNnm8blhuYVqI5sfsYTX/DQrq9rcR/ENJ\r\n"
"QBp2YMlK1kgNsdKnOFi2JPnyHb9j5r7PKLgplxsb7aE+V/viin0jcjW6NbPnJ7vR\r\n"
"VkpicoIrZ21xlTCuj2df1Kxe7+PtWNw9M1N1hGkXLaPx0QIDAQABoxowGDAJBgNV\r\n"
"HRMEAjAAMAsGA1UdDwQEAwIF4DANBgkqhkiG9w0BAQsFAAOCAQEAfDG3hSECmhzt\r\n"
"f1tIYPjFi5UfiS6Tvx7tTaOEmaQ3F5pyHLT3Xgun1ZqqoK9HRTy81AHKe4MxdC8M\r\n"
"wP0FHxGyTx76MMuZBHBjfu/GgdjUIini/yPiHGaujKCGvqjEPvutkFQ569ht4gdu\r\n"
"HUeJVK+yp/AYojrpgMmauTClm8DE5FoGw7ZuUaQFcpsXfg2Yt7Uc5zA/xXYFWOY4\r\n"
"f+FkcwujriRHw8Imtsqc5jbOsEe2LfCGXfOzObJrGleoZGeWT2rN2OkbHhsUAOLo\r\n"
"2MuLZzsgagsMsWYq04MlLMeeXDH3IpaORdoknNuo7JRzroJYbpBm45cNQX4ysf46\r\n"
"KVQVSPdmLQ==\r\n"
"-----END CERTIFICATE-----\r\n"
"\003";

char mos_pri[] = "\eC2,"
"-----BEGIN PRIVATE KEY-----\r\n"
"MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQCsXLjhdanaLqkj\r\n"
"BBKTQZzM+clgTSsz6iPxpkZaM8sraLDt7sLhMdb2ueStSlQNj3Vl9Ua6jGnRkL/+\r\n"
"wORkM6xtK90QbNxsXXqGKhZpa5X0h1dqPzdTSux047y6YSkuudLlGL74vCpgaPNF\r\n"
"WBgZvccP2nVqTUqMsl6yyq4bvado7BYbEnFVjhrZg3VePBbqRoEIxIZUdFI2ebxu\r\n"
"WG5hWojmx+xhNf8NCur2txH8Q0lAGnZgyUrWSA2x0qc4WLYk+fIdv2Pmvs8ouCmX\r\n"
"GxvtoT5X++KKfSNyNbo1s+cnu9FWSmJygitnbXGVMK6PZ1/UrF7v4+1Y3D0zU3WE\r\n"
"aRcto/HRAgMBAAECggEAE9dyBP+e4kSgKILDLKZ59B1bxt/l9Z+iLE2VfbQb/aRF\r\n"
"8dicIIEJ8PRsqbI2biqHO4nPxDvTwVHQzZ/LlZQJPmFf8mXFvh0eCgHK+1nCNRhm\r\n"
"F2StlsNiPavVwagxAyBVxxUL2ZBiWcowbwQkH2/O/Dk2w6+gFwWVjIQeejJhP4eg\r\n"
"EdaZ0LuCpZQPc581uAqj2lJJGSQ+Pmr/mepjtHaf1XAORF/cs/cDB+kJhriW2I1v\r\n"
"QSk3sYxSJnh6geFVEvNBbWS4nK5xoUT7RyV71MGNa9ItkTaqKmkMn5s3hxnGGXut\r\n"
"fk1HsppMEfyiJ7oWf3oatRy0jv1JqdlbsdZ26E/OrQKBgQDlDpkcpo+bz8bUTMNw\r\n"
"5++GwBMl0xaDvHl4xDmpumu1PSzusCSZZrEUbgBIXtVYZJ7xL6OgLvb4qq6TikKh\r\n"
"irvYy4bGTmoYbndpAVnSUGBSAc3c30BV/Oop85+bEFO5YshI93YOTgWP+8lgTHy+\r\n"
"4/g2A4JyJrMKdxSkhoAXuTRAlwKBgQDAouua/gMwT6AomFqpuM5+DrVbAUrqYXtn\r\n"
"1vrHRRf3QYDlOQ4DMbMmMCt6kjdnKiyiGAJzCND8I79dfoSZ27ALKRbxXIFtNHsH\r\n"
"ue94hhk0XPrcan9MQyhrWdlUMMoVpXht84cQOFdg3xD0DlAANEnmGjijKvMkzpRj\r\n"
"PEAib49F1wKBgGHmkmSfgCPdc6MLyED6sPLMJ6L0DNxzcwu9+tNjfWOyaQD/wjTa\r\n"
"oncT6QUFm3QzVYfKj8oIKMDx2rnuzznSXSV1H/6kR0538Iut6yEr/28tnDp6JTpb\r\n"
"Zg5WNXKGUPKcmPQu6IOGr3Px7wk8x9ijAVS8vUVi6wVfDjCf2CHLo9yzAoGAcmfP\r\n"
"2WsGZcjEa5egMLArIr6FgpjP70cZzV/l7DbittvWO0yZP9hid0mgaNkxwjlP7Kyp\r\n"
"t7wCsdxhKJudEOtiMB6lG48+5qaGct5AlKm/ilO2QPWWyKoR9T+VTOT0/8oYLeS1\r\n"
"0DJF4qhYHzno1VY4lUn5XR6C7NcrVYxQ4qKyyl0CgYBAxjkp5Nsxa14RMFyfEJOs\r\n"
"PBOxRocaFV+fKwRKOlYh6drc9bffhlNTHdUnwLgM8pyYvA9/RdT0kdMHrH9NTKse\r\n"
"9RbVYcL3gMOmPynT3M9Jvv3XsUoOqebSVOgw7Zo4fKPtskikjMHweZK4yLyDtjfl\r\n"
"+1IM6leVevuqFHeX38DaQw==\r\n"
"-----END PRIVATE KEY-----\r\n"
"\003";


char sim_CA_ats[] = "\eC0,"
"-----BEGIN CERTIFICATE-----\r\n"
"MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\r\n"
"ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6\r\n"
"b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL\r\n"
"MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv\r\n"
"b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj\r\n"
"ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM\r\n"
"9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw\r\n"
"IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6\r\n"
"VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L\r\n"
"93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm\r\n"
"jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\r\n"
"AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA\r\n"
"A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI\r\n"
"U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs\r\n"
"N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv\r\n"
"o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU\r\n"
"5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy\r\n"
"rqXRfboQnoZsG4q5WTP468SQvvG5\r\n"
"-----END CERTIFICATE-----\r\n"
"\003";

char sim_crt[] = "\eC1,"
"-----BEGIN CERTIFICATE-----\r\n"
"MIIDWTCCAkGgAwIBAgIUPW8wOE/NoIv+GXxNqkxUNSYjs34wDQYJKoZIhvcNAQEL\r\n"
"BQAwTTFLMEkGA1UECwxCQW1hem9uIFdlYiBTZXJ2aWNlcyBPPUFtYXpvbi5jb20g\r\n"
"SW5jLiBMPVNlYXR0bGUgU1Q9V2FzaGluZ3RvbiBDPVVTMB4XDTIzMDUyNTA2MjI0\r\n"
"OFoXDTQ5MTIzMTIzNTk1OVowHjEcMBoGA1UEAwwTQVdTIElvVCBDZXJ0aWZpY2F0\r\n"
"ZTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALda8ZlSyFmT/s5hX0Iv\r\n"
"soeOsiXMhjY94M2BJn/IDIUtpUJJoocWhCMml2e7e94PPNZXSMvDfVR9YxiVb2My\r\n"
"QBjcXO5Nrim2nrTpkFIarCxSESD9k8/YzTkjB2Rn4XCkJX6xa1kT4cXTYVK049SS\r\n"
"E/53u59fFQ4AsHkJzQy0aX68MlGgmkMcr+5eazgvSaULR4hzyZgReiGt4jPhtPog\r\n"
"tFJNPxQjflt6DjK/QO/aeWAtkNTHBA/RYVD0kmALJJi1lnX07q+d7F6P9yCG/W4C\r\n"
"IJNXSsWT9IOklNn8bTTFseskXZpI3JcWc1YSRlLmjgh2dlS65jjL4392oNZ7MZ9r\r\n"
"LBcCAwEAAaNgMF4wHwYDVR0jBBgwFoAUyI/4x7eLGEToVgJKsiHjniv3lGMwHQYD\r\n"
"VR0OBBYEFLgGn2DBCRh4W9UDVnqLYVTG+cVSMAwGA1UdEwEB/wQCMAAwDgYDVR0P\r\n"
"AQH/BAQDAgeAMA0GCSqGSIb3DQEBCwUAA4IBAQCdWfxLCgzD7/iVpri5RTcgq1PE\r\n"
"PDNKPQm8xDqsj3yfjnOpBeROyO3/ozwkEP9KhqjC8KBP8LMvh0Am+EWrXDi9Nbe3\r\n"
"J3t5YmYLpjqR1BvMLYJxydKRax/BnB1aVHlScT9HG6qk2+lhXsTdf7YfinXPBDw/\r\n"
"DA57xADGxxocxLyFzRMnIPpPdZ/3RYqoUHyxAM+wJPcOxaWuZqPDBSYXvNFRFy+s\r\n"
"K3hRpw4AHlOCojA7n9f2PDFpuBiUfLkoRkDS3s1F9C2maUtHnYZIfU3hf85VmRui\r\n"
"S2cUjRk76GDSyvUpr2bM4JLicTosvBHiJtChwoubwYPtiEY0ifkYx0hYe3Zw\r\n"
"-----END CERTIFICATE-----\r\n"
"\003";

char sim_pri[] = "\eC2,"
"-----BEGIN RSA PRIVATE KEY-----\r\n"
"MIIEpQIBAAKCAQEAt1rxmVLIWZP+zmFfQi+yh46yJcyGNj3gzYEmf8gMhS2lQkmi\r\n"
"hxaEIyaXZ7t73g881ldIy8N9VH1jGJVvYzJAGNxc7k2uKbaetOmQUhqsLFIRIP2T\r\n"
"z9jNOSMHZGfhcKQlfrFrWRPhxdNhUrTj1JIT/ne7n18VDgCweQnNDLRpfrwyUaCa\r\n"
"Qxyv7l5rOC9JpQtHiHPJmBF6Ia3iM+G0+iC0Uk0/FCN+W3oOMr9A79p5YC2Q1McE\r\n"
"D9FhUPSSYAskmLWWdfTur53sXo/3IIb9bgIgk1dKxZP0g6SU2fxtNMWx6yRdmkjc\r\n"
"lxZzVhJGUuaOCHZ2VLrmOMvjf3ag1nsxn2ssFwIDAQABAoIBAQCnj+EC8XhPBMTz\r\n"
"7mCTp+tLnsiHaqWspFfw9noshLGMc+526bwyIA2Z4gazsc69XMeISjQoovrCX+RT\r\n"
"7xzgVmflUF1NGohzboUTZ++QWPfHeShWMecHJ2ZFNRHoXFbWDeyGH7WurlDB7S8f\r\n"
"2lfrR6QmBV3dg5NGPLMJqj9NwQI34k+diVmJDXhRV4vbr+ONdVK8sjLXxOk40t5Y\r\n"
"nSaw/s7hPuaxf8+7e4zLxlWEbwGwToKVTSeFYsebLqKFbZDTf3IEJaF5NI8KYzoz\r\n"
"6gUIwsucfhr7yh/uFvqlGkntkMloaFMA2DLhf5Hx+Ijs6PYplDYtGIi7sww45sx5\r\n"
"PkEVlvqhAoGBAO2Q3xjXysEeyoPg1aVvxFqrO3mhX0Uldda8PsYcej/xgE8tyVCe\r\n"
"9pGPpgqhG5xAUJk3rJhfkLbOrolI1bNpdXSXwBjwgDGoVYYrDjhhFHyccbvqMvLP\r\n"
"FbpyhFx7175ZSXnKX/OIB3Y2SYHxoRYYGZWUwuIwii/JYgC5iZ5qgd1bAoGBAMWV\r\n"
"NF7An5nF89URPBDszBitHvEU38jWJH0mguDalVcxS9WnTaQWgCJzc54uCTHCfkCN\r\n"
"W4qSnOcK48CLwvtycTrMPQ1ItSkLzvJ1ZNR+pOOny+5jrM0ClAwxCzombVCOwMR8\r\n"
"ET6SrmherNWoyCpSoPYDdTrG+sPZ+OeUh1JSkzz1AoGBALGe569DaK0LwI7pw9N1\r\n"
"xXGlJUrDhN/GKlzrUmP9VsoIXs7UhPhqYiBjLtozqtkgnSJxpfInQaPs1EKA2obS\r\n"
"Cqep7k63QqHeIlO2TWOJ8i9ZKRA/AujYPH6ysJQVZDFFwNH2pdcHlcykukEV0EMc\r\n"
"scRM/YjwkeE4yLWSA3sWVxKRAoGBAKkDqgnHon8LCzpfBM/BkBEnvkkhvxBwxkPc\r\n"
"NqabtJYikClSdSMBMFjIA8XywWC0bAVSJlVSdy9YbFyf8YngaqWOYkdDw9w5wqw6\r\n"
"6aawMuKe/d6Nmxq/st7+8QisKGR5yMILE0FAfjq/if824wr5JcFsUdKWtZnlknqe\r\n"
"3mb4RgUlAoGAdXj1ePrO5BqkY79v1alIloB2a86bAXZBn0dP37u81rPGnxZMqFZ/\r\n"
"juvtbIaF6dIuoxoW2OswfJB5p/u0bZfnB/ju8xEcqFub71DtY1kYpshPEsZNMYGR\r\n"
"I4Kdg9PMeQ7AKSiVlF366pZWr0J0uIuYrFn1jAhIHNcGkgKJs4g+fTM=\r\n"
"-----END RSA PRIVATE KEY-----\r\n"
"\003";

char *certs[3][3] = {
    {mos_CA, mos_crt, mos_pri},
    {sim_CA_ats, sim_crt, sim_pri}
};

int g_current_cert = 0;

void do_insert_certs(const struct shell *sh, size_t argc, char **argv)
{
    int cert = 0;

    if (argc > 1) {
        cert = strtoul(argv[1], NULL, 10);
    }
    if (cert>2 || argc != 2) {
        shell_error(sh, "Usage: %s <0-2>,  0=none (unencrypted/remove certs), 1= mosquitto.org, 2=aws staging", argv[0]);
        return;
    }

    clear_recv(sh, true);
    if (!check_for_at_response(sh, 1000)) {
        return;
    }

    g_current_cert = cert;
    if (cert == 0) {
        shell_print(sh, "Removing MQTT certs");
        if (!send_cmd_check_OK(sh, "\eCERT,0,0,1", 1000)) {
            return;
        }
        if (!send_cmd_check_OK(sh, "\eCERT,0,1,1", 1000)) {
            return;
        }
        if (!send_cmd_check_OK(sh, "\eCERT,0,2,1", 1000)) {
            return;
        }
        return;
    }

    cert -= 1;
    if (cert == 0) {
        shell_print(sh, "Inserting mosquitto.org certs");
    } else {
        shell_print(sh, "Inserting AWS staging certs");
    }
    for (int i=0;i<3;i++) {
        shell_print(sh, "Sending %25.25s", certs[cert][i]+1);
        if (!send_cmd_check_OK(sh, certs[cert][i], 1000)) {
            return;
        }
    }
}

bool g_time_set = false;
void do_set_time(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 6 || strlen(argv[1]) != 4 || strlen(argv[2]) != 2 || strlen(argv[3]) != 2 || strlen(argv[4]) != 2 || strlen(argv[5]) != 2) {
        shell_error(sh, "Usage: %s <YYYY> <MM> <DD> <HH> <mm>", argv[0]);
        return;
    }

    clear_recv(sh, true);
    if (!check_for_at_response(sh, 1000)) {
        return;
    }

    char *year = argv[1];
    char *month = argv[2];
    char *day = argv[3];
    char *hour = argv[4];
    char *minute = argv[5];
    char timecmd[50];
    sprintf(timecmd, "AT+TIME=%s-%s-%s,%s:%s:00", year, month, day, hour, minute);

    if (!send_cmd_check_OK(sh, timecmd, 1000)) {
        return;
    }
    g_time_set = true;
}

static char com_buf[3000];
void do_send_mqtt_msg(const struct shell *sh, size_t argc, char **argv)
{
    char *broker_cmd, *tls_cmd, *sub_topic; 
    int len;

    if (argc != 2) {
        shell_error(sh, "Usage: %s <bytes to send>", argv[0]);
        return;
    }
    int amount = strtoul(argv[1], NULL, 10);

    clear_recv(sh, true);
    if (!check_for_at_response(sh, 1000)) {
        return;
    }

    if (!send_cmd_check_OK(sh, "ATZ", 1000)) {
        shell_error(sh, "Could not reset AT state.");
        return;
    }

    if (!send_cmd_check_OK(sh, "AT+WFSTAT", 3000)) {
        shell_error(sh, "Not connected to an AP.");
        return;
    }

    if (g_time_set == false && g_current_cert != 0 ) {
        shell_error(sh, "encrypted mqtt certs need the time set, use 'da16200 set_time' first");
        return;
    }
    shell_print(sh, "Configuring MQTT");
    switch (g_current_cert) {
    case 0:
        shell_print(sh, "Connecting to unencrypted mosquitto");
        broker_cmd = "AT+NWMQBR=test.mosquitto.org,1883";
        tls_cmd = "AT+NWMQTLS=0";
        sub_topic = "messages/35/10/35_eas-mpb-test-001/d2c";
        break;
    case 1:
        shell_print(sh, "Connecting to encrypted mosquitto");
        broker_cmd = "AT+NWMQBR=test.mosquitto.org,8883";
        tls_cmd = "AT+NWMQTLS=1";
        sub_topic = "messages/35/10/35_eas-mpb-test-001/d2c";
        break;
    case 2:
    default:
        shell_print(sh, "Connecting to encrypted staging");
        broker_cmd = "AT+NWMQBR=a3hoon64f0fuap-ats.iot.eu-west-1.amazonaws.com,8883";
        tls_cmd = "AT+NWMQTLS=1";
        sub_topic = "messages/35/10/35_eas-mpb-test-001/c2d";
        break;
    }

    if ((len = send_cmd_to_da(sh, "AT+NWMQCL=0", com_buf, 1000, 2000)) <= 0){
        shell_error(sh, "Error Stopping MQTT service %d |%s|", len, com_buf);
        return;
    }
    // We may get back an OK or a +NWMQCL:0 event first
    // if we get back the OK first then the service was already stopped so don't 
    // expect the event
    if (strstr(com_buf, "+NWMQCL:0") != NULL) {
        if ((len = get_at_response(sh, com_buf, 2000, 7000)) <= 0) {
            shell_error(sh, "Error Stopping MQTT service %d |%s|", len, com_buf);
            return;
        }
    }
    if (strstr(com_buf, "OK") == NULL) {
        shell_error(sh, "Got unexpected response %d |%s|", len, com_buf);
        return;
    }

    if (!send_cmd_check_OK(sh, broker_cmd, 1000)) {
        shell_error(sh, "Error Setting the MQTT broker");
        return;
    }

    if (!send_cmd_check_OK(sh, "AT+NWMQTP=messages/35/10/35_eas-mpb-test-001/d2c", 1000)) {
        shell_error(sh, "Error Setting the default publish topic");
        return;
    }

    sprintf(com_buf, "AT+NWMQTS=1,%s", sub_topic);
    if (!send_cmd_check_OK(sh, com_buf, 1000)) {
        shell_error(sh, "Error Setting the list of subscribe topics");
        return;
    }

    if (!send_cmd_check_OK(sh, "AT+NWMQCID=35_eas-mpb-test-001", 1000)) {
        shell_error(sh, "Error Setting the client id");
        return;
    }

    if (!send_cmd_check_OK(sh, "AT+NWMQCS=1", 1000)) {
        shell_error(sh, "Error Setting the clean session flag");
        return;
    }

    if (!send_cmd_check_OK(sh, tls_cmd, 1000)) {
        shell_error(sh, "Error setting the TLS flag");
        return;
    }

    if (!send_cmd_check_OK(sh,  "AT+NWMQCL=1", 1000)) {
        shell_error(sh, "Error Starting the MQTT Broker");
        return;
    }

    // Wait for the connection to be established
    shell_print(sh, "Waiting for broker to connect");
    len = get_at_response(sh, com_buf, 2000, 7000);
    if (len > 0 && strstr(com_buf, "+NWMQCL:1") != NULL) {
        shell_print(sh, "Broker connected");
    } else {
        shell_fprintf(sh, SHELL_NORMAL, "Broker didn't connect, |");
        shell_print_ctl(sh, com_buf);
        shell_print(sh, "|");
        return;
    }

    // Construct the publish message
    // We can only send message of < 255 to the broker. so we send several messages
    // to make up the total amount
    if (amount < 200) {
        amount = 200;
    }
    int num_messages = amount / 200;
    int body_size = (amount / num_messages)-142;


    for (int msg=0; msg<num_messages; msg++) {

        sprintf(com_buf, "AT+NWMQMSG='{\"P\": 2, \"MID\": \"eas-mpb-test-001\","
                        " \"MK\": \"US\", \"B\": 35, \"T\": 10, \"M\": {\"S\": \"Msg#_%d_",msg);  // Header is about 96 char
        for (int f=0;f<body_size;f++) {
            uint8_t byte = rand() % 256;
            char hex[4];
            sprintf(hex, "%x", byte);
            strcat(com_buf, hex);
        }
        strcat(com_buf, "\"}}',messages/35/10/35_eas-mpb-test-001/d2c");    // footer is about 45 char

        // Send to broker
        if (!send_cmd_check_OK(sh,  com_buf, 3000)) {
            shell_error(sh, "Error publishing a connectivity test messasge");
            return;
        }
        shell_print(sh, "Waiting for confirmation we sent the message successfully");
        len = get_at_response(sh, com_buf, 1000, 4000);
        if (len <= 0 || strstr(com_buf, "+NWMQMSGSND:1") == NULL) {
            shell_error(sh, "Failed to get a response on the subscription");
            clear_recv(sh, true);
            return;
        }

        shell_print(sh, "Waiting for connectivity message to come back through subscription");
        len = get_at_response(sh, com_buf, 1000, 20000);
        if (len <= 0 || strstr(com_buf, "+NWMQMSG:") == NULL) {
            shell_error(sh, "Failed to get a response on the subscription");
            return;
        } else {
            shell_print(sh, "SUCCESS! message came back through subscription");
            shell_print_ctl(sh, com_buf);
            shell_print(sh, "");
        }
    }

    k_msleep(1000);
    shell_print(sh, "Stopping the MQTT client");
    if ((len = send_cmd_to_da(sh, "AT+NWMQCL=0", com_buf, 1000, 2000)) <= 0){
        shell_error(sh, "Error Stopping MQTT service %d |%s|", len, com_buf);
        return;
    }
    // We may get back an OK or a +NWMQCL:0 event first
    // if we get back the OK first then the service was already stopped so don't 
    // expect the event
    if (strstr(com_buf, "+NWMQCL:0") != NULL) {
        if ((len = get_at_response(sh, com_buf, 2000, 7000)) <= 0) {
            shell_error(sh, "Error Stopping MQTT service %d |%s|", len, com_buf);
            return;
        }
    }
    if (strstr(com_buf, "OK") == NULL) {
        shell_error(sh, "Got unexpected response %d |%s|", len, com_buf);
        return;
    }
    if (send_cmd_to_da(sh, "AT+NWMQCL=0", com_buf, 1000, 2000) <= 0){
        shell_error(sh, "Error Stopping MQTT service |%s|", com_buf);
        return;
    }
}



void do_power_test(const struct shell *sh, size_t argc, char **argv)
{
    int len,j;

    if (argc != 2) {
        shell_error(sh, "Usage: %s <bytes to send>", argv[0]);
        return;
    }
    clear_recv(sh, true);
    // Make sure the level shifter is powered on
    gpio_pin_set(gpio_p1, 14, 1);

    // Send an AT command and see if we get a response, if not we may already be in DPM mode
    shell_print(sh, "Sending an AT to see if da responds");
    if (check_for_at_response(sh, 1000)) {
        shell_print(sh, "We got a response to an AT command, so we aren't in DPM mode, putting into DPM mode");
        if (!send_cmd_check_OK(sh, "AT+DPM=1", 250)) {
            shell_error(sh, "Failed to set DPM mode");
            return;
        }
        len = get_at_response(sh, com_buf, 250, 1000);
        if (len > 0) {
            if (strstr(com_buf, "INIT") != NULL) {
                shell_print(sh, "Got a response to the DPM command, so we are in DPM mode");
            } else {
                shell_fprintf(sh, SHELL_NORMAL, "Got unexpected response to the DPM command, |");
                shell_print_ctl(sh, com_buf);
                shell_print(sh, "|");
                return;
            }
        } else {
            shell_error(sh, "Failed to get a response to the DPM command");
            return;
        }
    }

    for (int i=0;i<5;i++) {
        // try to wake up the DA
        shell_print(sh, "Setting WAKEUP_RTC to 1");
        gpio_pin_set(gpio_p1, 8, 0);
        k_sleep(K_MSEC(100));
        gpio_pin_set(gpio_p1, 8, 1);
        k_sleep(K_MSEC(100));
        gpio_pin_set(gpio_p1, 8, 0);
    
        // Now look for a "+INIT:WAKEUP," message to confirm it is in DPM mode
        shell_print(sh, "Waiting for response to wake up");
        for (j=0; j<8; j++) {
            len = get_at_response(sh, com_buf, 1000, 1000);
            if (len > 0) {
                if (strstr(com_buf, "INIT:WAKEUP") != NULL) {
                    shell_print(sh, "Got a response to the DPM command");
                    break;
                } else {
                    shell_fprintf(sh, SHELL_NORMAL, "Didn;'t get an event yet: |");
                    shell_print_ctl(sh, com_buf);
                    shell_print(sh, "|");
                    continue;
                }
            }
        }
        if (j==5) { // We never got a wakeup
            shell_fprintf(sh, SHELL_NORMAL, "Got unexpected response, |");
            return;
        }

        shell_print(sh, "Telling DA we want to talk to it AT+MCUWUDONE");
        len = send_cmd_to_da(sh, "AT+MCUWUDONE", com_buf, 1000, 1000);
        for (j=0; j<8; j++) {
            if (len > 0) {
                if (strstr(com_buf, "OK") != NULL) {
                    shell_print(sh, "Got a ok response");
                    break;
                } else {
                    shell_fprintf(sh, SHELL_NORMAL, "Didn;'t get an ok yet: |");
                    shell_print_ctl(sh, com_buf);
                    shell_print(sh, "|");
                }
            }
            len = get_at_response(sh, com_buf, 1000, 1000);
        }

        shell_print(sh, "Telling DA not to go back into DPM mode with AT+CLRDPMSLPEXT");
        len = send_cmd_to_da(sh, "AT+CLRDPMSLPEXT", com_buf, 1000, 1000);
        for (j=0; j<8; j++) {
            if (len > 0) {
                if (strstr(com_buf, "OK") != NULL) {
                    shell_print(sh, "Got a ok response");
                    break;
                } else {
                    shell_fprintf(sh, SHELL_NORMAL, "Didn;'t get an ok yet: |");
                    shell_print_ctl(sh, com_buf);
                    shell_print(sh, "|");
                }
            }
            len = get_at_response(sh, com_buf, 1000, 1000);
        }
        
        do_send_mqtt_msg(sh, argc, argv);


        shell_print(sh, "going back into dpm");
        if (!send_cmd_check_OK(sh, "AT+DPM=1", 250)) {
            shell_error(sh, "Failed to set DPM mode");
            return;
        }

        shell_print(sh, "Waiting a 55 seconds before doing it again");
        uint64_t now = k_uptime_get();
        while (k_uptime_get() < now + 55000) {
            k_msleep(500);
            clear_recv(sh, false);
        }
    }
}


void do_set_dpm_mode(const struct shell *sh, size_t argc, char **argv)
{
    bool set_off = true;
    int len,j;

    if (argc != 2) {
        shell_error(sh, "Usage: %s <on|off>", argv[0]);
        return;
    }
    if (strcasecmp(argv[1], "on") == 0) {
        set_off = false;
    }   
    // Make sure the level shifter is powered on
    gpio_pin_set(gpio_p1, 14, 1);

    clear_recv(sh, true);

    // Send an AT command and see if we get a response, if not we may already be in DPM mode
    shell_print(sh, "Sending an AT to see if da responds");
    if (check_for_at_response(sh, 250)) {
        if (set_off) {
            shell_print(sh, "We got a response to an AT command, so we aren't in DPM mode");
            shell_print(sh, "done");
            return;
        }
        shell_print(sh, "Telling DA to turn on DPM mode");
        gpio_pin_set(gpio_p1, 8, 0);    // Turn off wakeup, triggered on falling edge
        if (!send_cmd_check_OK(sh, "AT+DPM=1", 1000)) {
            shell_fprintf(sh, SHELL_NORMAL, "DA didn't respond, we got back: |");
            shell_print_ctl(sh, com_buf);
            shell_print(sh, "|");
            return;
        }
        shell_print(sh, "done");
        return;
    } else { 
        if (set_off == false) {
            shell_print(sh, "We didn't get a response to an AT command, so are in DPM mode");
            shell_print(sh, "done");
            return;
        }
        // try to wake up the DA via gpio
        shell_print(sh, "Setting WAKEUP_RTC to 1");
        gpio_pin_set(gpio_p1, 8, 0);    // Make sure wakeup is low
        k_sleep(K_MSEC(100));
        gpio_pin_set(gpio_p1, 8, 1);    // Pulse high
        k_sleep(K_MSEC(100));
        gpio_pin_set(gpio_p1, 8, 0);    // Turn off wakeup, triggered on falling edge
    
        // Now look for a "+INIT:WAKEUP," message to confirm it is in DPM mode
        shell_print(sh, "Waiting for wake up event");
        for (j=0; j<5; j++) {
            len = get_at_response(sh, com_buf, 1000, 1000);
            if (len > 0) {
                if (strstr(com_buf, "INIT:WAKEUP") != NULL) {
                    // Don't print here, timing is important
                    break;
                } else {
                    continue;
                }
            }
        }
        if (j==5) { // We never got a wakeup
            shell_fprintf(sh, SHELL_NORMAL, "Never got a wake up event");
            return;
        }

        shell_print(sh, "Telling DA that the MCU is ready to talk to it with AT+MCUWUDONE");
        if (!send_cmd_check_OK(sh, "AT+MCUWUDONE", 250)) {
            shell_fprintf(sh, SHELL_NORMAL, "DA didn't respond, we got back: |");
            shell_print_ctl(sh, com_buf);
            shell_print(sh, "|");
            return;
        }
        shell_print(sh, "Telling DA to turn off DPM mode");
        if (!send_cmd_check_OK(sh, "AT+DPM=0", 1000)) {
            shell_fprintf(sh, SHELL_NORMAL, "DA didn't respond2, we got back: |");
            shell_print_ctl(sh, com_buf);
            shell_print(sh, "|");
            return;
        }
        shell_print(sh, "done");
    }
}

void do_ota(const struct shell *sh, size_t argc, char **argv)
{
    int len,errors = 0,stalls = 0;
    static char cmd[200];

    if (argc != 3) {
        shell_error(sh, "Usage: %s <https://server:port> <filename>", argv[0]);
        return;
    }
    clear_recv(sh, true);

    shell_print(sh, "Stopping MQTT");
    if ((len = send_cmd_to_da(sh, "AT+NWMQCL=0", com_buf, 1000, 2000)) <= 0){
        shell_error(sh, "Error Stopping MQTT service %d |%s|", len, com_buf);
        return;
    }

    // Send an AT command and see if we get a response, if not we may already be in DPM mode
    // AT+NWOTADWSTART=rtos,http://10.1.91.195:9000/DA16200_FRTOS-GEN01-01-UNTRACKED!-231130.img
    sprintf(cmd, "AT+NWOTADWSTART=rtos,%s/%s", argv[1], argv[2]);
    shell_print(sh, "Sending an AT command to download firmware: %s", cmd);
    if ((len = send_cmd_to_da(sh, cmd, com_buf, 1000, 2000)) <= 0){
        if (strstr(com_buf, "OK") != NULL) {
            shell_print(sh, "Got a ok response");
        } else {
            shell_fprintf(sh, SHELL_NORMAL, "Didn;'t get an ok: |");
            shell_print_ctl(sh, com_buf);
            shell_print(sh, "|");
            return;
        }
    }

    // Now look for a "+NWOTADWSTART:0x00" message to confirm fw was downloaded,it can take time
    // Normally we could check for progress using the AT+NWOTADWPROG=rtos cmd but it seems to crash the da
    shell_print(sh, "Waiting for download confirm event");
    bool download_complete = false;
    int32_t amount, last = -1;
    while (errors<5) {
        len = send_cmd_to_da(NULL, "AT+NWOTADWPROG=rtos", com_buf, 1000, 3000);
        if (len <= 0){
            shell_fprintf(sh, SHELL_NORMAL, "timeout in progress ask, aborting. we got back: |");
            shell_print_ctl(sh, com_buf);
            shell_print(sh, "|");
            return;
        }
        char *status = strstr(com_buf, "+NWOTADWPROG:");
        if (status == NULL) {
            shell_error(sh, "Unexpected response, |%s|",com_buf);
            errors++;
            continue;
        }
        amount = strtoul(status+13, NULL, 10);
        if(errno == ERANGE) {
            shell_error(sh, "Error parsing progress, |%s|",status);
            errors++;
            continue;
        }
        if (amount >= 100) {
            shell_print(sh, "Firmware download complete");
            download_complete = true;
            break;
        }
        if (amount <= last) {
            stalls++;
            if (stalls > 20) {
                shell_error(sh, "Firmware download stalled");
                return;
            }
            shell_fprintf(sh, SHELL_NORMAL, "!");
            continue;
        } else {
            shell_fprintf(sh, SHELL_NORMAL, "%d", amount);
        }
        stalls = 0;
        last = amount;
        shell_fprintf(sh, SHELL_NORMAL, ".");
        k_msleep(500);
    }

    if (download_complete == false) { 
        shell_fprintf(sh, SHELL_NORMAL, "Never got a firmware download confirmation");
        return;
    }

    shell_print(sh, "Sending an AT command to reboot with new firmware:");
    if (!send_cmd_check_OK(sh, "AT+NWOTARENEW", 1000)) {
        shell_error(sh, "failed at+renew");
        return;
    }
}

char sh_buf[WIFI_MSG_SIZE];
void do_flush(const struct shell *sh, size_t argc, char **argv) 
{
    clear_recv(sh, true);   // Flush receive buffer
}

void do_send_atcmd(const struct shell *sh, size_t argc, char **argv)
{
    int ret = 0;
    wifi_msg_t msg;
    int printctl = 0;

    if (argc != 2 && argc != 3) {
        shell_error(sh, "Usage: %s <cmd> [display returns yes/no]", argv[0]);
        return;
    }
    if (argc == 3 && strcasecmp(argv[2], "yes") == 0) {
        printctl = 1;
    }

    clear_recv(sh, true);   // Flush receive buffer

    ret = wifi_send_timeout(argv[1], K_NO_WAIT);
    if (ret != 0) {
        shell_error(sh, "Failed to send spi %d", ret);
        return;
    }

    if ((ret = wifi_recv(&msg, K_MSEC(3000))) != 0) {
        shell_error(sh, "Didn't receive a response %d", ret);
        return;
    }

    shell_fprintf(sh, SHELL_NORMAL, "Got a response, len %d |", msg.data_len);
    if (printctl) {
        shell_fprintf(sh, SHELL_NORMAL, "%s", msg.data);
    } else {
        shell_print_ctl(sh, msg.data);
    } 
    shell_print(sh, "|");
    wifi_msg_free(&msg);
}


void do_test_get_da_fw_ver(const struct shell *sh, size_t argc, char **argv)
{
    char ver[60];
    int ret = get_da_fw_ver(ver, 60);
    if (ret != 0) {
        shell_error(sh, "Failed to get fw ver");
    }   else {
        shell_print(sh, "FW version is %s", ver);
    }
}

void do_test_get_wfscan(const struct shell *sh, size_t argc, char **argv)
{
    char ver[800];
    int ret = get_wfscan(ver, 800);
    if (ret != 0) {
        shell_error(sh, "Failed to get wfscan");
    }   else {
        shell_print(sh, "wfscan:\r\n%s", ver);
    }
}

void do_test_connect_ssid(const struct shell *sh, size_t argc, char **argv)
{
    if (argc != 7) {
        shell_error(sh, "Usage: %s ssid key sec keyidx enc hidden ", argv[0]);
        shell_error(sh, "  <ssid>: SSID. 1 ~ 32 characters are allowed");
        shell_error(sh, "  <key>: Passphrase. 8 ~ 63 characters are allowed   or NULL if sec is 0 or 5");
        shell_error(sh, "  <sec>: Security protocol. 0 (OPEN), 1 (WEP), 2 (WPA), 3 (WPA2), 4 (WPA+WPA2) ), 5 (WPA3 OWE), 6 (WPA3 SAE), 7 (WPA2 RSN & WPA3 SAE)");
        shell_error(sh, "  <keyidx>: Key index for WEP. 0~3    ignored if sec is 0,2-7");
        shell_error(sh, "  <enc>: Encryption. 0 (TKIP), 1 (AES), 2 (TKIP+AES)   ignored if sec is 0,1 or 5");
        shell_error(sh, "  <hidden>: 1 (<ssid> is hidden), 0 (<ssid> is NOT hidden)");
        return;
    }
    k_timeout_t timeout = K_MSEC(2000);
    int ret = connect_to_ssid(argv[1], 
                              argv[2], 
                              strtoul(argv[3], NULL, 10),
                              strtoul(argv[4], NULL, 10), 
                              strtoul(argv[5], NULL, 10), 
                              strtoul(argv[6], NULL, 10),
                              timeout);
    if (ret != 0) {
        shell_error(sh, "Failed to connect: %d", ret);
    }   else {
        shell_print(sh, "connected");
    }
}


void do_level_shifter(const struct shell *sh, size_t argc, char **argv)
{
    int new_state = 1;

    if (argc != 2) {
        shell_error(sh, "Usage: %s <on|off>", argv[0]);
        return;
    }
    if (strcasecmp(argv[1], "off") == 0) {
        new_state = 0;
    }   
    gpio_pin_set(gpio_p1, 14, new_state);
}


/////////////////////////////////////////////////////////
// get_da_fw_ver()
//
// Get the DA firmware version
//
// @param fwver - pointer to buffer to store the version
// @param len - length of the buffer
int get_da_fw_ver(char *fwver, int len)
{
    int ret = 0;
    wifi_msg_t msg;

    wifi_flush_msgs();

    ret = wifi_send_timeout("AT+VER", K_NO_WAIT);
    if (ret != 0) {
        LOG_ERR("Failed to send spi %d", ret);
        return -1;
    }

	ret = wifi_recv(&msg, K_MSEC(1000));
    if (ret != 0) {
        LOG_ERR("Didn't receive a response %d", ret);
        return -1;
    }

    if (len > msg.data_len) {
        len = msg.data_len;
    } 
    strncpy(fwver, msg.data, len);
    wifi_msg_free(&msg);
    return 0;
}


/////////////////////////////////////////////////////////
// get_wfscan()
//
// Get an list of SSIDs seen by the DA
//
// @param buf - pointer to buffer to store the version
// @param len - length of the buffer
int get_wfscan(char *buf, int len)
{
    int ret = 0;
    wifi_msg_t msg;

    wifi_flush_msgs();

    ret = wifi_send_timeout("AT+WFSCAN", K_NO_WAIT);
    if (ret != 0) {
        LOG_ERR("Failed to send spi %d", ret);
        return -1;
    }

    ret = wifi_recv(&msg, K_MSEC(2000));
    if (ret != 0) {
        LOG_ERR("Didn't receive a response %d", ret);
        return -1;
    }
    if (len > msg.data_len) {
        len = msg.data_len;
    } 
    strncpy(buf, msg.data, len);
    wifi_msg_free(&msg);
    return 0;
}


/////////////////////////////////////////////////////////
// connect_to_ssid
//
// Connect to a ssid
//
// <ssid>: SSID. 1 ~ 32 characters are allowed
// <key>: Passphrase. 8 ~ 63 characters are allowed   or NULL if sec is 0 or 5
// <sec>: Security protocol. 0 (OPEN), 1 (WEP), 2 (WPA), 3 (WPA2), 4 (WPA+WPA2) ), 5 (WPA3 OWE), 6 (WPA3 SAE), 7 (WPA2 RSN & WPA3 SAE)
// <keyidx>: Key index for WEP. 0~3    ignored if sec is 0,2-7
// <enc>: Encryption. 0 (TKIP), 1 (AES), 2 (TKIP+AES)   ignored if sec is 0,1 or 5
// <hidden>: 1 (<ssid> is hidden), 0 (<ssid> is NOT hidden)
// <timeout>: timeout
int connect_to_ssid(char *ssid, char *key, int sec, int keyidx, int enc, int hidden, k_timeout_t timeout)
{
    int ret = 0;
    char cmd[128];
    wifi_msg_t msg;

    if (strlen(ssid) > 32) { LOG_ERR("SSID too long"); return -1; }
    if (sec < 0 || sec > 7) { LOG_ERR("Invalid sec"); return -1; }
    if (hidden < 0 || hidden > 1) { LOG_ERR("Invalid hidden"); return -1; }

    switch (sec)
    {
        case 0:
        case 5:
            sprintf(cmd, "AT+WFJAP='%s',%d,%d", ssid, sec, hidden);
            break;
        case 1:
            if (strlen(key) > 63) { LOG_ERR("Key too long"); return -1; }
            if (keyidx < 0 || keyidx > 3) { LOG_ERR("Invalid keyidx"); return -1; }
            sprintf(cmd, "AT+WFJAP='%s',%d,%d,%s,%d", ssid, sec, keyidx, key, hidden);
            break;
        default:
            if (strlen(key) > 63) { LOG_ERR("Key too long"); return -1; }
            if (enc < 0 || enc > 2) { LOG_ERR("Invalid enc"); return -1; }
            sprintf(cmd, "AT+WFJAP='%s',%d,%d,%s,%d", ssid, sec, enc, key, hidden);
            break;
    }

    k_timepoint_t timepoint = sys_timepoint_calc(timeout);
    wifi_flush_msgs();

    ret = wifi_send_timeout(cmd, timeout);
    timeout = sys_timepoint_timeout(timepoint);
    if (ret != 0 || K_TIMEOUT_EQ(timeout,K_NO_WAIT) == true) {
        LOG_ERR("Failure or timeout sending at+WFJAP %d", ret);
        return -1;
    }

    while(1) {
        ret = wifi_recv(&msg, timeout);
        if (ret != 0) {
            LOG_ERR("Didn't receive a response %d", ret);
            return -1;
        }
        bool is_deauth = (strstr(msg.data,"DEAUTH") != NULL);
        bool is_ok = (strstr(msg.data,"OK") != NULL);
        wifi_msg_free(&msg);

        if (is_deauth == false) {
            if (is_ok) {
                return 0;
            } else {
                return -1;
            }
        }
        timeout = sys_timepoint_timeout(timepoint);
        if (K_TIMEOUT_EQ(timeout,K_NO_WAIT) == true) {
            LOG_ERR("Failure or timeout sending at+WFJAP %d", ret);
            return -1;
        }
    }

    return -1;
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
    wifi_msg_t msg;

    strcpy(ver, "tde 0002.000");

    wifi_flush_msgs();

    int ret = wifi_send_timeout("AT+VER", K_NO_WAIT);
    if (ret != 0) {
        LOG_ERR("Failed to send spi %d", ret);
        return ver;
    }

    ret = wifi_recv(&msg, K_MSEC(1000));
    if (ret != 0) {
        LOG_ERR("Didn't receive a response %d", ret);
        return ver;
    }

    char *str = strstr(ver, "TDEVER_");
    if (str != NULL) {
        str += 7;
        memcpy(ver+9, str, 3);
    }
    wifi_msg_free(&msg); // free doesn't use len, so it fine to modify it
    return "tde 0002.000";
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
    int ret = 0;
    wifi_msg_t msg;

    k_timeout_t timeout = K_MSEC(500); 
    k_timepoint_t timepoint = sys_timepoint_calc(timeout);

    // EAS XXX TODO  we need to tie requests to responses so we don't have to flush the queue
    wifi_flush_msgs();

    ret = wifi_send_timeout("at+wfmac=?", timeout);
    if (ret != 0) {
        LOG_ERR("Failed to send at cmd, ret = %d", ret);
        return "tde 0022.000000000000";
    }

    while (K_TIMEOUT_EQ(timeout,K_NO_WAIT) == false) {
        if ((ret = wifi_recv(&msg, timeout)) != 0) {
            LOG_ERR("Didn't receive a response to at+wfmac=?, ret= %d", ret);
            return "tde 0022.000000000000";
        }
        int addrs[6];
        int num = sscanf(msg.data, "\r\n+WFMAC:%X:%X:%X:%X:%X:%X", &addrs[0], &addrs[1], &addrs[2], &addrs[3], &addrs[4], &addrs[5]);
        if (num == 6) {
            sprintf(mac, "tde 0022.%X%X%X%X%X%X", addrs[0], addrs[1], addrs[2], addrs[3], addrs[4], addrs[5]);
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
    int ret;
    wifi_msg_t msg;

    strcpy(result , "tde 0026.0000000000");
    k_timeout_t timeout = K_MSEC(1000);
    k_timepoint_t timepoint = sys_timepoint_calc(timeout);

    // EAS XXX TODO  we need to tie requests to responses so we don't have to flush the queue
    wifi_flush_msgs();

    ret = connect_to_ssid(ssid, pass, 4, 0, 2, 0, timeout);
    timeout = sys_timepoint_timeout(timepoint);
    if ((ret != 0) || (K_TIMEOUT_EQ(timeout,K_NO_WAIT))) {
        return result;
    }

    // Wait for the +WFJAP:1,'AP_SSID',192.168.2.131
    while (1) {
        timeout = sys_timepoint_timeout(timepoint);
        if (K_TIMEOUT_EQ(timeout,K_NO_WAIT)) {
            LOG_ERR("Timeout waiting for connect msg");
            break;
        }
        if ((ret = wifi_recv(&msg, timeout)) != 0) {
            LOG_ERR("Didn't receive a +WFJAP in time. ret=%d", ret);
            break;
        }
        char *sub = strstr(msg.data, "+WFJAP:");
        char ip[17];
        char ssid[33];
        if (sub != NULL) {
            sscanf(sub, "+WFJAP:1,'%[^']',%s", ssid, ip);
            sprintf(result, "tde 0026.%s", ip);
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
    int ret;
    wifi_msg_t msg;
    strcpy(result , "tde 0027.0");
    k_timeout_t timeout = K_MSEC(500);
    k_timepoint_t timepoint = sys_timepoint_calc(timeout);

    // EAS XXX TODO  we need to tie requests to responses so we don't have to flush the queue
    wifi_flush_msgs();

    ret = wifi_send_timeout("AT+WFRSSI", timeout);
    timeout = sys_timepoint_timeout(timepoint);
    if ((ret != 0) || (K_TIMEOUT_EQ(timeout,K_NO_WAIT))) {
        return result;
    }

    // Wait for the result + OK or ERROR
    while (1) {
        timeout = sys_timepoint_timeout(timepoint);
        if (K_TIMEOUT_EQ(timeout,K_NO_WAIT)) {
            LOG_ERR("Timeout waiting for connect msg");
            break;
        }
        if ((ret = wifi_recv(&msg, timeout)) != 0) {
            LOG_ERR("Didn't receive a +WFJAP in time. ret=%d", ret);
            break;
        }
        char *sub = strstr(msg.data, "+RSSI:");
        char rssi[17];
        if (sub != NULL) {
            sscanf(sub, "+RSSI:%s", rssi);
            sprintf(result, "tde 0027.%s", rssi);
            wifi_msg_free(&msg);
        } else if (strstr(msg.data, "OK") != NULL) {
            wifi_msg_free(&msg);
            break;
        } else if (strstr(msg.data, "ERROR") != NULL) {
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
    int ret;
    wifi_msg_t msg;
    strcpy(result , "tde 0028.0");
    k_timeout_t timeout = K_MSEC(1500);
    k_timepoint_t timepoint = sys_timepoint_calc(timeout);

    // EAS XXX TODO  we need to tie requests to responses so we don't have to flush the queue
    wifi_flush_msgs();

    ret = wifi_send_timeout("AT+WFSTAT", timeout);
    timeout = sys_timepoint_timeout(timepoint);
    if ((ret != 0) || (K_TIMEOUT_EQ(timeout,K_NO_WAIT))) {
        return result;
    }

    // Wait for the result + OK or ERROR
    while (1) {
        timeout = sys_timepoint_timeout(timepoint);
        if (K_TIMEOUT_EQ(timeout,K_NO_WAIT)) {
            LOG_ERR("Timeout waiting for connect msg");
            break;
        }
        if ((ret = wifi_recv(&msg, timeout)) != 0) {
            LOG_ERR("Didn't receive a response to WFSTAT in time. ret=%d", ret);
            break;
        }
        char *sub = strstr(msg.data, "+WFSTAT:");
        if (sub != NULL) {
            sub = strstr(msg.data, "wpa_state=COMPLETED");
            if (sub != NULL) {
                strcpy(result, "tde 0027.1");
            }
            wifi_msg_free(&msg);
        } else if (strstr(msg.data, "OK") != NULL) {
            wifi_msg_free(&msg);
            break;
        } else if (strstr(msg.data, "ERROR") != NULL) {
            wifi_msg_free(&msg);
            break;
        }
    }
    return result;
}

char tde_usage[] = "Usage: test_tde <tde test number (2=wifiver,22=getMAC,26=connect,27=getRSSI,28=connStatus)> [test params (26 sdid pass)]";
void do_test_tde(const struct shell *sh, size_t argc, char **argv)
{
    char *err = NULL;
    char *res;
    int testnum;
    if (argc < 2) {
        goto usage;
    }
    testnum = strtoul(argv[1], NULL, 10);

    switch (testnum) {
    case 2:
        res = tde0002();
        break;
    case 22:
        res = tde0022();
        break;
    case 26:
        if (argc != 4) {
            goto usage;
        }
        res = tde0026(argv[2], argv[3]);
        break;
    case 27:
        res = tde0027();
        break;
    case 28:
        res = tde0028();
        break;
    default:
        err = "Unknown test number";
        goto usage;
    }
    shell_print(sh, "Result: %s", res);
    return;

usage:
    if (err != NULL) {
        shell_error(sh, "%s", err); 
    }
    shell_error(sh, "%s", tde_usage); 
}

SHELL_CMD_REGISTER(wifi_at_passthru, NULL, "enable AT passthru mode to the DA16200 (deprecated, use 'da16200 uart_passthru')", do_wifi_ATPassthru_mode);

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_da16200,
    SHELL_CMD(reset, NULL, "reset the wifi", do_wifi_reset),
    SHELL_CMD(turn_off, NULL, "turn off the wifi", do_wifi_turn_off),
    SHELL_CMD(turn_on, NULL, "turn on the wifi", do_wifi_turn_on),
    SHELL_CMD(power_key, NULL, "set wifi power key line", do_wifi_power_key),
    SHELL_CMD(enable_3v3, NULL, "set wifi 3v3 enable line", do_wifi_set_3v3_enable),
    SHELL_CMD(set_wakeup, NULL, "set wifi wakeup line", do_wifi_set_wakeup),
    SHELL_CMD(insert_certs, NULL, "Send the CA,crt and private key in '<ESC>C' format to the DA, insert_certs <0-2>,  0=none (unencrypted/remove certs), 1= mosquitto.org, 2=aws staging", do_insert_certs),
    SHELL_CMD(set_time, NULL, "Set the time on the DA, needed for checking certs, set_time <YYYY> <MM> <DD> <HH> <mm>", do_set_time),
    SHELL_CMD(send_mqtt_msg, NULL, "Send a canned message a mqtt broker (based on which certs are installed)", do_send_mqtt_msg),
    SHELL_CMD(power_test, NULL, "Start a test where we send 4K/min for 5 min to staging", do_power_test),
    SHELL_CMD(dpm_mode, NULL, "Set the DPM mode, <on|off>", do_set_dpm_mode),
    SHELL_CMD(ota, NULL, "start a OTA update, <https://server:port> <filename>", do_ota),
    SHELL_CMD(uart_passthru, NULL, "enable AT passthru mode to the DA16200", do_wifi_ATPassthru_mode),
    SHELL_CMD(test_tde, NULL, tde_usage, do_test_tde),
    SHELL_CMD(test_get_wfscan, NULL, "test the get_wfscan() call", do_test_get_wfscan),
    SHELL_CMD(test_get_da_fw_ver, NULL, "test the get_da_fw_ver() call", do_test_get_da_fw_ver),
    SHELL_CMD(test_connect_ssid, NULL, "test the connect_to_ssid() call, ssid key sec keyidx enc hidden", do_test_connect_ssid),
    SHELL_CMD(send_atcmd, NULL, "set an at cmd over wifi to the da, <cmd> [display returns yes/no]", do_send_atcmd),
    SHELL_CMD(flush, NULL, "flush recv q", do_flush),
    SHELL_CMD(level_shifter, NULL, "Set the power on the wifi level shifter, <on|off>", do_level_shifter),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(da16200, &sub_da16200, "Commands to control the DA16200", NULL);
