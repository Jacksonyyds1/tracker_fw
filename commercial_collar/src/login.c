/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/fs/fs.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/iterable_sections.h>
#include <tinycrypt/sha256.h>
#include <tinycrypt/constants.h>
#include <nrf5340_application.h>
#include <hal/nrf_uarte.h>

#include <stdlib.h>
#include <ctype.h>

#include "app_version.h"
#include "modem.h"
#include "uicr.h"

LOG_MODULE_REGISTER(login, LOG_LEVEL_ERR);

#define CONSOLE DT_CHOSEN(zephyr_console)

static bool should_console_be_disabled = true;

void login_init(void)
{
    if (!CONFIG_SHELL_CMD_ROOT[0]) {
        shell_set_root_cmd("login");
    }
}

/*
 * Use a salted password to avoid obvious use of the IMEI.
 * The salt is "culvert" padded out to the length of the IMEI.
 * The IMEI is 15 decimal digits and
 * the format of the IMEI is AABBBBBBCCCCCCD
 * where CCCCCC is the serial number and D is a check digit.
 * All of the characters fall into the ASCII range 0x30 - 0x39
 * The salt used is ASCII lower case alpha chars.
 * XORing a digit with an alpha always results in a printable (and thus typeable)
 * character.
 */
static char salt_string[15] = {
    0x41,    // A
    0x41,    // A
    0x42,    // B
    0x42,    // B
    0x42,    // B
    0x42,    // B
    0x42,    // B
    0x42,    // B
    0x63,    // c
    0x75,    // u
    0x6c,    // l
    0x76,    // v
    0x65,    // e
    0x72,    // r
    0x74     // t
};

static void salt(char *pw)
{
    if (strlen(pw) > sizeof(salt_string)) {
        // it is an invalid password, so don't even think about it!
        return;
    }
    for (int i = 0; i < strlen(pw); i++) {
        pw[i] ^= salt_string[i];
    }
}

static int check_passwd(char *passwd, const struct shell *sh)
{
    char imei[48];
    memset(imei, 0, sizeof(imei));
    modem_get_IMEI(imei, sizeof(imei));
    // the IMEI is exactly 15 chars
    imei[15] = 0;
    if (imei[0]) {
        salt(passwd);
        return strcmp(passwd, imei);
    } else {
        // unable to get the IMEI ... use backdoor password in this case.
        // Ditch+Tunnel=Culvert
        struct tc_sha256_state_struct s;
        uint8_t                       digest[TC_SHA256_DIGEST_SIZE];
        (void)tc_sha256_init(&s);
        tc_sha256_update(&s, (const uint8_t *)passwd, strlen(passwd));
        (void)tc_sha256_final(digest, &s);
        uint8_t hash[TC_SHA256_DIGEST_SIZE] = { 0xbd, 0xdb, 0x04, 0x21, 0x98, 0x61, 0x12, 0x02, 0x0a, 0x45, 0xec,
                                                0x28, 0x1f, 0x41, 0x86, 0xab, 0x3b, 0xef, 0xfd, 0x54, 0x7f, 0x2a,
                                                0xf0, 0x2e, 0x2f, 0xc5, 0x5e, 0x83, 0x1d, 0x96, 0x63, 0x13 };
        shell_print(sh, "No modem!");
        return memcmp(digest, hash, TC_SHA256_DIGEST_SIZE);
    }
}

static int cmd_login(const struct shell *sh, size_t argc, char **argv)
{
    static uint32_t attempts;

    if (check_passwd(argv[1], sh) != 0) {
        shell_error(sh, "Incorrect password!");
        attempts++;
        if (attempts > 3) {
            k_sleep(K_SECONDS(attempts));
        }
        z_shell_history_purge(sh->history);
        return -EINVAL;
    }

    /* clear history so password not visible there */
    z_shell_history_purge(sh->history);
    shell_obscure_set(sh, false);
    shell_set_root_cmd(NULL);
    shell_prompt_change(sh, "ccollar:~$ ");
    shell_print(
        sh, "Commercial Collar Version  %d.%d.%d (%s)", APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_VERSION_PATCH, GIT_HASH);
    attempts = 0;
    shell_print(sh, "Enabling shell log backend");
    z_shell_log_backend_enable(sh->log_backend, (void *)sh, LOG_LEVEL_DBG);
    return 0;
}

static int cmd_logout(const struct shell *sh, size_t argc, char **argv)
{
    shell_set_root_cmd("login");
    shell_obscure_set(sh, true);
    shell_prompt_change(sh, "login: ");
    shell_print(sh, "\n");
    z_shell_history_purge(sh->history);
    return 0;
}

static const char *persist = "/lfs1/uart.txt";
static void        disable_console_on_timer(struct k_timer *id);
K_TIMER_DEFINE(console_off, disable_console_on_timer, NULL);

static bool is_persistent_console(void)
{
    struct fs_dirent entry;
    return (fs_stat(persist, &entry) == 0);
}

static void set_persistent_console(bool should_persist)
{
    struct fs_file_t entry;

    fs_file_t_init(&entry);
    if (should_persist) {
        fs_open(&entry, persist, FS_O_CREATE);
        fs_close(&entry);
    } else {
        fs_unlink(persist);
    }
}

void logout(void)
{
    STRUCT_SECTION_FOREACH(shell, sh)
    {
        cmd_logout(sh, 0, NULL);
    }
    // possibly arrange for the console to be disabled in a while
    should_console_be_disabled = !is_persistent_console();
    if (CONFIG_DISABLE_UART_TIME > 0 && should_console_be_disabled) {
        k_timer_start(&console_off, K_SECONDS(CONFIG_DISABLE_UART_TIME), K_NO_WAIT);
    }
}

SHELL_COND_CMD_ARG_REGISTER(CONFIG_RELEASE_BUILD, login, NULL, "<password>", cmd_login, 2, 0);

SHELL_CMD_REGISTER(logout, NULL, "Log out.", cmd_logout);

/**
 * Overriding: this check can be overridden by pulling test point M62 low.
 * It can also be overridden by compiling with CONFIG_RELEASE_CONSOLE=y, *provided* that the
 * APPROTECT uicr flag is not set. Real, actual shipped, units can only be overridden by shorting
 * M62.
 */

bool disable_console(void)
{
    const struct gpio_dt_spec override = GPIO_DT_SPEC_GET(DT_NODELABEL(override), gpios);

    gpio_pin_configure_dt(&override, GPIO_INPUT | GPIO_PULL_UP | GPIO_ACTIVE_LOW);
    if (gpio_pin_get_dt(&override)) {
        return false;
    }
    // disable the shell log backend ... it should be off anyway since
    // release builds start in OBSCURED mode which disables logging. Just in case ...
    LOG_PANIC();
    STRUCT_SECTION_FOREACH(shell, sh)
    {
        z_shell_log_backend_disable(sh->log_backend);
        // now signal the shell to abort
        struct k_poll_signal *sig = &sh->ctx->signals[SHELL_SIGNAL_KILL];

        k_poll_signal_raise(sig, true);
    }
    // put the console into the low power state
    const struct device *console = DEVICE_DT_GET(CONSOLE);

    pm_device_action_run(console, PM_DEVICE_ACTION_SUSPEND);
    // for now, hard code the uart address etc.
    NRF_UARTE_Type *uart0 = (NRF_UARTE_Type *)NRF_UARTE0_S_BASE;
    nrf_uarte_disable(uart0);

    LOG_ERR("You should not see this");
    return 0;
}

static void disable_console_on_timer(struct k_timer *id)
{
    ARG_UNUSED(id);
    if (should_console_be_disabled) {
        disable_console();
    }
}

static void console_enable(const struct shell *sh, size_t argc, char **argv)
{
    should_console_be_disabled = false;
    k_timer_stop(&console_off);
    if (argc > 1 && (argv[1][0] & 'P') == 'P') {
        set_persistent_console(true);
        shell_print(sh, "Persistent console enabled");
    }
}

static void test_disable(const struct shell *sh, size_t argc, char **argv)
{
    set_persistent_console(false);
    disable_console();
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_console,
    SHELL_CMD(enable, NULL, "Stop the console from disabling", console_enable),
    SHELL_CMD(disable, NULL, "Disable the console immediately", test_disable),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(console, &sub_console, "Control the UART", NULL);