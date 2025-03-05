/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/sys/crc.h>

#include <zcbor_decode.h>
#include <zcbor_common.h>

#include "pmic.h"
#include "storage.h"

#define MAX_PATH_LEN (256)

#define PARTITION_NODE DT_NODELABEL(lfs1)
FS_FSTAB_DECLARE_ENTRY(PARTITION_NODE);

#define MAX_OPEN_FILES (1)

struct file_data_t
{
    char             activity_filename[MAX_PATH_LEN];
    struct fs_file_t activity_file;
    uint16_t         activity_sample_count;
};

static struct file_data_t m_file_data[MAX_OPEN_FILES];
static int                m_open_file_count;

static struct fs_mount_t *mp = &FS_FSTAB_ENTRY(PARTITION_NODE);

static int  lsdir(const char *path);
static void erase_and_reboot(void);

LOG_MODULE_REGISTER(storage, CONFIG_STORAGE_LOG_LEVEL);

int storage_init(void)
{
    struct fs_statvfs sbuf;

    int rc = fs_statvfs(mp->mnt_point, &sbuf);
    if (rc < 0) {
        LOG_ERR("FAIL: statvfs: %d", rc);
        return rc;
    }

    LOG_INF(
        "%s: bsize = %lu ; frsize = %lu ;"
        " blocks = %lu ; bfree = %lu",
        mp->mnt_point,
        sbuf.f_bsize,
        sbuf.f_frsize,
        sbuf.f_blocks,
        sbuf.f_bfree);

    /*
	rc = lsdir(mp->mnt_point);
	if (rc < 0) {
		LOG_ERR("FAIL: lsdir %s: %d", mp->mnt_point, rc);
		return rc;
	}
*/
    return 0;
}

file_handle_t storage_open_file(const char *fname)
{
    if (m_open_file_count >= MAX_OPEN_FILES) {
        LOG_ERR("max open file count exceeded: %d", m_open_file_count);
        return NULL;
    }

    struct file_data_t *file_data = &m_file_data[m_open_file_count];

    // activity file
    snprintf(file_data->activity_filename, MAX_PATH_LEN, "%s/%s", mp->mnt_point, fname);
    file_data->activity_sample_count = 0;
    fs_file_t_init(&file_data->activity_file);
    int rc = fs_open(&file_data->activity_file, file_data->activity_filename, FS_O_CREATE | FS_O_RDWR);
    if (rc < 0) {
        LOG_ERR("FAIL: open %s: %d", file_data->activity_filename, rc);
        return NULL;
    }

    m_open_file_count++;
    return file_data;
}

int storage_close_file(file_handle_t handle)
{
    int ret = fs_close(&handle->activity_file);

    if (ret < 0) {
        LOG_ERR("FAIL: close %s: %d", handle->activity_filename, ret);
        return ret;
    }

    m_open_file_count--;
    return 0;
}

static int storage_print(const struct shell *sh, size_t argc, char **argv)
{
    struct fs_file_t file;
    uint16_t         sample_count = 0;
    char            *fname        = argv[1];

    fs_file_t_init(&file);
    int rc = fs_open(&file, fname, FS_O_RDWR);
    if (rc < 0) {
        LOG_ERR("FAIL: open %s: %d", fname, rc);
        return rc;
    }

    rc = fs_seek(&file, 0, FS_SEEK_SET);
    if (rc < 0) {
        LOG_ERR("FAIL: seek %s: %d", fname, rc);
        return rc;
    }

    for (;;) {
        sample_count++;
        break;
    }

    rc = fs_close(&file);
    if (rc < 0) {
        LOG_ERR("FAIL: close %s: %d", fname, rc);
        return rc;
    }

    return 0;
}

#define STORAGE_DUMP_PARAMS "<filename> <0-2 0-dump,nocrc, 1-dump,crc 2-just crc> [start_byte] [end_byte] "
static int storage_dump(const struct shell *sh, size_t argc, char **argv)
{
    struct fs_file_t file;
    struct fs_dirent dirent;
    char            *fname = argv[1];

    if (argc < 3 || argc > 5) {
        LOG_ERR("usage: storage_dump " STORAGE_DUMP_PARAMS);
        return -1;
    }
    uint32_t start_byte = 0;
    if (argc > 3) {
        start_byte = strtoul(argv[3], NULL, 0);
        shell_print(sh, "start_byte: %d", start_byte);
    }

    int print = 0;
    if (argc > 2) {
        print = strtoul(argv[2], NULL, 0);
        shell_print(sh, "print: %d", print);
    }

    int rc = fs_stat(fname, &dirent);
    if (rc >= 0) {
        shell_print(sh, "\tfn '%s' siz %u", dirent.name, dirent.size);
    } else {
        LOG_ERR("cannot stat %s", fname);
        return -1;
    }
    uint32_t end_byte = dirent.size;
    if (argc > 4) {
        int end  = strtoul(argv[4], NULL, 0);
        end_byte = MIN(end, end_byte);
    }
    shell_print(sh, "end_byte: %d", end_byte);

    fs_file_t_init(&file);
    rc = fs_open(&file, fname, FS_O_RDWR);
    if (rc < 0) {
        LOG_ERR("FAIL: open %s: %d", fname, rc);
        return rc;
    }

    rc = fs_seek(&file, start_byte, FS_SEEK_SET);
    if (rc < 0) {
        LOG_ERR("FAIL: seek %s: %d", fname, rc);
        return rc;
    }

    uint32_t crc32  = 0;
    uint32_t amount = 0;
    for (;;) {
        char buf[260];
        char index[20];
        rc = fs_read(&file, &buf, 256);
        if (rc == 0) {
            LOG_INF("complete");
            break;
        } else if (rc < 0 || rc > 256) {
            LOG_ERR("FAIL: read %s: [rd:%d]", fname, rc);
            break;
        }
        if (amount + rc > end_byte) {
            rc = end_byte - amount;
        }
        if (print == 1 || print == 2) {
            if (amount == 0) {
                crc32 = crc32_ieee(buf, rc);
            } else {
                crc32 = crc32_ieee_update(crc32, buf, rc);
            }
        }
        sprintf(index, "idx: 0x%X:", amount);
        if (print < 2) {
            LOG_PANIC();    // flush
            LOG_HEXDUMP_INF(buf, rc, index);
        }
        amount += rc;
        if (amount >= end_byte) {
            break;
        }
    }
    if (print == 1 || print == 2) {
        LOG_INF("crc32: %08x", crc32);
    }
    rc = fs_close(&file);
    if (rc < 0) {
        LOG_ERR("FAIL: close %s: %d", fname, rc);
        return rc;
    }

    return 0;
}

static int erase_flash(unsigned int id)
{
    const struct flash_area *pfa;
    int                      rc;

    rc = flash_area_open(id, &pfa);
    if (rc < 0) {
        LOG_ERR("FAIL: unable to find flash area %u: %d", id, rc);
        return rc;
    }

    LOG_DBG(
        "Area %u at 0x%x on %s for %u bytes", id, (unsigned int)pfa->fa_off, pfa->fa_dev->name, (unsigned int)pfa->fa_size);

    rc = flash_area_erase(pfa, 0, pfa->fa_size);
    LOG_ERR("Erasing flash area ... %d", rc);

    flash_area_close(pfa);
    return rc;
}

static int storage_erase(const struct shell *sh, size_t argc, char **argv)
{
    int rc = erase_flash((uintptr_t)mp->storage_dev);
    if (rc < 0) {
        LOG_ERR("failure: erase_flash rc=%d", rc);
        return rc;
    }

    return 0;
}

static int storage_ls(const struct shell *sh, size_t argc, char **argv)
{
    int rc = lsdir(mp->mnt_point);
    if (rc < 0) {
        LOG_ERR("FAIL: lsdir %s: %d", mp->mnt_point, rc);
        return rc;
    }

    return 0;
}

static int lsdir(const char *path)
{
    int                     res;
    struct fs_dir_t         dirp;
    static struct fs_dirent entry;

    fs_dir_t_init(&dirp);

    /* Verify fs_opendir() */
    res = fs_opendir(&dirp, path);
    if (res) {
        LOG_ERR("Error opening dir %s [%d]", path, res);
        return res;
    }

    LOG_DBG("Listing dir %s ...", path);
    for (;;) {
        /* Verify fs_readdir() */
        res = fs_readdir(&dirp, &entry);

        /* entry.name[0] == 0 means end-of-dir */
        if (res || entry.name[0] == 0) {
            if (res < 0) {
                LOG_ERR("Error reading dir [%d]", res);
            }
            break;
        }

        if (entry.type == FS_DIR_ENTRY_DIR) {
            LOG_INF("[DIR ] %s", entry.name);
        } else {
            LOG_INF("[FILE] %s (size = %zu)", entry.name, entry.size);
        }
    }

    /* Verify fs_closedir() */
    fs_closedir(&dirp);

    return res;
}

/************************************
 * Activity related interfaces
 ************************************/
int storage_delete_activity_file(char *basename)
{
    char fname[128];
    snprintf(fname, sizeof(fname), "%s/%s", mp->mnt_point, basename);
    int ret = fs_unlink(fname);
    if (ret < 0) {
        LOG_ERR("error unlinking %s", fname);
        return ret;
    }
    return 0;
}

int storage_write_activity_record(file_handle_t handle, const uint8_t *data, size_t nbytes)
{
    int rc = fs_write(&handle->activity_file, &nbytes, sizeof(size_t));
    if (rc > 0) {
        rc = fs_write(&handle->activity_file, data, nbytes);
    }
    if (rc <= 0) {
        LOG_ERR("FAIL: write %s: %d", handle->activity_filename, rc);
        erase_and_reboot();
        return -1;
    }
    return 0;
}

#if 0
int storage_get_activity_record_count(char *basename)
{
	struct fs_dirent dirent;
	char fname[MAX_PATH_LEN];

	snprintf(fname, sizeof(fname), "%s/%s", mp->mnt_point, basename);

	int rc = fs_stat(fname, &dirent);

	if (rc >= 0) {
		LOG_INF("\tfn '%s' siz %u", dirent.name, dirent.size);
	} else {
		LOG_ERR("cannot stat %s", basename);
		return -1;
	}

	int records = dirent.size / sizeof(activity_record_t);

	LOG_DBG("%s records: %d", fname, records);
	return records;
}
#endif

int storage_read_activity_record(char *basename, int record_number, uint8_t *data, size_t dsize)
{
    struct fs_file_t file;
    char             fname[MAX_PATH_LEN];
    uint8_t          record[64];
    size_t           nbytes;

    snprintf(fname, sizeof(fname), "%s/%s", mp->mnt_point, basename);

    fs_file_t_init(&file);
    int rc = fs_open(&file, fname, FS_O_RDWR);
    if (rc < 0) {
        LOG_ERR("FAIL: open %s: %d", fname, rc);
        return rc;
    }

    int seek_offset = 0;
    rc              = fs_seek(&file, seek_offset, FS_SEEK_SET);
    if (rc < 0) {
        LOG_ERR("FAIL: seek %s: %d", fname, rc);
        return rc;
    }

    int recno = 0;
    do {
        rc = fs_read(&file, &nbytes, sizeof(nbytes));
        if (rc > 0) {
            rc = fs_read(&file, record, nbytes);
        }
        if (rc == 0) {
            LOG_ERR("FAIL: read %s: [rd:%d]", fname, rc);
            return rc;
        }
        recno++;
    } while (recno < record_number);

    rc = fs_close(&file);
    if (rc < 0) {
        LOG_ERR("FAIL: close %s: %d", fname, rc);
        return -1;
    }
    if (nbytes <= dsize) {
        memcpy(data, record, nbytes);
        return 0;
    } else {
        LOG_ERR("FAIL: record too big (%zd > %zd)", nbytes, dsize);
        return -1;
    }

    return 0;
}

// if we hit flash errors, it's likely because we ran out of disk space
// in this case, erase the flash and reboot to put the device back into a usable state
static void erase_and_reboot(void)
{
    LOG_ERR("erasing flash!");
    int rc = erase_flash((uintptr_t)mp->storage_dev);
    if (rc < 0) {
        LOG_ERR("failure: erase_flash rc=%d", rc);
    }

    pmic_save_reset_reason("Out of storage");
    LOG_ERR("rebooting device!");
    sys_reboot(SYS_REBOOT_COLD);
}

SHELL_CMD_REGISTER(storage_ls, NULL, "list stored sample files", storage_ls);
SHELL_CMD_REGISTER(storage_print, NULL, "print stored samples for <filename> to console", storage_print);
SHELL_CMD_REGISTER(storage_dump, NULL, "Hexdump a file to console. " STORAGE_DUMP_PARAMS, storage_dump);
SHELL_CMD_REGISTER(storage_erase, NULL, "erase stored samples", storage_erase);
