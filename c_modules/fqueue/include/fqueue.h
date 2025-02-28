/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */

#ifndef _FQUEUE_H_
#define _FQUEUE_H_

#include <stdint.h>
#include <stdbool.h>

#include <zephyr/fs/littlefs.h>

#ifdef __cplusplus
extern "C" {
#define restrict
#endif

typedef struct fqueue fqueue_t;

enum fqmode
{
    FQ_READ,
    FQ_WRITE,
};

struct fqueue
{
    char        dirname[LFS_NAME_MAX];
    enum fqmode mode;
    uint32_t    cur_idx;    // index of last file read or written
};

/**
 * Initialize a file queue with a given name
 * @param fq        the queue structure to initialize
 * @param qname     the name for the queue.
 * @param mode      If the first character of mode is 'r', the queue is opened for reading (and should already exist)
 *                  If the first character of mode is 'w', the queue is created for writing (see do_reinitialize)
 * @param do_reinitialize
 *                  If true and the queue is being opened for writing and it already exists, then remove all the content.
 *                  If false and the queue is being opened for writing and it already exists, the queue is left unchanged and an error is returned.
 * @returns         0 on success. -EBUSY if queue is being opened for writing and already exists
 */
int fqueue_init(fqueue_t *restrict fq, const char *restrict qname, enum fqmode mode, bool do_reinitialize);

/**
 * put a new entry into the queue
 * @param fq        pointer to the queue to append data to
 * @param data      pointer to the data to append
 * @param size      number of bytes of data to add
 * @returns         0 on success, negative errno on failure
 */
int fqueue_put(fqueue_t *restrict fq, const void *restrict data, size_t size);

/**
 * get the next entry from the queue
 * @param fq        pointer to the queue to read data to
 * @param data      pointer to the buffer where the entry will be copied
 * @param size      on entry, a pointer to a value with the maximum number of bytes that can be
 * read. On return, this value will be updatted to the actual number read.
 * @param timeout   how long to wait for the next message
 * @returns         0 on success, negative errno on failure
 *                  -ENOMESG     Returned without waiting or queue purged.
 *                  -EAGAIN      Waiting period timed out.
 */
int fqueue_get(fqueue_t *restrict fq, void *restrict buf, size_t *restrict size, k_timeout_t timeout);

/**
 * Look at the first entry in the queue without removing it
 * @param fq        pointer to the queue to read data to
 * @param data      pointer to the buffer where the entry will be copied
 * @param size      on entry, a pointer to a value with the maximum number of bytes that can be
 * read. On return, this value will be updatted to the actual number read.
 * @param timeout   how long to wait for the next message
 * @returns         0 on success, negative errno on failure
 *                  -ENOMESG     Returned without waiting or queue purged.
 *                  -EAGAIN      Waiting period timed out.
 */
int fqueue_peek(fqueue_t *restrict fq, void *restrict buf, size_t *restrict size, k_timeout_t timeout);

/**
 * Remove all the message from an existing queue.
 * This routine discards all unreceived messages in a file queue. Any threads that
 * are blocked waiting to read a message from the queue are unblocked and see an -ENOMSG error
 * code.
 */
int fqueue_purge(fqueue_t *msgq);


/**
 * Delete a queue and all its content
 * Since you cannot create a new queue for writing with `fqueue_init()` if the queue already
 * exists, it must be possible to handle the case where the system has crashed or been
 * restarted before a queue was drained. Therefore typical usge is to call `fqueue_delete()`
 * if `fqueue_init()` return `EEXIST`
 * @param name      The name for the queue
 * @returns         0 on success, -ENOENT if there was no queue with that name
 */
int fqueue_delete(const char *name);

#ifdef __cplusplus
}
#endif

#endif