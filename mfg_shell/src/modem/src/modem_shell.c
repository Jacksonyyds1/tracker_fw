#include "modem.h"

#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <string.h>
#include "d1_json.h"

#define CHAR_1       0x18
#define CHAR_2       0x11
#define SEND_BUF_LEN 1280
char modem_send_buf[SEND_BUF_LEN];
int modem_send_buf_length = 0;

void modem_rx_callback(uint8_t *data, size_t len, void *user_data)
{
	// struct shell *sh = (struct shell *)user_data;
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
	// uint8_t rbuf[SEND_BUF_LEN];
	if (modem_send_buf_length == 0) {
		memset(modem_send_buf, 0, SEND_BUF_LEN);
	}

	bool modem_string_complete = false;
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
		modem_set_bypass(sh, NULL);
		tail = 0;
		return;
	}

	for (int i = 0; i < len; i++) {
		if (modem_send_buf_length < SEND_BUF_LEN) {
			if ((tail == '\r' && data[i] == '\n') ||
			    ((i > 0) && (data[i - 1] == '\r' && data[i] == '\n')) ||
			    data[i] == '\0') {
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
		int my_handle = modem_send_command(MESSAGE_TYPE_AT, modem_send_buf,
						   modem_send_buf_length, true);
		uint8_t buf[SEND_BUF_LEN];
		uint16_t len = 2048;
		k_sleep(K_MSEC(500)); // Wait a tiny bit so we have time for the data to go out and
				      // get acted on.
		// TODO -  this probably scales with the data length
		if (modem_recv_resp(my_handle, buf, &len, 10000) == 0) {
			buf[len] = '\0';
			shell_print(sh, "%s\n", buf);
			modem_free_reply_data(my_handle);
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
	k_msleep(1000);
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

void do_modem_send(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t buf[256];
	shell_print(sh, "Sending to modem...\n");

	modem_send_command(MESSAGE_TYPE_AT, argv[1], strlen(argv[1]), NULL);
	shell_print(sh, "Modem returned: %s\n", buf);
}

void do_modem_send_json(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t buf[256];
	shell_print(sh, "Sending to modem...\n");
	modem_send_command(MESSAGE_TYPE_JSON, argv[1], strlen(argv[1]), NULL);
	shell_print(sh, "Modem returned: %s\n", buf);
}

void do_send_ssid_json(const struct shell *sh, size_t argc, char **argv)
{
#if 0
	if (argc <= 1) {
		shell_print(sh, "usage: send_ssids <num ssids>\n");
		return;
	}

	wifi_arr_t *wifi_top = malloc(sizeof(wifi_arr_t));
	if (!wifi_top) {
		shell_print(sh, "allocation failed\n");
	}

	int num_ssids = atoi(argv[1]);
	if (num_ssids > MAX_WIFI_OBJS) {
		num_ssids = MAX_WIFI_OBJS;
	}

	wifi_top->count = 0;

	for (int i = 0; i < num_ssids; i++) {
		sprintf(wifi_top->wifi[i].ssid, "%02x:%02x:%02x:%02x:%02x:%02x", i, i, i, i, i, i);
		wifi_top->wifi[i].rssi = -i * 11;
		wifi_top->count++;
	}

	char *retStr = json_wifi_data(wifi_top, 1.0);
	shell_print(sh, "JSON(%d) = %s\n", strlen(retStr), retStr);
	free(wifi_top);

	// send to spi
	modem_send_command(MESSAGE_TYPE_JSON, retStr, strlen(retStr), false);
#endif
}

void do_send_raw(const struct shell *sh, size_t argc, char **argv)
{
	if (argc <= 1) {
		shell_print(sh, "usage: send_raw <string_to_send> \n");
		return;
	}

	// send to spi
	int my_handle = modem_send_command(MESSAGE_TYPE_JSON, argv[1], strlen(argv[1]), true);
	uint8_t buf[256];
	uint16_t len = 256;
	if (modem_recv_resp(my_handle, buf, &len, 10000) == 0) {
		shell_print(sh, "Modem returned(%d): %d\n", len, buf[0]);
	} else {
		shell_print(sh, "Modem response timed out\n");
	}
	modem_free_reply_data(my_handle);
}

void do_send_at(const struct shell *sh, size_t argc, char **argv)
{
	if (argc <= 1) {
		shell_print(sh, "usage: send_at <string_to_send> \n");
		return;
	}

	// send to spi
	int my_handle = modem_send_command(MESSAGE_TYPE_AT, argv[1], strlen(argv[1]), true);
	shell_print(sh, "Sending AT command: %s\n", argv[1]);
	uint8_t buf[1024];
	uint16_t len = 1024;
	if (modem_recv_resp(my_handle, buf, &len, 3000) == 0) {
		shell_print(sh, "Modem returned(%d): %d\n", len, buf[0]);
		buf[len] = '\0';
		shell_print(sh, "Modem returned AT response(%d): %s\n", len, buf);
		modem_free_reply_data(my_handle);
	} else {
		shell_print(sh, "Modem response timed out\n");
	}
}

void do_enable_modem(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "Sending modem start command\n");
	uint8_t cmd = COMMAND_START;
	modem_send_command(MESSAGE_TYPE_COMMAND, &cmd, 1, false);
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_nrf9160, SHELL_CMD(reset, NULL, "reset the modem", do_modem_reset),
	SHELL_CMD(turn_off, NULL, "turn off the modem", do_modem_turn_off),
	SHELL_CMD(turn_on, NULL, "turn on the modem", do_modem_turn_on),
	SHELL_CMD(send_json, NULL, "send json string to the modem", do_modem_send_json),
	SHELL_CMD(uart_passthru, NULL, "enable AT passthru mode to the 9160", do_ATPassthru_mode),
	SHELL_CMD(spi_send, NULL, "send a command to the modem", do_modem_send),
	SHELL_CMD(send_ssids, NULL, "send a command fake SSID array to the modem",
		  do_send_ssid_json),
	SHELL_CMD(send_raw, NULL, "send a raw string to the modem", do_send_raw),
	SHELL_CMD(send_at, NULL, "send an AT string to the modem", do_send_at),
	SHELL_CMD(enable_modem, NULL, "send a command to enable the modem", do_enable_modem),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(nrf9160, &sub_nrf9160, "Commands to control the nrf9160", NULL);
SHELL_CMD_REGISTER(9160_at_passthru, NULL,
		   "enable AT passthru mode to the 9160 (deprecated, use 'nrf9160 uart_passthru')",
		   do_ATPassthru_mode);
