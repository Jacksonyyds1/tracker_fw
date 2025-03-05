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
#include "pmic.h"

#define CHAR_1       0x18
#define CHAR_2       0x11
#define SEND_BUF_LEN 1024
char wifi_send_buf[SEND_BUF_LEN];
uint8_t wifi_send_buf_ptr = 0;

#define MAX_AT_BUFFER_SIZE 3100
#define shell_error_opt(sh, fmt, ...)                                                              \
	if (sh) {                                                                                  \
		shell_fprintf(sh, SHELL_ERROR, fmt "\n", ##__VA_ARGS__);                           \
	}
#define shell_print_opt(sh, fmt, ...)                                                              \
	if (sh) {                                                                                  \
		shell_fprintf(sh, SHELL_NORMAL, fmt "\n", ##__VA_ARGS__);                          \
	}

LOG_MODULE_REGISTER(wifi_shell);

char com_buf[COM_BUF_LEN];
static int com_idx = 0;
int g_current_cert = 0;
bool g_time_set = false;
bool bypass_in_use = false;
extern const struct device *gpio_p1;
extern const struct device *gpio_p0;
void shell_print_ctl_n(const struct shell *sh, char *in_buf, int len, bool printlf);
void shell_print_ctl(const struct shell *sh, char *in_buf, bool printlf);

// This will be called whenever we are in bypass mode and
// we receive a message from the DA16200
bool wifi_shell_on_rx(wifi_msg_t *msg, void *user_data)
{
	const struct shell *sh = (const struct shell *)user_data;
	if (bypass_in_use == false) {
		return false;
	}

	// Print any messages already in the queue and
	// then print the new message
	wifi_msg_t qmsg;
	while (wifi_recv(&qmsg, K_NO_WAIT) == 0) {
		shell_print_ctl_n(sh, qmsg.data, qmsg.data_len, true);
		wifi_msg_free(&qmsg);
	}
	shell_print_ctl_n(sh, msg->data, qmsg.data_len, true);
	return true;
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
		wifi_set_rx_cb(wifi_shell_on_rx, (void *)sh);
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

	for (int i = 0; i < len; i++) {
		if (wifi_send_buf_ptr < SEND_BUF_LEN) {
			if (data[i] == '\n' || data[i] == '\r' || data[i] == '\0') {
				wifi_string_complete = true;
			} else {
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

////////////////////////////////////////////////////////////
// wake_DA()
//
// try to wake up the DA by pulsing the WAKEUP_RTC line
// The DA wakes on the falling edge of WAKEUP_RTC so if
// it is already low, we need to pulse it high first.
//
// @param sh - shell to print toS oir NULL
//
// @return - none
void wake_DA(const struct shell *sh)
{
	if (sh != NULL) {
		shell_print(sh, "Pulsing WAKEUP_RTC");
	}
	int curr = gpio_pin_get_raw(gpio_p1, 8);
	if (curr == 0) {
		gpio_pin_set(gpio_p1, 8, 1); // Pulse high
		k_sleep(K_MSEC(100));
	}
	k_sleep(K_MSEC(100));
	gpio_pin_set(gpio_p1, 8, 0); // Turn off wakeup, triggered on falling edge
}

///////////////////////////////////////////////////////////////////////
// send_ok_err_atcmd()
//  Send a command to the DA and wait for a OK or ERROR response.
//
//  @param sh - the shell to print to or NULL
//  @param cmd - the command to send
//  @param timeout - timeout for the write
//
//  @return - true if OK was received
bool send_ok_err_atcmd(const struct shell *sh, char *cmd, k_timeout_t timeout)
{
	int ret;
	char errstr[50];
	wifi_wait_array_t wait_msgs;

	k_timepoint_t timepoint = sys_timepoint_calc(timeout);
	if (wifi_send_timeout(cmd, timeout) != 0) {
		shell_error_opt(sh, "Timed out sending %s", cmd);
		return false;
	}
	timeout = sys_timepoint_timeout(timepoint);
	wait_msgs.num_msgs = 0; // Initialize the structure
	wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
	wifi_add_wait_msg(&wait_msgs, "\r\nERROR%50s\r\n", true, 1, errstr);
	ret = wifi_wait_for(&wait_msgs, timeout);
	if (ret == 0) {
		return true;
	}
	if (ret == 1) {
		shell_error_opt(sh, "Error received on %s: %s", cmd, errstr);
	} else {
		shell_error_opt(sh, "Command |%s| timed out", cmd);
	}
	return false;
}

////////////////////////////////////////////////////////////////////////////////
// dpm_wake_no_sleep()
//
// Wake the DA from DPM sleep and tell it not to sleep for now.
// If the DA is not in DPM mode, the it won't respond to the WAKEUP
//
//  @param sh - the shell to print to or NULL
//
//  @return - true if we are in DPM mode and we told the DA not to sleep
bool dpm_wake_no_sleep(const struct shell *sh)
{
	// At spi speed of 4Mhz, my tests show this takes < 305ms 95% of the time
	// there was one case where it took 370
	k_timeout_t timeout = K_MSEC(400);
	k_timepoint_t timepoint = sys_timepoint_calc(timeout);
	wifi_wait_array_t wait_msgs;
	bool result = false;
	int ret;

	wake_DA(sh); // Pulse the WEAKEUP_RTC line.

	wait_msgs.num_msgs = 0; // Initialize the structure
	wifi_add_wait_msg(&wait_msgs, "+INIT:WAKEUP", true, 0);
	ret = wifi_wait_for(&wait_msgs, timeout);
	if (ret != 0) {
		shell_error_opt(sh, "No +INIT:WAKEUP event, not in DPM mode, can't change sleep");
		goto print_fn_time;
	}

	// At this point, we know we are in DPM mode and the caller wants
	// us to pause sleep.  So send the commands to the DA to do that.
	shell_print(sh, "Sending MCU Wake up done cmd");
	timeout = sys_timepoint_timeout(timepoint);
	if (send_ok_err_atcmd(sh, "AT+MCUWUDONE", timeout) == false) {
		shell_error_opt(sh, "%s cmd failed", "AT+MCUWUDONE");
		goto print_fn_time;
	}

	shell_print(sh, "Stopping the DA from sleeping");
	timeout = sys_timepoint_timeout(timepoint);
	if (send_ok_err_atcmd(sh, "AT+CLRDPMSLPEXT", timeout) == false) {
		shell_error_opt(sh, "%s cmd failed", "AT+CLRDPMSLPEXT");
		goto print_fn_time;
	}
	result = true;

print_fn_time:
	return result;
}

////////////////////////////////////////////////////////////////////////////////
// dpm_back_to_sleep()
//
// Tell the DA it can go back to sleep.
//
//  @param sh - the shell to print to or NULL
//
//  @return - none
void dpm_back_to_sleep(const struct shell *sh)
{
	// At spi speed of 4Mhz, my tests show this never takes more then 10ms
	k_timeout_t timeout = K_MSEC(30);

	shell_print_opt(sh, "Allowing DA to sleep");
	if (send_ok_err_atcmd(sh, "AT+SETDPMSLPEXT", timeout) == false) {
		shell_error_opt(sh, "%s cmd failed", "AT+SETDPMSLPEXT");
	}
}

////////////////////////////////////////////////////////////////////////////////
// check_dpm_mode()
//
// Check to see if the DA is in dpm mode.  This is done
// more complexly then strictly needed because I wanted
// to try the higher level "wait_for" APIs
//
//  @param sh - the shell to print to or NULL
//
//  @return - none
bool check_dpm_mode(const struct shell *sh)
{
	int ret;
	bool is_dpm = false;
	wifi_wait_array_t wait_msgs;
	char dpm_state[50];
	char errorstr[50];

	// At spi speed of 4Mhz, my tests show this take around 300ms
	k_timeout_t timeout = K_MSEC(1400);
	k_timepoint_t timepoint = sys_timepoint_calc(timeout);

	if (dpm_wake_no_sleep(sh) == false) {
		goto print_fn_time;
	}

	// Now we give it a AT+DPM=?
	wait_msgs.num_msgs = 0; // Initialize the structure
	wifi_add_wait_msg(&wait_msgs, "+DPM:%1s", false, 1, dpm_state);
	wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
	wifi_add_wait_msg(&wait_msgs, "\r\nERROR%50s\r\n", true, 1, errorstr);
	timeout = sys_timepoint_timeout(timepoint);
	ret = wifi_send_and_wait_for("AT+DPM=?", &wait_msgs, timeout);
	if (ret < 0) {
		shell_error_opt(sh, "Timed out waiting for response to %s", "AT+DPM=?");
		goto go_back_to_sleep;
	} else if (ret == 2) {
		shell_error_opt(sh, "Error on %s received: %s", "AT+DPM=?", errorstr);
		goto go_back_to_sleep;
	}
	if (wait_msgs.num_matched[0] > 0) {
		if (strstr(dpm_state, "1") != NULL) {
			is_dpm = true;
		}
	} else {
		shell_error_opt(sh, "No response received");
	}
go_back_to_sleep:
	dpm_back_to_sleep(sh);

print_fn_time:
	return is_dpm;
}

///////////////////////////////////////////////////////////////
// set_mqtt_subscribed_topics()
//
// Set the list of topics that the MQTT service subscribes
// to.
// This need to be called when the DA is awake.
//
//  @param sh - the shell to print to or NULL
//  @param new_topics - The list of topics to subscribe to
//
//  @return - If the settting succeeded
bool set_mqtt_subscribed_topics(const struct shell *sh, int num_topics, char *new_topics[])
{
	int ret;
	wifi_wait_array_t wait_msgs;
	char errorstr[50];
	char cmd[15];
	int i, len = 0;

	// At spi speed of 4Mhz, my tests show this take around 300ms
	k_timeout_t timeout = K_MSEC(1400);

	sprintf(com_buf, "AT+NWMQTS=%d", num_topics);
	len = strlen(com_buf);
	for (i = 0; i < num_topics; i++) {
		len += strlen(new_topics[i]);
		len += 1; // for the comma
	}
	len += 1; // for the terminating 0

	if (len > WIFI_MSG_SIZE) {
		shell_error_opt(sh, "Topic list too long");
		return false;
	}

	for (i = 0; i < num_topics; i++) {
		strcat(com_buf, ",");
		strcat(com_buf, new_topics[i]);
	}

	wait_msgs.num_msgs = 0; // Initialize the structure
	wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
	wifi_add_wait_msg(&wait_msgs, "\r\nERROR%50s\r\n", true, 1, errorstr);
	ret = wifi_send_and_wait_for(com_buf, &wait_msgs, timeout);
	if (ret == 0) {
		return true;
	}
	if (ret < 0) {
		shell_error_opt(sh, "Timed out waiting for response to %s", cmd);
	} else if (ret == 2) {
		shell_error_opt(sh, "Error on %s received: %s", cmd, errorstr);
	}
	return false;
}

void do_wifi_reset(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "Resetting wifi...\n");
	wifi_1v8_off();
	wifi_set_power_key(0);
	wifi_set_3v0_enable(0);
	k_msleep(1000);
	 wifi_1v8_on();
	wifi_set_power_key(1);
	wifi_set_3v0_enable(1);
	k_msleep(1000);
	shell_print(sh, "Wifi reset complete\n");
}

void do_wifi_turn_off(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "Turning off wifi...\n");
	wifi_1v8_off();
	shell_print(sh, "Wifi turned off\n");
}

void do_wifi_turn_on(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "Turning on wifi...\n");
	 wifi_1v8_on();
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

void do_wifi_set_3v0_enable(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "Usage: wifi enable_3v0 <1|0>\n");
		return;
	}
	int newState = atoi(argv[1]);
	wifi_set_3v0_enable(newState);
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
	int ret, total = 0;
	wifi_msg_t msg;

	if (sh != NULL) {
		shell_fprintf(sh, SHELL_NORMAL, "Clearing wifi buffers: |");
	}
	for (int i = 0; i < 300; i++) {
		ret = wifi_recv(&msg, K_USEC(10));
		if (ret != 0) {
			break;
		}
		if (sh != NULL) {
			if (hide_ctl) {
				shell_print_ctl_n(sh, msg.data, msg.data_len, false);
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

// Send an 'AT' to the DA16200 and return if the response was 'OK'
bool check_for_at_response(const struct shell *sh, int maxwaitms)
{
	return send_ok_err_atcmd(sh, "at", K_MSEC(maxwaitms));
}

/////////////////////////////////////////////////////////
// set_dpm_mode()
// Set the DA16200 to DPM mode or not
//
// @param sh - shell to print to
// @param new_state - true to turn on DPM mode, false to turn it off
//
// @return - 0 on success, -1 on error
int set_dpm_mode(const struct shell *sh, bool new_state)
{
	int ret;
	// Make sure the level shifter is powered on
	gpio_pin_set(gpio_p1, 14, 1);

	clear_recv(sh, true);

	bool curr_dpm = check_dpm_mode(sh);
	if (curr_dpm == new_state) { // We are already in the state we want
		shell_print(sh, "DPM mode is already %s", curr_dpm ? "on" : "off");
		return -1;
	}

	// flip the state
	if (curr_dpm) {
		dpm_wake_no_sleep(sh);
		ret = send_ok_err_atcmd(sh, "AT+DPM=0", K_MSEC(1000));
	} else {
		ret = send_ok_err_atcmd(sh, "AT+DPM=1", K_MSEC(1000));
	}
	if (ret <= 0) {
		shell_error(sh, "Failed to set DPM mode");
		return -1;
	}
	return 0;
}

// Construct the publish message
// We can only send message of < 255 to the broker. so we send several messages
// to make up the total amount
// #define MQTT_TST_HDR "AT+NWMQMSG='{\"P\": 2, \"MID\": \"eas-mpb-test-001\", \"MK\": \"US\",
// \"B\": 35, \"T\": 10, \"M\": {" #define MQTT_TST_FTR "}}',messages/35/10/35_eas-mpb-test-001/d2c"
#define MQTT_TST_HDR                                                                               \
	"AT+NWMQMSG='{"                                                                            \
	"\"P\":2,"                                                                                 \
	"\"MID\":\"eas-mpb-test-001\","                                                            \
	"\"MK\":\"US\","                                                                           \
	"\"B\":35,"                                                                                \
	"\"T\":1,"                                                                                 \
	"\"M\":{"                                                                                  \
	"\"BATTERY_SOC\":95,"                                                                      \
	"\"CHARGING\":false,"                                                                      \
	"\"GPS\":{"                                                                                \
	"\"LAT\":231.30,"                                                                          \
	"\"LONG\":442.33,"                                                                         \
	"\"ALT\":34.23,"                                                                           \
	"\"ACC\":1"                                                                                \
	"},"                                                                                       \
	"\"WIFI\":["                                                                               \
	"{"                                                                                        \
	"\"SSID\":\"SSID_One\","                                                                   \
	"\"RSSI\":-34,"                                                                            \
	"\"CHAN\":1"                                                                               \
	"},"                                                                                       \
	"{"                                                                                        \
	"\"SSID\":\"SSID_Two\","                                                                   \
	"\"RSSI\":-43,"                                                                            \
	"\"CHAN\":2"                                                                               \
	"},"                                                                                       \
	"{"                                                                                        \
	"\"SSID\":\"SSID_Three\","                                                                 \
	"\"RSSI\":-13,"                                                                            \
	"\"CHAN\":3"                                                                               \
	"}"                                                                                        \
	"],"                                                                                       \
	"\"XTR\":{"
#define MQTT_TST_FTR                                                                               \
	"}"                                                                                        \
	"}"                                                                                        \
	"}',messages/35/1/35_eas-mpb-test-001/d2c"
#define MQTT_TST_HDR_LEN      (sizeof(MQTT_TST_HDR) - 1)
#define MQTT_TST_FTR_LEN      (sizeof(MQTT_TST_FTR) - 1)
#define MQTT_TST_NON_BODY_LEN (MQTT_TST_HDR_LEN + MQTT_TST_FTR_LEN)
#define MIN_MQTT_TST_MSG_LEN  (MQTT_TST_NON_BODY_LEN + 9 + 2 + 3)
// A single field with one char
#define MAX_MQTT_FIELD_LEN    (250)
char *make_mqtt_msg(const struct shell *sh, int body_size)
{
	int num_fields, field_len;

	if (body_size > COM_BUF_LEN) {
		return NULL;
	}
	if (body_size < MIN_MQTT_TST_MSG_LEN) {
		body_size = MIN_MQTT_TST_MSG_LEN;
	}
	int field_total_size = body_size - MQTT_TST_NON_BODY_LEN;
	num_fields = (field_total_size / (9 + 3 + MAX_MQTT_FIELD_LEN)) + 1;
	field_len = field_total_size / num_fields;
	shell_print(sh,
		    "Making MQTT message with %d header, %d fields (%d header + %d payload) and %d "
		    "footer, ",
		    MQTT_TST_HDR_LEN, num_fields, 9 + 3, field_len, MQTT_TST_FTR_LEN);
	strcpy(com_buf, MQTT_TST_HDR);

	for (int field = 0; field < num_fields;
	     field++) { // each loop adds 9 + field_len*2 + 3 chars
		char temp[20];
		strcpy(temp, "\"S\":\"");
		// sprintf(temp, "\"S%03d\" : \"", field);
		strcat(com_buf, temp);

		for (int f = 0; f < field_len / 2; f++) {
			uint8_t byte = rand() % 256;
			char hex[4];
			sprintf(hex, "%x", byte);
			strcat(com_buf, hex);
		}
		if (field < num_fields - 1) {
			strcat(com_buf, "\",");
		} else {
			strcat(com_buf, "x\"");
		}
	}
	strcat(com_buf, MQTT_TST_FTR);

	return com_buf;
}

//////////////////////////////////////////////////////////////////////
// send_recv_mqtt()
//  Send and receive MQTT messages. We must not be in DPM sleep, and
//  we must be connected to an AP.
//
//  @param sh - shell to print to
//  @param do_recv - true to receive messages, false to just send
//  @param num_bytes - number of bytes to send
//
//  @return - none
void send_recv_mqtt(const struct shell *sh, bool do_recv, int num_bytes)
{
	char *broker_cmd, *tls_cmd, *sub_topic;
	int ret;
	wifi_wait_array_t wait_msgs;
	char mqtt_state[200];

	if (g_time_set == false && g_current_cert != 0) {
		shell_error(sh,
			    "encrypted mqtt certs need the time set, use 'da16200 set_time' first");
		return;
	}
	shell_print(sh, "Configuring MQTT");
	switch (g_current_cert) {
	case 0:
		shell_print(sh, "Connecting to unencrypted mosquitto");
		broker_cmd = "AT+NWMQBR=test.mosquitto.org,1883";
		tls_cmd = "AT+NWMQTLS=0";
		sub_topic = "messages/35/1/35_eas-mpb-test-001/d2c";
		break;
	case 1:
		shell_print(sh, "Connecting to encrypted mosquitto");
		broker_cmd = "AT+NWMQBR=test.mosquitto.org,8883";
		tls_cmd = "AT+NWMQTLS=1";
		sub_topic = "messages/35/1/35_eas-mpb-test-001/d2c";
		break;
	case 2:
	default:
		shell_print(sh, "Connecting to encrypted staging");
		broker_cmd = "AT+NWMQBR=a3hoon64f0fuap-ats.iot.eu-west-1.amazonaws.com,8883";
		tls_cmd = "AT+NWMQTLS=1";
		sub_topic = "messages/35/1/35_eas-mpb-test-001/c2d";
		break;
	}

	if (!send_ok_err_atcmd(sh, "AT+NWMQCL=0", K_MSEC(200))) {
		shell_error(sh, "Error Stopping MQTT service");
		return;
	}

	if (!send_ok_err_atcmd(sh, broker_cmd, K_MSEC(1000))) {
		shell_error(sh, "Error Setting the MQTT broker");
		return;
	}

	if (!send_ok_err_atcmd(sh, "AT+NWMQTP=messages/35/1/35_eas-mpb-test-001/d2c",
			       K_MSEC(1000))) {
		shell_error(sh, "Error Setting the default publish topic");
		return;
	}

	if (do_recv) {
		char *sub_topics[1] = {sub_topic};
		if (set_mqtt_subscribed_topics(sh, 1, sub_topics) != true) {
			shell_error(sh, "Error Setting the list of subscribe topics");
			return;
		}
	}

	if (!send_ok_err_atcmd(sh, "AT+NWMQCID=35_eas-mpb-test-001", K_MSEC(1000))) {
		shell_error(sh, "Error Setting the client id");
		return;
	}

	if (!send_ok_err_atcmd(sh, "AT+NWMQCS=1", K_MSEC(1000))) {
		shell_error(sh, "Error Setting the clean session flag");
		return;
	}

	if (!send_ok_err_atcmd(sh, tls_cmd, K_MSEC(1000))) {
		shell_error(sh, "Error setting the TLS flag");
		return;
	}

	if (!send_ok_err_atcmd(sh, "AT+NWMQCL=1", K_MSEC(200))) {
		shell_error(sh, "Error Starting MQTT service");
		return;
	}

	// Wait for the connection to be established
	shell_print(sh, "Waiting for broker to connect");
	wait_msgs.num_msgs = 0; // Initialize the structure
	wifi_add_wait_msg(&wait_msgs, "+NWMQCL:%1s", true, 1, mqtt_state);
	if (wifi_wait_for(&wait_msgs, K_MSEC(7000)) != 0) {
		shell_error(sh, "Timed out waiting for mqtt service to start");
		return;
	}

	make_mqtt_msg(sh, num_bytes);
	shell_print(sh, "Sending %d bytes to broker %s", strlen(com_buf), com_buf);
	// Send to broker
	if (!send_ok_err_atcmd(sh, com_buf, K_MSEC(3000))) {
		shell_error(sh, "Error publishing a connectivity test messasge");
		return;
	}
	if (do_recv) {
		bool sent = false;
		bool recv = false;
		wifi_msg_t msg;
		char *sub;
		shell_print(
			sh,
			"Waiting for confirmation we sent and received the message successfully");

		while (1) {
			ret = wifi_recv(&msg, K_MSEC(9000));
			if (ret != 0) {
				shell_error(sh, "Timed out waiting for confirmation");
				return;
			}
			if ((sub = strstr(msg.data, "+NWMQMSGSND:")) != NULL) {
				shell_print(sh, "Confirmed send");
				shell_print_ctl(sh, sub + 12, false);
				sent = true;
			}
			if ((sub = strstr(msg.data, "+NWMQMSG:")) != NULL) {
				shell_print(sh, "Confirmed recv");
				shell_print_ctl(sh, sub + 9, false);
				recv = true;
			}
			shell_print(sh, "");
			if (recv && sent) {
				break;
			}
		}
	}

	k_msleep(1000);

	shell_print(sh, "Stopping the MQTT client");
	if (!send_ok_err_atcmd(sh, "AT+NWMQCL=0", K_MSEC(1000))) {
		shell_error(sh, "Error Stopping MQTT service");
	}
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

char gen_crt[] = "\eC1,"
		 "-----BEGIN CERTIFICATE-----\r\n"
		 "MIIDWjCCAkKgAwIBAgIVAMaexJyU4p5UwcogGMQZcyNXnckYMA0GCSqGSIb3DQEB\r\n"
		 "CwUAME0xSzBJBgNVBAsMQkFtYXpvbiBXZWIgU2VydmljZXMgTz1BbWF6b24uY29t\r\n"
		 "IEluYy4gTD1TZWF0dGxlIFNUPVdhc2hpbmd0b24gQz1VUzAeFw0yNDAxMDkxNzA1\r\n"
		 "MTFaFw00OTEyMzEyMzU5NTlaMB4xHDAaBgNVBAMME0FXUyBJb1QgQ2VydGlmaWNh\r\n"
		 "dGUwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDHro/905SjprSL5E8X\r\n"
		 "4+DVqfL+uy/go2w9cjv4mOeohegsjRKAbLTujuiVajvOaqPp2dpC8IbQPEYJbBiX\r\n"
		 "g+gfzrQMl7tGEjeSv6gU/HIL36W1J7OdB0AAQJLShKE0qXgoe+wp7uz/oVJcupVj\r\n"
		 "SSPL/DX4+aLgJZF87bQEvLWs3+OmAm+5x27g8d4pZzD5mO4njMbNZv3VuhZntcwu\r\n"
		 "fJ41VaI/Q8nmVfzNbJbkrvBx0/rhhtb6svoLo9JxIUnRWkuDzyzFqQ8peCnlAnL+\r\n"
		 "pJth0kFKPRflQamn2yBfU1bDP50bL+OeunPybU2daDOcUyFEvFqITaWQ75EaDADv\r\n"
		 "cxI5AgMBAAGjYDBeMB8GA1UdIwQYMBaAFL7jfazBeXj1y+UOncLdkwFXVfjgMB0G\r\n"
		 "A1UdDgQWBBSzUwTlONq757lpaCt0/SfIQxOL1zAMBgNVHRMBAf8EAjAAMA4GA1Ud\r\n"
		 "DwEB/wQEAwIHgDANBgkqhkiG9w0BAQsFAAOCAQEAPbZDSWA2hF1A+jiFWJKoJety\r\n"
		 "aW91jgtUxGxY1IZyL5eUFp+DtL3mNkuGqyrQpF7Jz6mhubhgbGuKiA+Grz5iNGSB\r\n"
		 "JHxmvJbh5w2Qr4ICwiM6gS7/YoWSYdVdjyKAWMrTzZmaAxf4jztCTL99nqdTi0UI\r\n"
		 "9cpwLCmlMe6iIYG2JUK/FXY+eyzSb9ZxGqFQOgzxY//oAbWHwRNztupx2XyJGaOp\r\n"
		 "N2saJ5mAQBJE6ZDRw8pWADqAqhhh90Vrtoy6KPbFZA7O602nFNfyUElpdjTFNYV3\r\n"
		 "ykWSK5ZBqu0BcarC9gST5LDzwQFOU3b1byLExA6i1AW6ifSiyU9EA2pFaLYesw==\r\n"
		 "-----END CERTIFICATE-----\r\n"
		 "\003";

char gen_pri[] = "\eC2,"
		 "-----BEGIN RSA PRIVATE KEY-----\r\n"
		 "MIIEpQIBAAKCAQEAx66P/dOUo6a0i+RPF+Pg1any/rsv4KNsPXI7+JjnqIXoLI0S\r\n"
		 "gGy07o7olWo7zmqj6dnaQvCG0DxGCWwYl4PoH860DJe7RhI3kr+oFPxyC9+ltSez\r\n"
		 "nQdAAECS0oShNKl4KHvsKe7s/6FSXLqVY0kjy/w1+Pmi4CWRfO20BLy1rN/jpgJv\r\n"
		 "ucdu4PHeKWcw+ZjuJ4zGzWb91boWZ7XMLnyeNVWiP0PJ5lX8zWyW5K7wcdP64YbW\r\n"
		 "+rL6C6PScSFJ0VpLg88sxakPKXgp5QJy/qSbYdJBSj0X5UGpp9sgX1NWwz+dGy/j\r\n"
		 "nrpz8m1NnWgznFMhRLxaiE2lkO+RGgwA73MSOQIDAQABAoIBAQDBfmRFoIs+ccIu\r\n"
		 "dNQ34Df0k7TGJnlkgrfWayW15eVFpkyvLxyoma5SJOU4NDMz+J5RcytPBmh8zItJ\r\n"
		 "ghfqaoW6nMBYG4f7hJeZemLTwzR4UQXwH0KrfWUFWY1stdzIfRfUkxDsKXw60gZP\r\n"
		 "mPY9uZgYiJN8adrsvTrYBjcVCBA8LRetQvedaQ4sXMDYLGUXqPPHa2J/8BUgedse\r\n"
		 "1Tg2D3YHb0SFdiq9/y1nlwuJ2KPoGM0MrNhKcUTna5UPlpwcXyeRt8N12LGDj4p7\r\n"
		 "qSuiISI4ciTJaT9W2TXf0YBFZIuoJi+fpNNPZ1yX+uVeskuTujgegFR/Hrlbhben\r\n"
		 "g5lood0BAoGBAPonBfaxkhivf1JBLcMywW10RJxYuiJtfvxphpxG9dwZGSMauQv8\r\n"
		 "coFHlST0186CXBIz+UVZpK9yvXSeQndeSfKbBZXc7MqxoVECr4btCJ+sDDaojT/4\r\n"
		 "fz8zJFxuo4Y8XjzJ4ovDZkVEuB51aBUeabWoiJYOXfpgZ8yPnyxUxOupAoGBAMxZ\r\n"
		 "gqgx0Je9zqDhncpY1y642SQlUPvQqC9lNc9g0vAaCcTvPWJJJOjwuIOIe1pmeu14\r\n"
		 "EY9PYQ+WRxZOmD8btmnSA6xQ3+ez/L9g3r188E4AwDlD7EqBP1RBpniktozj2Ln0\r\n"
		 "Qx3OyhOLMgW78ZkR5RVfpy6uqpTfUK6M0IY454wRAoGBANQMNE/0IkXurl0Jh0NK\r\n"
		 "SREBWA+4XsBEVTqeMU9UD6FtXROW2XnBm0bfaRwUuMMDWgzMJeeVn7Zh6xtp9Cm/\r\n"
		 "sauHoWpyBf+kJ3zGbAhR8dd4PjovEt1BRoxKS+5WkyN9FN0uvW2AS7c4E2MuXvZX\r\n"
		 "+3/Tx0R8FUniYtrSfDlV+j9RAoGAFLw4b8yuN9eAsf5yvmeJXipPjkVjFEo15qlE\r\n"
		 "boiKnuZle2AzQFffsOtMLbyZl7CyZIo8behGFwjOqcrL1AuB1PcEfGWwcphm2xx/\r\n"
		 "PYcB80hMOQNVZLH8tRX8SF2eUGmGQkgwz0N+acFEECTj/P492o9cLXZ3xKdsTmnn\r\n"
		 "dtcrV6ECgYEA9gfp0XG+KGGh8dmOWukAHpXNauQHPhtdX+QsvQV0FtzhPK6rxLeB\r\n"
		 "QXGt2Iv2OzdK1GqyVgyEq84OFnjzVJ6XuJo+ZwX9gbuSSSoQRbBekJs8Zlh8PfCP\r\n"
		 "xqe6xcC1qKxyyDuJrZptuatPkNIg6xKbu+CRoXYKQVbXCbamwSMiud4=\r\n"
		 "-----END RSA PRIVATE KEY-----\r\n"
		 "\003";

char *certs[3][3] = {
	{mos_CA, mos_crt, mos_pri}, {sim_CA_ats, sim_crt, sim_pri}, {sim_CA_ats, gen_crt, gen_pri}};

void do_insert_certs(const struct shell *sh, size_t argc, char **argv)
{
	int cert = 0;

	if (argc > 1) {
		cert = strtoul(argv[1], NULL, 10);
	}
	if (cert > 3 || argc != 2) {
		shell_error(sh,
			    "Usage: %s <0-3>,  0=none (unencrypted/remove certs), 1= "
			    "mosquitto.org, 2=aws staging, 3=aws stag provisioned",
			    argv[0]);
		return;
	}

	clear_recv(sh, true);

	g_current_cert = cert;
	if (cert == 0) {
		shell_print(sh, "Removing MQTT certs");
		if (!send_ok_err_atcmd(sh, "\eCERT,0,0,1", K_MSEC(1000))) {
			return;
		}
		if (!send_ok_err_atcmd(sh, "\eCERT,0,1,1", K_MSEC(1000))) {
			return;
		}
		if (!send_ok_err_atcmd(sh, "\eCERT,0,2,1", K_MSEC(1000))) {
			return;
		}
		return;
	}

	cert -= 1;
	if (cert == 0) {
		shell_print(sh, "Inserting mosquitto.org certs");
	}
	if (cert == 1) {
		shell_print(sh, "Inserting AWS staging certs");
	}
	if (cert == 2) {
		shell_print(sh, "Inserting AWS provisioned staging certs");
	}
	for (int i = 0; i < 3; i++) {
		shell_print(sh, "Sending %25.25s", certs[cert][i] + 1);
		if (!send_ok_err_atcmd(sh, certs[cert][i], K_MSEC(1000))) {
			return;
		}
	}
}

void do_set_time(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 6 || strlen(argv[1]) != 4 || strlen(argv[2]) != 2 || strlen(argv[3]) != 2 ||
	    strlen(argv[4]) != 2 || strlen(argv[5]) != 2) {
		shell_error(sh, "Usage: %s <YYYY> <MM> <DD> <HH> <mm>", argv[0]);
		return;
	}

	clear_recv(sh, true);

	char *year = argv[1];
	char *month = argv[2];
	char *day = argv[3];
	char *hour = argv[4];
	char *minute = argv[5];
	char timecmd[50];
	sprintf(timecmd, "AT+TIME=%s-%s-%s,%s:%s:00", year, month, day, hour, minute);

	if (!send_ok_err_atcmd(sh, timecmd, K_MSEC(1000))) {
		return;
	}
	g_time_set = true;
}

void do_send_mqtt_msg(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 3) {
		shell_error(sh, "Usage: %s <num:bytes to send> <bool: recv messages>", argv[0]);
		return;
	}
	int amount = strtoul(argv[1], NULL, 10);
	bool recv = false;
	if (strcmp(argv[2], "true") == 0 || strcmp(argv[2], "1") == 0) {
		recv = true;
	}

	clear_recv(sh, true);

	if (!check_for_at_response(sh, 1000)) {
		return;
	}

	if (!send_ok_err_atcmd(sh, "ATZ", K_MSEC(1000))) {
		shell_error(sh, "Could not reset AT state.");
		return;
	}

	if (!send_ok_err_atcmd(sh, "AT+WFSTAT", K_MSEC(3000))) {
		shell_error(sh, "Not connected to an AP.");
		return;
	}

	send_recv_mqtt(sh, recv, amount);
}

void do_power_test(const struct shell *sh, size_t argc, char **argv)
{
	int ret, j;
	uint64_t now;

	if (argc != 2) {
		shell_error(sh,
			    "Usage: %s <num: seconds> <num:bytes to send> <bool: recv messages>",
			    argv[0]);
		return;
	}
	int interval = strtoul(argv[1], NULL, 10);
	int amount = strtoul(argv[2], NULL, 10);
	bool recv = false;
	if (strcmp(argv[3], "true") == 0 || strcmp(argv[3], "1") == 0) {
		recv = true;
	}
	if (amount < 10) {
		shell_error(sh, "Amount must be >= 10");
		return;
	}
	if (interval < 10 || interval > 600) {
		shell_error(sh, "Interval must be between 10 and 600 seconds");
		return;
	}
	interval *= 1000;

	clear_recv(sh, true);
	// Make sure the level shifter is powered on
	gpio_pin_set(gpio_p1, 14, 1);

	ret = set_dpm_mode(sh, true);
	if (ret != 0) {
		shell_error(sh, "Failed to set DPM mode");
		return;
	}

	for (j = 0; j < 3; j++) {
		shell_print(sh, "Waiting %d seconds in DPM sleep", interval / 1000);
		now = k_uptime_get();
		int cnt = 0;
		while (k_uptime_get() < now + interval) {
			k_msleep(500);
			int amt = clear_recv(NULL, false);
			if (amt > 0) {
				shell_fprintf(sh, SHELL_NORMAL, "%d", amt);
			} else {
				shell_fprintf(sh, SHELL_NORMAL, ".");
			}
			cnt++;
			if (cnt % 50) {
				shell_fprintf(sh, SHELL_NORMAL, "\r\n");
			}
		}

		shell_print(sh, "\r\nWake up DA");
		dpm_wake_no_sleep(sh);

		shell_print(sh, "Sending %d bytes", amount);
		send_recv_mqtt(sh, recv, amount);

		shell_print(sh, "Sleeping up DA");
		dpm_back_to_sleep(sh);
	}
}

void do_dpm_set_sleep(const struct shell *sh, size_t argc, char **argv)
{
	bool set_off = true;

	if (argc != 2) {
		shell_error(sh, "Usage: %s <on|off>", argv[0]);
		return;
	}
	if (strcasecmp(argv[1], "on") == 0) {
		set_off = false;
	}
	if (set_off) {
		dpm_wake_no_sleep(sh);
	} else {
		dpm_back_to_sleep(sh);
	}
}

void do_set_dpm_mode(const struct shell *sh, size_t argc, char **argv)
{
	bool turn_on = false;
	int ret;

	if (argc != 2) {
		shell_error(sh, "Usage: %s <on|off>", argv[0]);
		return;
	}
	if (strcasecmp(argv[1], "on") == 0) {
		turn_on = true;
	}
	ret = set_dpm_mode(sh, turn_on);
	if (ret == 0) {
		shell_print(sh, "DPM mode is now %s", turn_on ? "on" : "off");
	}
}

void do_check_dpm(const struct shell *sh, size_t argc, char **argv)
{
	bool is_dpm = check_dpm_mode(sh);
	shell_print(sh, "DPM mode is %s", is_dpm ? "on" : "off");
}

void do_ota(const struct shell *sh, size_t argc, char **argv)
{
	int ret, errors = 0;
	static char cmd[200];
	wifi_wait_array_t wait_msgs;

	if (argc != 3) {
		shell_error(sh, "Usage: %s <https://server:port> <filename>", argv[0]);
		return;
	}
	clear_recv(sh, true);

	shell_print(sh, "Stopping MQTT");
	if (!send_ok_err_atcmd(sh, "AT+NWMQCL=0", K_MSEC(3000))) {
		shell_error(sh, "Error Stopping MQTT service");
		return;
	}

	// AT+NWOTADWSTART=rtos,http://10.1.91.195:9000/DA16200_FRTOS-GEN01-01-UNTRACKED!-231130.img
	sprintf(cmd, "AT+NWOTADWSTART=rtos,%s/%s", argv[1], argv[2]);
	shell_print(sh, "Sending an AT command to download firmware: %s", cmd);
	if (!send_ok_err_atcmd(sh, cmd, K_MSEC(2000))) {
		shell_error(sh, "Error Stopping MQTT service");
		return;
	} else {
		shell_print(sh, "Got a ok response");
	}

	// Now look for a "+NWOTADWSTART:0x00" message to confirm fw was downloaded,it can take time
	wait_msgs.num_msgs = 0; // Initialize the structure
	wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
	wifi_add_wait_msg(&wait_msgs, "\r\nERROR%99s\r\n", true, 1, com_buf);
	wifi_add_wait_msg(&wait_msgs, "+NWOTADWPROG:%3s", true, 1, cmd);
	int32_t amount, last = -1;
	bool download_complete = false;
	while (errors < 5) {
		ret = wifi_send_and_wait_for("AT+NWOTADWPROG=rtos", &wait_msgs, K_MSEC(3000));
		if (ret < 0) {
			shell_error(sh, "Timeout to wait for download progress");
			return;
		}
		if (ret == 2) {
			amount = strtoul(cmd, NULL, 10);
			if (errno == ERANGE) {
				shell_error(sh, "Error parsing progress");
				errors++;
				continue;
			}
			if (amount >= 100) {
				shell_print(sh, "Firmware download complete");
				download_complete = true;
				break;
			}
			if (amount <= last) {
				continue;
			} else {
				shell_fprintf(sh, SHELL_NORMAL, "%d", amount);
			}
			last = amount;
			shell_fprintf(sh, SHELL_NORMAL, ".");
			k_msleep(500);
		}
		if (ret == 1) {
			shell_print(sh, "ERROR:%s", com_buf);
			return;
		}
	}

	if (download_complete == false) {
		shell_fprintf(sh, SHELL_NORMAL, "Never got a firmware download confirmation");
		return;
	}

	shell_print(sh, "Sending an AT command to reboot with new firmware:");
	if (!send_ok_err_atcmd(sh, "AT+NWOTARENEW", K_MSEC(1000))) {
		shell_error(sh, "failed at+renew");
		return;
	}
}

char sh_buf[WIFI_MSG_SIZE];
void do_flush(const struct shell *sh, size_t argc, char **argv)
{
	clear_recv(sh, true); // Flush receive buffer
}

void do_send_atcmd(const struct shell *sh, size_t argc, char **argv)
{
	int ret = 0;
	int printctl = 0;
	k_timeout_t timeout = K_MSEC(4000);
	k_timepoint_t timepoint = sys_timepoint_calc(timeout);
	wifi_wait_array_t wait_msgs;
	char errstr[100];

	if (argc != 2 && argc != 3) {
		shell_error(sh, "Usage: %s <cmd> [display returns yes/no]", argv[0]);
		return;
	}
	if (argc == 3 && strcasecmp(argv[2], "yes") == 0) {
		printctl = 1;
	}

	clear_recv(sh, true); // Flush receive buffer

	ret = wifi_send_timeout(argv[1], K_NO_WAIT);
	if (ret != 0) {
		shell_error(sh, "Failed to send spi %d", ret);
		return;
	}

	while (1) {
		timeout = sys_timepoint_timeout(timepoint);
		wait_msgs.num_msgs = 0; // Initialize the structure
		wifi_add_wait_msg(&wait_msgs, "\r\nOK\r\n", true, 0);
		wifi_add_wait_msg(&wait_msgs, "\r\nERROR%99s\r\n", true, 1, errstr);
		wifi_add_wait_msg(&wait_msgs, "%s", true, 1, com_buf);
		com_buf[0] = 0;
		ret = wifi_wait_for(&wait_msgs, timeout);
		if (ret < 0) {
			shell_error(sh, "Timeout to wait for OK or ERROR");
			return;
		}
		if (ret == 0) {
			shell_print(sh, "OK");
			return;
		}
		if (ret == 1) {
			shell_print(sh, "ERROR:%s", errstr);
			return;
		} else {
			shell_print_ctl(sh, com_buf, true);
			shell_print(sh, "");
		}
	}
}

void do_test_get_da_fw_ver(const struct shell *sh, size_t argc, char **argv)
{
	char ver[60];
	int ret = get_da_fw_ver(ver, 60);
	if (ret != 0) {
		shell_error(sh, "Failed to get fw ver");
	} else {
		shell_print(sh, "FW version is %s", ver);
	}
}

void do_test_get_wfscan(const struct shell *sh, size_t argc, char **argv)
{
	char ver[800];
	int ret = get_wfscan(ver, 800);
	if (ret != 0) {
		shell_error(sh, "Failed to get wfscan");
	} else {
		shell_print(sh, "wfscan:\r\n%s", ver);
	}
}

void do_test_connect_ssid(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 7) {
		shell_error(sh, "Usage: %s ssid key sec keyidx enc hidden ", argv[0]);
		shell_error(sh, "  <ssid>: SSID. 1 ~ 32 characters are allowed");
		shell_error(sh, "  <key>: Passphrase. 8 ~ 63 characters are allowed   or NULL if "
				"sec is 0 or 5");
		shell_error(sh,
			    "  <sec>: Security protocol. 0 (OPEN), 1 (WEP), 2 (WPA), 3 (WPA2), 4 "
			    "(WPA+WPA2) ), 5 (WPA3 OWE), 6 (WPA3 SAE), 7 (WPA2 RSN & WPA3 SAE)");
		shell_error(sh, "  <keyidx>: Key index for WEP. 0~3    ignored if sec is 0,2-7");
		shell_error(sh, "  <enc>: Encryption. 0 (TKIP), 1 (AES), 2 (TKIP+AES)   ignored if "
				"sec is 0,1 or 5");
		shell_error(sh, "  <hidden>: 1 (<ssid> is hidden), 0 (<ssid> is NOT hidden)");
		return;
	}
	k_timeout_t timeout = K_MSEC(6000);
	int ret = connect_to_ssid(argv[1], argv[2], strtoul(argv[3], NULL, 10),
				  strtoul(argv[4], NULL, 10), strtoul(argv[5], NULL, 10),
				  strtoul(argv[6], NULL, 10), timeout);
	if (ret != 0) {
		shell_error(sh, "Failed to connect: %d", ret);
	} else {
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
// <sec>: Security protocol. 0 (OPEN), 1 (WEP), 2 (WPA), 3 (WPA2), 4 (WPA+WPA2) ), 5 (WPA3 OWE), 6
// (WPA3 SAE), 7 (WPA2 RSN & WPA3 SAE) <keyidx>: Key index for WEP. 0~3    ignored if sec is 0,2-7
// <enc>: Encryption. 0 (TKIP), 1 (AES), 2 (TKIP+AES)   ignored if sec is 0,1 or 5
// <hidden>: 1 (<ssid> is hidden), 0 (<ssid> is NOT hidden)
// <timeout>: timeout
int connect_to_ssid(char *ssid, char *key, int sec, int keyidx, int enc, int hidden,
		    k_timeout_t timeout)
{
	int ret = 0;
	char cmd[128];
	wifi_msg_t msg;

	if (strlen(ssid) > 32) {
		LOG_ERR("SSID too long");
		return -1;
	}
	if (sec < 0 || sec > 7) {
		LOG_ERR("Invalid sec");
		return -1;
	}
	if (hidden < 0 || hidden > 1) {
		LOG_ERR("Invalid hidden");
		return -1;
	}

	switch (sec) {
	case 0:
	case 5:
		sprintf(cmd, "AT+WFJAP='%s',%d,%d", ssid, sec, hidden);
		break;
	case 1:
		if (strlen(key) > 63) {
			LOG_ERR("Key too long");
			return -1;
		}
		if (keyidx < 0 || keyidx > 3) {
			LOG_ERR("Invalid keyidx");
			return -1;
		}
		sprintf(cmd, "AT+WFJAP='%s',%d,%d,%s,%d", ssid, sec, keyidx, key, hidden);
		break;
	default:
		if (strlen(key) > 63) {
			LOG_ERR("Key too long");
			return -1;
		}
		if (enc < 0 || enc > 2) {
			LOG_ERR("Invalid enc");
			return -1;
		}
		sprintf(cmd, "AT+WFJAP='%s',%d,%d,%s,%d", ssid, sec, enc, key, hidden);
		break;
	}

	k_timepoint_t timepoint = sys_timepoint_calc(timeout);
	wifi_flush_msgs();

	ret = wifi_send_timeout(cmd, timeout);
	timeout = sys_timepoint_timeout(timepoint);
	if (ret != 0 || K_TIMEOUT_EQ(timeout, K_NO_WAIT) == true) {
		LOG_ERR("Failure or timeout sending at+WFJAP %d", ret);
		return -1;
	}

	while (1) {
		ret = wifi_recv(&msg, timeout);
		if (ret != 0) {
			LOG_ERR("Didn't receive a response %d", ret);
			return -1;
		}
		bool is_deauth = (strstr(msg.data, "DEAUTH") != NULL);
		bool is_ok = (strstr(msg.data, "\r\nOK") != NULL);
		wifi_msg_free(&msg);

		if (is_deauth == false) {
			if (is_ok) {
				return 0;
			} else {
				return -1;
			}
		}
		timeout = sys_timepoint_timeout(timepoint);
		if (K_TIMEOUT_EQ(timeout, K_NO_WAIT) == true) {
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

	char *str = strstr(msg.data, "TDEVER_");
	if (str != NULL) {
		str += 7;
		memcpy(ver + 9, str, 3);
	}
	wifi_msg_free(&msg); // free doesn't use len, so it fine to modify it
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

	while (K_TIMEOUT_EQ(timeout, K_NO_WAIT) == false) {
		if ((ret = wifi_recv(&msg, timeout)) != 0) {
			LOG_ERR("Didn't receive a response to at+wfmac=?, ret= %d", ret);
			return "tde 0022.000000000000";
		}
		int addrs[6];
		int num = sscanf(msg.data, "\r\n+WFMAC:%X:%X:%X:%X:%X:%X", &addrs[0], &addrs[1],
				 &addrs[2], &addrs[3], &addrs[4], &addrs[5]);
		if (num == 6) {
			sprintf(mac, "tde 0022.%02X%02X%02X%02X%02X%02X", addrs[0], addrs[1],
				addrs[2], addrs[3], addrs[4], addrs[5]);
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

	strcpy(result, "tde 0026.0000000000");
	k_timeout_t timeout = K_MSEC(6000);
	k_timepoint_t timepoint = sys_timepoint_calc(timeout);

	// EAS XXX TODO  we need to tie requests to responses so we don't have to flush the queue
	wifi_flush_msgs();

	ret = connect_to_ssid(ssid, pass, 4, 0, 2, 0, timeout);
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
	strcpy(result, "tde 0027.0");
	k_timeout_t timeout = K_MSEC(500);
	k_timepoint_t timepoint = sys_timepoint_calc(timeout);

	// EAS XXX TODO  we need to tie requests to responses so we don't have to flush the queue
	wifi_flush_msgs();

	ret = wifi_send_timeout("AT+WFRSSI", timeout);
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
		char rssi[17];
		if (sub != NULL) {
			sscanf(sub, "+RSSI:%s", rssi);
			sprintf(result, "tde 0027.%s", rssi);
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
	int ret;
	wifi_msg_t msg;
	strcpy(result, "tde 0028.0");
	k_timeout_t timeout = K_MSEC(1500);
	k_timepoint_t timepoint = sys_timepoint_calc(timeout);

	// EAS XXX TODO  we need to tie requests to responses so we don't have to flush the queue
	wifi_flush_msgs();

	ret = wifi_send_timeout("AT+WFSTAT", timeout);
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
			LOG_ERR("Didn't receive a response to WFSTAT in time. ret=%d", ret);
			break;
		}
		char *sub = strstr(msg.data, "+WFSTAT:");
		if (sub != NULL) {
			sub = strstr(msg.data, "wpa_state=COMPLETED");
			if (sub != NULL) {
				strcpy(result, "tde 0028.1");
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
	int macaddr[6];
	static char cmd[50];

	if (sscanf(newmac, "%02X:%02X:%02X:%02X:%02X:%02X", &macaddr[0], &macaddr[1], &macaddr[2],
		   &macaddr[3], &macaddr[4], &macaddr[5]) != 6) {
		LOG_ERR("Invalid mac address");
		return "tde 0058.000000000000";
	}

	wifi_flush_msgs();
	sprintf(cmd, "at+wfspf=%02X:%02X:%02X:%02X:%02X:%02X", macaddr[0], macaddr[1], macaddr[2],
		macaddr[3], macaddr[4], macaddr[5]);
	if (!send_ok_err_atcmd(NULL, cmd, K_MSEC(1000))) {
		LOG_ERR("At command failed");
		return "tde 0058.000000000000";
	}
	sprintf(cmd, "tde 0058.%02X%02X%02X%02X%02X%02X", macaddr[0], macaddr[1], macaddr[2],
		macaddr[3], macaddr[4], macaddr[5]);
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
	int ret = wifi_get_xtal(K_MSEC(1000));
	if (ret != -1) {
		snprintf(result,sizeof(result), "tde 0060.%d", ret);
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
		int ret = wifi_set_xtal(newval, K_MSEC(1000));
		if (ret != -1) {
			sprintf(result, "tde 0061.%d", newval);
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
			sprintf(result, "tde 0062.%d", newval);
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

char tde_usage[] =
	"Usage: test_tde <tde test number "
	"(2=wifiver,22=getMAC,26=connect,27=getRSSI,28=connStatus,58=set mac)> [test params (26 "
	"sdid pass) (58 xx:xx:xx:xx:xx)] 60, 61 <newval>, 62<newval>,63 <0=stop, 1=start>";
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
	case 58:
		if (argc != 3) {
			goto usage;
		}
		res = tde0058(argv[2]);
		break;
	case 60:
		res = tde0060();
		break;
	case 61:
		if (argc != 3) {
			goto usage;
		}
		res = tde0061(atoi(argv[2]));
		break;
	case 62:
		if (argc != 3) {
			goto usage;
		}
		res = tde0062(atoi(argv[2]));
		break;
	case 63:
		if (argc != 3) {
			goto usage;
		}
		res = tde0063(atoi(argv[2]));
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

void cert_upload_cb(const struct shell *sh, uint8_t *data, size_t len)
{
	int i = 0;
	static uint8_t tail;
	bool escape = false;

	/* Check if escape criteria is met. */
	if (tail == CHAR_1 && data[0] == CHAR_2) {
		escape = true;
		// We already put the ctrl-x in the buffer, so remove it
		com_buf[--com_idx] = 0;
	} else {
		for (i = 0; ((i < (len - 1)) && (com_idx < COM_BUF_LEN - 1)); i++) {
			if (data[i] == CHAR_1 && data[i + 1] == CHAR_2) {
				escape = true;
				break;
			} else {
				com_buf[com_idx++] = data[i];
			}
		}
		tail = data[i];
		com_buf[com_idx++] = data[i++];
	}

	if (escape) {
		com_buf[com_idx++] = '\003';
		com_buf[com_idx++] = 0;
		shell_print(sh, "Cert Upload complete");
		shell_print_ctl_n(sh, com_buf, com_idx, false);
		shell_print(sh, "\n\rSending cert to DA");
		if (!send_ok_err_atcmd(sh, com_buf, K_MSEC(1000))) {
			shell_error(sh, "Error sending cert to DA");
		} else {
			g_current_cert = 3; // Provisioned
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
	com_buf[0] = 0x1B;
	com_buf[1] = 'C';
	com_buf[2] = argv[1][0];
	com_buf[3] = ',';
	com_buf[4] = 0;

	com_idx = 4;

	shell_print(sh, "------ Certficate Upload mode, press ctrl-x ctrl-q to end ------\n");

	shell_set_bypass(sh, cert_upload_cb);
}

void do_get_opt_reg(const struct shell *sh, size_t argc, char **argv)
{
	int addr, len;

	if (argc != 3) {
		shell_error(sh, "Usage: %s <[0x]register addr> <size of data>", argv[0]);
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

void do_set_opt_reg(const struct shell *sh, size_t argc, char **argv)
{
	int addr, len, data;

	if (argc != 4) {
		shell_error(sh, "Usage: %s <[0x]register addr> <size of data> <[0x]data>", argv[0]);
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

void do_test_wifi_work(const struct shell *sh, size_t argc, char **argv)
{
	wifi_do_work();
}

SHELL_CMD_REGISTER(
	wifi_at_passthru, NULL,
	"enable AT passthru mode to the DA16200 (deprecated, use 'da16200 wifi_at_passthru')",
	do_wifi_ATPassthru_mode);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_da16200, SHELL_CMD(reset, NULL, "reset the wifi", do_wifi_reset),
	SHELL_CMD(turn_off, NULL, "turn off the wifi", do_wifi_turn_off),
	SHELL_CMD(turn_on, NULL, "turn on the wifi", do_wifi_turn_on),
	SHELL_CMD(power_key, NULL, "set wifi power key line", do_wifi_power_key),
	SHELL_CMD(enable_3v3, NULL, "set wifi 3v0 enable line", do_wifi_set_3v0_enable),
	SHELL_CMD(set_wakeup, NULL, "set wifi wakeup line", do_wifi_set_wakeup),
	SHELL_CMD(insert_certs, NULL,
		  "Send the CA,crt and private key in '<ESC>C' format to the DA, insert_certs "
		  "<0-2>,  0=none (unencrypted/remove certs), 1= mosquitto.org, 2=aws staging",
		  do_insert_certs),
	SHELL_CMD(set_time, NULL,
		  "Set the time on the DA, needed for checking certs, set_time <YYYY> <MM> <DD> "
		  "<HH> <mm>",
		  do_set_time),
	SHELL_CMD(send_mqtt_msg, NULL,
		  "Send a msg to the mqtt server. <num:bytes to send> <bool: recv messages>",
		  do_send_mqtt_msg),
	SHELL_CMD(power_test, NULL,
		  "Send(/recv) X data once every Y seconds to the mqtt server, <num: seconds> "
		  "<num:bytes to send> <bool: recv messages>",
		  do_power_test),
	SHELL_CMD(dpm_mode, NULL, "Set the DA to be in DPM mode or not.  May reboot DA. <on|off>",
		  do_set_dpm_mode),
	SHELL_CMD(ota, NULL, "start a OTA update, <https://server:port> <filename>", do_ota),
	SHELL_CMD(wifi_at_passthru, NULL, "enable AT passthru mode to the DA16200",
		  do_wifi_ATPassthru_mode),
	SHELL_CMD(test_tde, NULL, tde_usage, do_test_tde),
	SHELL_CMD(test_get_wfscan, NULL, "test the get_wfscan() call", do_test_get_wfscan),
	SHELL_CMD(test_get_da_fw_ver, NULL, "test the get_da_fw_ver() call", do_test_get_da_fw_ver),
	SHELL_CMD(test_connect_ssid, NULL,
		  "test the connect_to_ssid() call, ssid key sec keyidx enc hidden",
		  do_test_connect_ssid),
	SHELL_CMD(send_atcmd, NULL,
		  "set an at cmd over wifi to the da, <cmd> [display returns yes/no]",
		  do_send_atcmd),
	SHELL_CMD(flush, NULL, "flush recv q", do_flush),
	SHELL_CMD(level_shifter, NULL, "Set the power on the wifi level shifter, <on|off>",
		  do_level_shifter),
	SHELL_CMD(dpm_set_sleep, NULL,
		  "Wake the DA from DPM mode and tell it to not go back to sleep or tell it to go "
		  "back to sleep. <on|off>",
		  do_dpm_set_sleep),
	SHELL_CMD(check_dpm, NULL, "Check if the dpm mode is set", do_check_dpm),
	SHELL_CMD(upload_cert, NULL,
		  "Upload a an mqtt certificate to the DA. <type, 0=CA, 1=cert, 2=priv_key>",
		  do_upload_cert),
	SHELL_CMD(work_test, NULL, "test the wifi work function that sends SSIDs",
		  do_test_wifi_work),
	SHELL_CMD(get_opt_reg, NULL,
		  "Get the value of a DA otp register  <[0x]register addr> <size of data>",
		  do_get_opt_reg),
	SHELL_CMD(
		set_opt_reg, NULL,
		"Get the value of a DA otp register  <[0x]register addr> <size of data> <[0x]data>",
		do_set_opt_reg),
	SHELL_CMD(get_xtal, NULL, "Get the current value of the XTAL", do_get_xtal),
	SHELL_CMD(start_rf_test_mode, NULL, "Reboot the DA into RF Test mode",
		  do_start_rf_test_mode),
	SHELL_CMD(stop_rf_test_mode, NULL, "Reboot the DA into Normal mode", do_stop_rf_test_mode),

	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(da16200, &sub_da16200, "Commands to control the DA16200", NULL);
