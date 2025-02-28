/*
 * This library implements a simple map of statically allocated k_work_item
 * structures.
 * Work items cannot by dynamically allocated, so this is used instead to allocate
 * and deallocate work items as needed.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "wi.h"
LOG_MODULE_REGISTER(wr, LOG_LEVEL_DBG);

#define MAX_WORK_ITEMS 128

static workref_t wrbuf[MAX_WORK_ITEMS];
static K_FIFO_DEFINE(wr_free_list);
static int  wrcount        = MAX_WORK_ITEMS;
static bool wr_initialized = false;

void wr_init(void)
{
    // mark all workrefs as free
    for (int i = 0; i < MAX_WORK_ITEMS; i++) {
        k_fifo_put(&wr_free_list, &wrbuf[i]);
    }
    wr_initialized = true;
}

// the following would all be better as static inline funcs in the header,
// except they need access to the freelist

workref_t *wr_get(void *ref, int line)
{
    if (!wr_initialized) {
        wr_init();
    }
    workref_t *wr = k_fifo_get(&wr_free_list, K_NO_WAIT);
    if (wr) {
        wr->reference = ref;
        wr->in_use    = line;
        wrcount--;
    } else {
        LOG_ERR("%s: No items on free list", __func__);
    }
    return wr;
}

void wr_put(workref_t *work)
{
    if (!wr_initialized) {
        wr_init();
    }
    work->in_use = 0;
    k_fifo_put(&wr_free_list, work);
    wrcount++;
}

void print_wr_stats()
{
    LOG_WRN("%d free workrefs", wrcount);
    for (int i = 0; i < MAX_WORK_ITEMS; i++) {
        if (wrbuf[i].in_use) {
            LOG_WRN("Index %d used for ref %p from line %d", i, wrbuf[i].reference, wrbuf[i].in_use);
        }
    }
}
