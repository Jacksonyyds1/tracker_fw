
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>

#include <zephyr/logging/log.h>


#include <zephyr/fs/fs.h>
#include "nrf91_upgrade.h"

LOG_MODULE_REGISTER(nrf91_upgrade, LOG_LEVEL_DBG);

struct flash_img_context _flash_img_context;
#define BLOCKSIZE (512)
static uint8_t extra_data[BLOCKSIZE];
static int extra_data_size = 0;
static bool upgrade_started = false;
static uint16_t fw_offset = 0;
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
	off_t addr;
	off_t end;
	int bytes_to_read;
	int rc;
	int i;
	uint8_t erased_val;
	uint32_t erased_val_32;

	__ASSERT_NO_MSG(fa->fa_size % 4 == 0);

	erased_val = flash_area_erased_val(fa);
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
	int err;

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

int nrf91_cancel_upgrade() {
	if (!upgrade_started) {
		LOG_ERR("No upgrade in progress!");
		return -EBUSY;
	}

	upgrade_started = false;
	return 0;
}

int nrf91_start_upgrade() {
	
	if (upgrade_started) {
		LOG_ERR("Upgrade already started!");
		return -EBUSY;
	}

	int rc;


	LOG_DBG("preparing flash image...");
	rc = flash_img_prepare(&_flash_img_context);
	if (rc) {
		LOG_ERR("FAIL: flash_img_prepare: %d", rc);
		return rc;
	}
	upgrade_started = true;
	LOG_DBG("ready to write image to secondary...");

	return 0;
}

int nrf91_upgrade_with_mem_chunk(uint8_t *chunk, uint16_t chunk_size)
{
	int rc; 
	int offset = 0;

	if (extra_data_size > 0) {
		// fill remaining extra_data from chunk and set offset for how much was used
		int remaining = BLOCKSIZE - extra_data_size;
		if (remaining > chunk_size) {
			// chunk fits in extra_data
			memcpy(extra_data + extra_data_size, chunk, chunk_size);
			extra_data_size += chunk_size;
			return 0;
		}
		else {
			// chunk does not fit in extra_data
			memcpy(extra_data + extra_data_size, chunk, remaining);
			rc = fw_update_write_block(extra_data, BLOCKSIZE);
			fw_offset += BLOCKSIZE;
			if (rc) {
				LOG_ERR("Failed to write block: %d", rc);
				return rc;
			}
			else {
				//LOG_DBG("wrote block: %d", offset);
			}
			offset += remaining;
			extra_data_size = 0;
		}
		
	}

	// for every BLOCKSIZE bytes in chunk, write to flash
	for (int i = offset; i < chunk_size; i += BLOCKSIZE) {
		if (i + BLOCKSIZE > chunk_size) {
			// partial block
			// save it for next time
			memcpy(extra_data, chunk + i, chunk_size - i);
			extra_data_size = chunk_size - i;
			break;
		}
		uint8_t block[BLOCKSIZE];
		memcpy(block, chunk + i, BLOCKSIZE);
		rc = fw_update_write_block(block, BLOCKSIZE);
		fw_offset += BLOCKSIZE;
		if (rc) {
			LOG_ERR("Failed to write block: %d", rc);
			return rc;
		}
		else {
			//LOG_DBG("wrote block: %d", i);
		}
	}
	return 0;
}

int nrf91_finish_upgrade() {
	int rc; 

	if (extra_data_size > 0) {
		// if there is anything left in extra_data, write it to flash
		int rc = fw_update_write_block(extra_data, extra_data_size);	
		if (rc) {
			LOG_ERR("Failed to write block: %d", rc);
			return rc;
		}
		else {
			//LOG_DBG("wrote final block");
		}
	}

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
	upgrade_started = false;
	sys_reboot(SYS_REBOOT_COLD);

	return 0;
}

// static void do_upgrade_from_fs(const struct shell *sh, size_t argc, char **argv)
// {
// 	if (argc < 2) {
// 		shell_error(sh, "%s", usage);
// 		return;
// 	}

// 	char *filename = argv[1];

// 	shell_print(sh, "upgrading to %s...!\n", filename);
// 	nrf53_upgrade_with_file(filename);
// }

// SHELL_CMD_REGISTER(upgrade, NULL, "upgrade nRF53 FW from image on filesystem", do_upgrade_from_fs);
