/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/fs/fs.h>
#include <zephyr/shell/shell.h>

#include <limits.h>
#include <stdio.h>

#include "fqueue.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(fqueue, LOG_LEVEL_DBG);

#ifndef FILENAME_MAX
#define FILENAME_MAX 512
#endif

#define PARTITION_NODE DT_NODELABEL(lfs1)
FS_FSTAB_DECLARE_ENTRY(PARTITION_NODE);

static struct fs_mount_t *mp = &FS_FSTAB_ENTRY(PARTITION_NODE);

typedef void (*qcb_t)(fqueue_t *fq, struct fs_dirent *);

/*
 * there is a global file queue lock. Any time anything is written to any queue
 * any thread waiting on reading something from any queue will be awoken. That thread
 * must recheck the condition and re-sleep if necesary
 */
K_SEM_DEFINE(fqueue_lock, 0, 1);
/*
 * we poll the semaphore and also allow for signals to show that we need to rescan the
 * queue (purge or item removed)
 */
static struct k_poll_signal fq_rescan;

static struct k_poll_event fq_events[] = {
    K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_SIGNAL, K_POLL_MODE_NOTIFY_ONLY, &fq_rescan, 0),
    K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_SEM_AVAILABLE, K_POLL_MODE_NOTIFY_ONLY, &fqueue_lock, 0),
};

static int fq_walk(fqueue_t *fq, qcb_t cb)
{
    int                     res = 0;
    int                     ret = 0;
    struct fs_dir_t         dirp;
    static struct fs_dirent entry;

    fs_dir_t_init(&dirp);

    /* Verify fs_opendir() */
    res = fs_opendir(&dirp, fq->dirname);
    if (res) {
        if (fq->mode == FQ_WRITE) {
            res = fs_mkdir(fq->dirname);
        }
        if (res) {
            LOG_ERR("Unable to initialize in %s: %d", fq->dirname, res);
            return res;
        }
    }

    LOG_DBG("Walking queue %s ...", fq->dirname);
    while (1) {
        /* Verify fs_readdir() */
        ret = fs_readdir(&dirp, &entry);

        /* entry.name[0] == 0 means end-of-dir */
        if (ret || entry.name[0] == 0) {
            if (ret < 0) {
                LOG_ERR("Error reading dir [%d]", ret);
            }
            break;
        }
        if (cb) {
            cb(fq, &entry);
        }
    }

    /* Verify fs_closedir() */
    fs_closedir(&dirp);
    if (res == 0) {
        res = ret;
    }
    return res;
}

static int fq_find_first_and_last(fqueue_t *fq, int *first, int *last)
{
    int                     res = 0;
    int                     ret = 0;
    struct fs_dir_t         dirp;
    static struct fs_dirent entry;
    int                     current;

    fs_dir_t_init(&dirp);

    *first = INT_MAX;
    *last  = -1;

    /* Verify fs_opendir() */
    res = fs_opendir(&dirp, fq->dirname);
    if (res) {
        if (fq->mode == FQ_WRITE) {
            res = fs_mkdir(fq->dirname);
        }
        if (res) {
            LOG_ERR("Unable to initialize in %s: %d", fq->dirname, res);
            return res;
        }
    }

    while (1) {
        /* Verify fs_readdir() */
        ret = fs_readdir(&dirp, &entry);

        /* entry.name[0] == 0 means end-of-dir */
        if (ret || entry.name[0] == 0) {
            if (ret < 0) {
                LOG_ERR("Error reading dir [%d]", ret);
            }
            break;
        }
        // file names are just numbers (base 10)
        if (entry.type != FS_DIR_ENTRY_DIR) {
            if (entry.size > 0) {
                current = strtol(entry.name, NULL, 10);
                if (current < *first) {
                    *first = current;
                }
                if (current > *last) {
                    *last = current;
                }
            }
        }
    }

    /* Verify fs_closedir() */
    fs_closedir(&dirp);
    if (*first == INT_MAX) {
        *first = 0;
    }
    if (*last < 0) {
        *last = 0;
    } else {
        *last += 1;    // if we have existing files, start after those
    }

    if (res == 0) {
        res = ret;
    }
    return res;
}

static void fq_del(fqueue_t *fq, struct fs_dirent *entry)
{
    //char fname[LFS_NAME_MAX + 5];
    char fname[strlen(fq->dirname) + strlen(entry->name) + 5];
    snprintf(fname, sizeof(fname) - 1, "%s/%s", fq->dirname, entry->name);
    LOG_INF("Delete %s", fname);
    fs_unlink(fname);
    k_poll_signal_raise(&fq_rescan, 1);
}

int fqueue_purge(fqueue_t *fq)
{
    if (fq->mode != FQ_WRITE) {
        return -EINVAL;
    }
    fq_walk(fq, fq_del);
    fq->cur_idx = 0;
    return 0;
}

int fqueue_init(fqueue_t *restrict fq, const char *restrict qname, enum fqmode mode, bool do_reinitialize)
{
    int ret = 0;
    int first;
    int last;

    memset(fq, 0, sizeof(fqueue_t));
    /* a file queue is simply a directory with one file per queued item */
    snprintf(fq->dirname, sizeof(fq->dirname) - 1, "%s/%s", mp->mnt_point, qname);
    fq->mode = mode;

    ret = fq_find_first_and_last(fq, &first, &last);

    switch (mode) {
    case FQ_WRITE:
        if (ret == -EEXIST && do_reinitialize) {
            fqueue_purge(fq);
        }
        LOG_DBG("Set current file to last entry (%d)", last);
        fq->cur_idx = last;
        // initialize the poll signal for rescan
        k_poll_signal_init(&fq_rescan);
        break;
    case FQ_READ:
        fq->cur_idx = first;
        break;

    default:
        return -EINVAL;
    }
    return ret;
}

int fqueue_put(fqueue_t *restrict fq, const void *restrict data, size_t size)
{
    char              fname[FILENAME_MAX];
    struct fs_file_t  entry;
    struct fs_statvfs fs_stats;
    int               ret;

    if (fq->mode != FQ_WRITE) {
        return -EINVAL;
    }
    fs_statvfs(fq->dirname, &fs_stats);
    if (fs_stats.f_bfree < CONFIG_MIN_BFREE) {
        LOG_ERR("Inufficient space to store data");
        return -ENOMEM;
    }
    snprintf(fname, sizeof(fname) - 1, "%s/%03d", fq->dirname, fq->cur_idx++);
    fs_file_t_init(&entry);
    if ((ret = fs_open(&entry, fname, FS_O_CREATE | FS_O_WRITE))) {
        LOG_ERR("Unable to create fq entry %s [%d]", fname, ret);
        return ret;
    }
    LOG_DBG("Append %d bytes to queue", size);
    ret = fs_write(&entry, data, size);
    if (ret <= 0) {
        LOG_ERR("Failed to write entry to file queue: %s [%d]", fname, ret);
    } else {
        ret = 0;
    }
    fs_close(&entry);
    k_sem_give(&fqueue_lock);

    return ret;
}

static int
fqueue_read(fqueue_t *restrict fq, void *restrict buf, size_t *restrict size, k_timeout_t timeout, bool do_remove)
{
    char             fname[FILENAME_MAX];
    struct fs_file_t entry;
    int              ret;

    if (fq->mode != FQ_READ) {
        return -EINVAL;
    }
restart:
    snprintf(fname, sizeof(fname) - 1, "%s/%03d", fq->dirname, fq->cur_idx);
    fs_file_t_init(&entry);
    while ((ret = fs_open(&entry, fname, FS_O_READ))) {
        // the next entry is not yet there
        if (k_poll(fq_events, ARRAY_SIZE(fq_events), timeout)) {
            return -EAGAIN;
        }
        // woken for either data ready or rescan needed
        if (fq_events[0].state == K_POLL_STATE_SIGNALED) {
            int first, last;

            LOG_WRN("Rescan triggered!");
            ret         = fq_find_first_and_last(fq, &first, &last);
            fq->cur_idx = first;
            goto restart;
        }
        if (fq_events[1].state == K_POLL_STATE_SEM_AVAILABLE) {
            if (k_sem_take(&fqueue_lock, K_NO_WAIT)) {
                return -EAGAIN;
            }
            break;
        }
    }
    struct fs_dirent stat;
    do {
        ret = fs_stat(fname, &stat);
        if (stat.size == 0) {
            // file exists, but has not yet been written to
            ret = k_sem_take(&fqueue_lock, timeout);
        }
    } while (ret == 0 && stat.size == 0);

    int bytes_to_read = *size;
    if (stat.size > *size) {
        bytes_to_read = stat.size;
    }
    ret = fs_read(&entry, buf, bytes_to_read);
    fs_close(&entry);
    if (ret > 0) {
        if (do_remove) {
            fs_unlink(fname);
            fq->cur_idx++;
        }
        *size = ret;
        ret   = 0;
    }
    return ret;
}

int fqueue_get(fqueue_t *restrict fq, void *restrict buf, size_t *restrict size, k_timeout_t timeout)
{
    return fqueue_read(fq, buf, size, timeout, true);
}

int fqueue_peek(fqueue_t *restrict fq, void *restrict buf, size_t *restrict size, k_timeout_t timeout)
{
    return fqueue_read(fq, buf, size, timeout, false);
}

//////////////////////////////////////////
// test functions
//////////////////////////////////////////

static fqueue_t test_queue;

void fq_print(fqueue_t *fq, struct fs_dirent *entry)
{
    printk("%s: %d\n", entry->name, entry->size);
}

int fq_list(fqueue_t *fq)
{
    fq_walk(fq, fq_print);
    return 0;
}

static int fq_create_shell(const struct shell *sh, int argc, char **argv)
{
    if (argc != 3) {
        shell_error(sh, "Usage: fq create <r|w> qname");
        return 1;
    }
    enum fqmode mode;
    if (*argv[1] == 'w') {
        mode = FQ_WRITE;
    } else if (*argv[1] == 'r') {
        mode = FQ_READ;
    } else {
        shell_error(sh, "Unknown mode %s", argv[1]);
        return 1;
    }
    int ret = fqueue_init(&test_queue, argv[2], mode, false);
    if (ret) {
        shell_error(sh, "Failed to init queue %s for %s [%d]", argv[2], mode == FQ_READ ? "Reading" : "Writing", ret);
    }
    return ret;
}

static int fq_print_shell(const struct shell *sh, int argc, char **argv)
{
    if (argc > 1) {
        int ret = fqueue_init(&test_queue, argv[1], FQ_READ, false);
        if (ret) {
            shell_error(sh, "Could not init queue %s", argv[1]);
            return 1;
        }
    }
    shell_print(sh, "dirname: %s", test_queue.dirname);
    shell_print(sh, "mode = %d (%s)", test_queue.mode, test_queue.mode == FQ_WRITE ? "write" : "read");
    shell_print(sh, "next file = %03d", test_queue.cur_idx);
    fq_list(&test_queue);
    return 0;
}

static int fq_purge_shell(const struct shell *sh, int argc, char **argv)
{
    if (fqueue_purge(&test_queue)) {
        shell_error(sh, "Failed to purge test queue");
    }
    return 0;
}

static int fq_put_shell(const struct shell *sh, int argc, char **argv)
{
    if (test_queue.dirname[0] == 0) {
        shell_error(sh, "Queue not initialized");
        return 1;
    }
    if (argc < 2) {
        shell_error(sh, "Usage: fq put <data>");
        return 1;
    }
    int ret = fqueue_put(&test_queue, argv[1], strlen(argv[1]) + 1);    // write the trailing nul byte too
    if (ret) {
        shell_error(sh, "Failed to write to queue: %d", ret);
    } else {
        shell_print(sh, "Wrote \"%s\" to queue", argv[1]);
    }
    return ret;
}

// note this function is used for both "get" and "peek" functions, based on argv[0]
static int fq_get_shell(const struct shell *sh, int argc, char **argv)
{
    if (test_queue.dirname[0] == 0) {
        shell_error(sh, "Queue not initialized");
        return 1;
    }
    char   buffer[128];
    size_t sz      = sizeof(buffer);
    bool   do_peek = strcmp(argv[0], "peek") == 0;
    int    ret;
    if (do_peek) {
        ret = fqueue_peek(&test_queue, buffer, &sz, K_MSEC(2000));
    } else {
        ret = fqueue_get(&test_queue, buffer, &sz, K_MSEC(2000));
    }
    if (ret) {
        shell_error(sh, "fqueue_get failed %d", ret);
    } else {
        shell_print(sh, "Got %d bytes: \"%s\"", sz, buffer);
    }
    return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_fq,
    SHELL_CMD(create, NULL, "Create a file queue", fq_create_shell),
    SHELL_CMD(peek, NULL, "Look at first item in the queue", fq_get_shell),
    SHELL_CMD(print, NULL, "Print the current file queue structure", fq_print_shell),
    SHELL_CMD(purge, NULL, "Purge queue", fq_purge_shell),
    SHELL_CMD(put, NULL, "Add data to queue", fq_put_shell),
    SHELL_CMD(get, NULL, "Read data from queue", fq_get_shell),
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(fq, &sub_fq, "Test file queue", NULL);
