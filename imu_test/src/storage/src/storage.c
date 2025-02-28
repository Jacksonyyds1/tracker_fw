#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>

#include "imu.h"
#include "storage.h"

#define PARTITION_NODE DT_NODELABEL(lfs1)
FS_FSTAB_DECLARE_ENTRY(PARTITION_NODE);

#define MAX_PATH_LEN (255)

static struct fs_mount_t *mp = &FS_FSTAB_ENTRY(PARTITION_NODE);
static char filename[MAX_PATH_LEN];
static struct fs_file_t file;
static uint16_t filecount, samplecount;

static int lsdir(const char *path);

LOG_MODULE_REGISTER(storage, LOG_LEVEL_DBG);

int storage_init(void)
{
    struct fs_statvfs sbuf;

    int rc = fs_statvfs(mp->mnt_point, &sbuf);
    if (rc < 0)
    {
        LOG_ERR("FAIL: statvfs: %d", rc);
        return rc;
    }

    LOG_INF("%s: bsize = %lu ; frsize = %lu ;"
            " blocks = %lu ; bfree = %lu",
            mp->mnt_point,
            sbuf.f_bsize, sbuf.f_frsize,
            sbuf.f_blocks, sbuf.f_bfree);

    rc = lsdir(mp->mnt_point);
    if (rc < 0)
    {
        LOG_ERR("FAIL: lsdir %s: %d", mp->mnt_point, rc);
        return rc;
    }

    return 0;
}

int storage_open_file(char *fname)
{
    if (filename[0] != 0)
    {
        LOG_ERR("file already open: %s", filename);
        return -1;
    }

    snprintf(filename, sizeof(filename), "%s/%s", mp->mnt_point, fname);

    samplecount = 0;
    fs_file_t_init(&file);
    int rc = fs_open(&file, filename, FS_O_CREATE | FS_O_RDWR);
    if (rc < 0)
    {
        LOG_ERR("FAIL: open %s: %d", filename, rc);
        return -1;
    }

    return 0;
}

int storage_close_file(char *fname)
{
    char *opened_file_basename = strrchr(filename, '/') + 1;
    if (!opened_file_basename)
    {
        LOG_ERR("no opened file!");
        return -1;
    }

    if (strcmp(fname, opened_file_basename) != 0)
    {
        LOG_ERR("%s is not the opened file, it's: %s", fname, opened_file_basename);
        return -1;
    }

    int ret = fs_close(&file);

    if (ret < 0)
    {
        LOG_ERR("FAIL: close %s: %d", filename, ret);
        return ret;
    }

    memset(filename, 0, MAX_PATH_LEN);
    return 0;
}

int storage_write_imu_sample(imu_sample_t imu_sample)
{
    int rc = fs_write(&file, &imu_sample, sizeof(imu_sample));
    if (rc < 0)
    {
        LOG_ERR("FAIL: write %s: %d", filename, rc);
        return -1;
    }

    // LOG_DBG("%s: wrote sample %d", filename, samplecount++);
    return 0;
}

static int storage_print(const struct shell *sh, size_t argc, char **argv)
{
    struct fs_file_t file;
    uint16_t sample_count = 0;
    char *fname = argv[1];

    fs_file_t_init(&file);
    int rc = fs_open(&file, fname, FS_O_RDWR);
    if (rc < 0)
    {
        LOG_ERR("FAIL: open %s: %d", fname, rc);
        return rc;
    }

    rc = fs_seek(&file, 0, FS_SEEK_SET);
    if (rc < 0)
    {
        LOG_ERR("FAIL: seek %s: %d", fname, rc);
        return rc;
    }

    for (;;)
    {
        imu_sample_t sample;

        rc = fs_read(&file, &sample, sizeof(sample));
        if (rc == 0)
        {
            LOG_INF("complete");
            break;
        }

        else if (rc < 0 || rc < sizeof(sample))
        {
            LOG_ERR("FAIL: read %s: [rd:%d]", fname, rc);
            break;
        }

        LOG_INF("%d:\t time=%llu, x=%0.3f, y=%0.3f, z=%0.3f", sample_count++, sample.timestamp, sample.ax, sample.ay, sample.az);
        k_sleep(K_MSEC(100)); // don't flood console and drop messages
    }

    rc = fs_close(&file);
    if (rc < 0)
    {
        LOG_ERR("FAIL: close %s: %d", fname, rc);
        return rc;
    }

    return 0;
}

static int erase_flash(unsigned int id)
{
    const struct flash_area *pfa;
    int rc;

    rc = flash_area_open(id, &pfa);
    if (rc < 0)
    {
        LOG_ERR("FAIL: unable to find flash area %u: %d",
                id, rc);
        return rc;
    }

    LOG_DBG("Area %u at 0x%x on %s for %u bytes",
            id, (unsigned int)pfa->fa_off, pfa->fa_dev->name,
            (unsigned int)pfa->fa_size);

    rc = flash_area_erase(pfa, 0, pfa->fa_size);
    LOG_ERR("Erasing flash area ... %d", rc);

    flash_area_close(pfa);
    return rc;
}

static int storage_erase(const struct shell *sh, size_t argc, char **argv)
{
    int rc = erase_flash((uintptr_t)mp->storage_dev);
    if (rc < 0)
    {
        LOG_ERR("failure: erase_flash rc=%d", rc);
        return rc;
    }

    return 0;
}

static int storage_ls(const struct shell *sh, size_t argc, char **argv)
{
    int rc = lsdir(mp->mnt_point);
    if (rc < 0)
    {
        LOG_ERR("FAIL: lsdir %s: %d", mp->mnt_point, rc);
        return rc;
    }

    return 0;
}

static int lsdir(const char *path)
{
    int res;
    struct fs_dir_t dirp;
    static struct fs_dirent entry;

    fs_dir_t_init(&dirp);

    /* Verify fs_opendir() */
    res = fs_opendir(&dirp, path);
    if (res)
    {
        LOG_ERR("Error opening dir %s [%d]", path, res);
        return res;
    }

    LOG_DBG("Listing dir %s ...", path);
    for (;;)
    {
        /* Verify fs_readdir() */
        res = fs_readdir(&dirp, &entry);

        /* entry.name[0] == 0 means end-of-dir */
        if (res || entry.name[0] == 0)
        {
            if (res < 0)
            {
                LOG_ERR("Error reading dir [%d]", res);
            }
            break;
        }

        if (entry.type == FS_DIR_ENTRY_DIR)
        {
            LOG_DBG("[DIR ] %s", entry.name);
        }
        else
        {
            LOG_DBG("[FILE] %s (size = %zu)",
                    entry.name, entry.size);
            filecount++;
        }
    }

    /* Verify fs_closedir() */
    fs_closedir(&dirp);

    return res;
}

SHELL_CMD_REGISTER(storage_ls, NULL, "list stored sample files", storage_ls);
SHELL_CMD_REGISTER(storage_print, NULL, "print stored samples for <filename> to console", storage_print);
SHELL_CMD_REGISTER(storage_erase, NULL, "erase stored samples", storage_erase);
