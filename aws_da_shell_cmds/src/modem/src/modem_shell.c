#include "modem.h"

#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <stdlib.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <string.h>


#define CHAR_1 0x18
#define CHAR_2 0x11
#define SEND_BUF_LEN 1024
char modem_send_buf[SEND_BUF_LEN];
uint8_t modem_send_buf_ptr = 0;


void modem_rx_callback(uint8_t *data, size_t len, void *user_data)
{
    //struct shell *sh = (struct shell *)user_data;
    for(int i=0;i<len;i++) {
        //shell_fprintf(sh, SHELL_INFO, "%c", data[i]);  // causes race condition and hangs MCU - SMR
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
    uint8_t rbuf[SEND_BUF_LEN];
    if (modem_send_buf_ptr == 0) {
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

    for (int i=0;i<len;i++) {
        if (modem_send_buf_ptr < SEND_BUF_LEN) {
            if (data[i] == '\n' || data[i] == '\r' || data[i] == '\0') {
                modem_send_buf[modem_send_buf_ptr] = '\r';
                modem_send_buf_ptr++;
                modem_string_complete = true;
            }
            else {
                modem_send_buf[modem_send_buf_ptr] = data[i];
                modem_send_buf_ptr++;
            }
        }
    }
	/* Store last byte for escape sequence detection */
	tail = data[len - 1];



    if (modem_string_complete) {
        modem_send(modem_send_buf);
        modem_send_buf_ptr = 0;
        memset(modem_send_buf, 0, SEND_BUF_LEN);
        while (modem_recv(rbuf, K_MSEC(1000)) == 0) {
            shell_fprintf(sh, SHELL_INFO, "%s", rbuf);
        }
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


#if (CONFIG_USE_UART_TO_NRF9160)
SHELL_CMD_REGISTER(9160_at_passthru, NULL, "enable AT passthru mode to the 9160 (deprecated, use 'nrf9160 uart_passthru')", do_ATPassthru_mode);
#endif

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_nrf9160,
    SHELL_CMD(reset, NULL, "reset the modem", do_modem_reset),
    SHELL_CMD(turn_off, NULL, "turn off the modem", do_modem_turn_off),
    SHELL_CMD(turn_on, NULL, "turn on the modem", do_modem_turn_on),
#if (CONFIG_USE_UART_TO_NRF9160)
    SHELL_CMD(uart_passthru, NULL, "enable AT passthru mode to the 9160", do_ATPassthru_mode),
#endif
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(nrf9160, &sub_nrf9160, "Commands to control the nrf9160", NULL);
