/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */

#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include "commMgr.h"
#include "fota.h"
#include "d1_json.h"
#include <zephyr/fs/fs.h>

void
do_fota_check(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "Usage: check <device_type> [do-update (1 or 0)]");
        shell_error(sh, "device_type: 1 for nrf9160, 2 for da16200, 4 for nrf5340");
        return;
    }
    int  target    = atoi(argv[1]);
    bool do_update = false;

    if (argc == 3) {
        int tmp = atoi(argv[2]);
        if (tmp == 1) {
            shell_print(sh, "Will update if available");
            do_update = true;
        }
    }
    check_for_updates(do_update, target);
}

void
fota_update_all_devices_cmd(const struct shell *sh, size_t argc, char **argv)
{
    fota_update_all_devices();
}

void
fota_add_file_cmd(const struct shell *sh, size_t argc, char **argv)
{
    int              ret;
    char             file_data[65] = { 0 };
    struct fs_file_t update_notes;
    fs_file_t_init(&update_notes);
    fs_open(&update_notes, "/lfs1/fota_in_progress.txt", FS_O_WRITE | FS_O_CREATE);
    snprintf(file_data, 65, "%s\n", "d6e63cdd-4b65-443b-82cc-511b6a5348fc");
    ret = fs_write(&update_notes, file_data, strlen(file_data));
    snprintf(file_data, 65, "%d\n%d\n%d\n", 4, 5, 6);
    ret = fs_write(&update_notes, file_data, strlen(file_data));
    fs_close(&update_notes);
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_fota,
    SHELL_CMD(check, NULL, "check for an optionally do update", do_fota_check),
    SHELL_CMD(update_all, NULL, "update all devices", fota_update_all_devices_cmd),
    SHELL_CMD(write_file, NULL, "bleh", fota_add_file_cmd),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(fota, &sub_fota, "Commands to control the fota", NULL);
