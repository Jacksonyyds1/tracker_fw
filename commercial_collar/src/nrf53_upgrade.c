/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */

#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#include <zephyr/fs/fs.h>
#include "watchdog.h"
#include "pmic.h"
#include "nrf53_upgrade.h"
#include "tracker_service.h"

LOG_MODULE_REGISTER(nrf53_upgrade, LOG_LEVEL_DBG);

struct flash_img_context _flash_img_context;

/*
 * @note This is a copy of ERASED_VAL_32() from mcumgr.
 */
#define ERASED_VAL_32(x) (((x) << 24) | ((x) << 16) | ((x) << 8) | (x))

/**
 * Determines if the specified area of flash is completely unwritten.
 *
 * @note This is a copy of img_mgmt_flash_check_empty() from mcumgr.
 */
static int flash_area_check_empty(const struct flash_area *fa, bool *out_empty)
{
    uint32_t data[16];
    off_t    addr;
    off_t    end;
    int      bytes_to_read;
    int      rc;
    int      i;
    uint8_t  erased_val;
    uint32_t erased_val_32;

    __ASSERT_NO_MSG(fa->fa_size % 4 == 0);

    erased_val    = flash_area_erased_val(fa);
    erased_val_32 = ERASED_VAL_32(erased_val);

    end = fa->fa_size;
    for (addr = 0; addr < end; addr += sizeof(data)) {
        if (end - addr < sizeof(data)) {
            bytes_to_read = end - addr;
        } else {
            bytes_to_read = sizeof(data);
        }

        rc = flash_area_read(fa, addr, data, bytes_to_read);
        if (rc != 0) {
            flash_area_close(fa);
            return rc;
        }

        for (i = 0; i < bytes_to_read / 4; i++) {
            if (data[i] != erased_val_32) {
                *out_empty = false;
                flash_area_close(fa);
                return 0;
            }
        }
    }

    *out_empty = true;

    return 0;
}

static int flash_img_erase_if_needed(struct flash_img_context *ctx)
{
    bool empty;
    int  err;

    if (IS_ENABLED(CONFIG_IMG_ERASE_PROGRESSIVELY)) {
        return 0;
    }

    err = flash_area_check_empty(ctx->flash_area, &empty);
    if (err) {
        return err;
    }

    if (empty) {
        return 0;
    }

    err = flash_area_erase(ctx->flash_area, 0, ctx->flash_area->fa_size);
    if (err) {
        return err;
    }

    return 0;
}

static const char *swap_type_str(int swap_type)
{
    switch (swap_type) {
    case BOOT_SWAP_TYPE_NONE:
        return "none";
    case BOOT_SWAP_TYPE_TEST:
        return "test";
    case BOOT_SWAP_TYPE_PERM:
        return "perm";
    case BOOT_SWAP_TYPE_REVERT:
        return "revert";
    case BOOT_SWAP_TYPE_FAIL:
        return "fail";
    }

    return "unknown";
}

static int flash_img_prepare(struct flash_img_context *flash)
{
    int swap_type;
    int err;

    swap_type = mcuboot_swap_type();
    switch (swap_type) {
    case BOOT_SWAP_TYPE_REVERT:
        LOG_WRN("'revert' swap type detected, it is not safe to continue");
        return -EBUSY;
    default:
        LOG_INF("swap type: %s", swap_type_str(swap_type));
        break;
    }

    err = flash_img_init(flash);
    if (err) {
        LOG_ERR("failed to init: %d", err);
        return err;
    }

    err = flash_img_erase_if_needed(flash);
    if (err) {
        LOG_ERR("failed to erase: %d", err);
        return err;
    }

    return 0;
}

static int fw_update_write_block(const uint8_t *block, size_t block_size)
{
    int err;

    err = flash_img_buffered_write(&_flash_img_context, block, block_size, false);
    if (err) {
        LOG_ERR("Failed to write to flash: %d", err);
        return err;
    }

    return 0;
}

static void fw_update_finish(void)
{
    int err;

    if (!_flash_img_context.stream.buf_bytes) {
        return;
    }

    err = flash_img_buffered_write(&_flash_img_context, NULL, 0, true);
    if (err) {
        LOG_ERR("Failed to write to flash: %d", err);
    }
}

static char usage[] = "Usage: upgrade <filename>";

int nrf53_upgrade_with_file(char *fname)
{
    int              rc;
    struct fs_file_t file;
    fs_file_t_init(&file);

    rc = fs_open(&file, fname, FS_O_READ);
    if (rc < 0) {
        LOG_ERR("FAIL: open %s: %d", fname, rc);
        return rc;
    }

    LOG_DBG("opened %s for reading...", fname);

    rc = fs_seek(&file, 0, FS_SEEK_SET);
    if (rc < 0) {
        LOG_ERR("FAIL: seek %s: %d", fname, rc);
        return rc;
    }

    LOG_DBG("preparing flash image...");
    rc = flash_img_prepare(&_flash_img_context);
    if (rc) {
        LOG_ERR("FAIL: flash_img_prepare: %d", rc);
        return rc;
    }

    LOG_DBG("writing image to secondary...");

    int i = 0;
    for (;;) {
#define BLOCKSIZE (512)
        watchdog_kick();
        static uint8_t block[BLOCKSIZE];
        rc = fs_read(&file, &block, BLOCKSIZE);
        if (rc >= 0) {
            fw_update_write_block(block, rc);
            // Logging less frequently to allow scrollback buffer in terminal to be less full
            if (i % 10) {
                i++;
            } else {
                LOG_DBG("wrote block: %d", i++);
            }
            if (rc < BLOCKSIZE) {
                LOG_DBG("wrote last block!");
                break;
            }
        } else {
            LOG_ERR("FAIL: read %s: [rd:%d]", fname, rc);
            break;
        }
    }

    rc = fs_close(&file);

    if (rc < 0) {
        LOG_ERR("FAIL: close %s: %d", fname, rc);
        return rc;
    }

    LOG_DBG("synching file...");
    fw_update_finish();

    rc = boot_write_img_confirmed();
    if (rc) {
        LOG_ERR("boot_write_img_confirmed: %d", rc);
        return rc;
    }

    rc = boot_request_upgrade(BOOT_UPGRADE_PERMANENT);
    if (rc) {
        LOG_ERR("boot_request_upgrade: %d", rc);
        return rc;
    }

    LOG_DBG("upgrade complete, rebooting!");
    pmic_save_reset_reason("Upgrade complete");
    fota_notify("5340_REBOOTING");
    k_sleep(K_SECONDS(1));
    sys_reboot(SYS_REBOOT_COLD);

    return 0;
}

static void do_upgrade_from_fs(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_error(sh, "%s", usage);
        return;
    }

    char *filename = argv[1];

    shell_print(sh, "upgrading to %s...!\n", filename);
    nrf53_upgrade_with_file(filename);
}

SHELL_CMD_REGISTER(upgrade, NULL, "upgrade nRF53 FW from image on filesystem", do_upgrade_from_fs);
