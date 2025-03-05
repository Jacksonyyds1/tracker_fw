/*
 * This library implements a simple map of statically allocated k_work_item
 * structures.
 * Work items cannot by dynamically allocated, so this is used instead to allocate
 * and deallocate work items as needed.
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/logging/log.h>

#include "wi.h"
LOG_MODULE_REGISTER(wr, LOG_LEVEL_DBG);

#define MAX_WORK_ITEMS 128

static workref_t wrbuf[MAX_WORK_ITEMS];
static K_FIFO_DEFINE(wr_free_list);
static atomic_t wrcount = ATOMIC_INIT(MAX_WORK_ITEMS);

void wr_init(void)
{
    // mark all workrefs as free
    for (int i = 0; i < MAX_WORK_ITEMS; i++) {
        k_fifo_put(&wr_free_list, &wrbuf[i]);
    }
}

// the following would all be better as static inline funcs in the header,
// except they need access to the freelist

workref_t *wr_get(void *ref, int line)
{
    workref_t *wr = k_fifo_get(&wr_free_list, K_NO_WAIT);
    if (wr) {
        __ASSERT(wr->in_use == 0, "in use workref from line %d on free queue!", wr->in_use);
        wr->reference = ref;
        wr->in_use    = line;
        atomic_dec(&wrcount);
    } else {
        LOG_ERR("%s: No items on free list", __func__);
    }
    return wr;
}

void wr_put(workref_t *work)
{
    __ASSERT(work->in_use, "Double free of workref allocated at line %d!", work->in_use);
    work->in_use = 0;
    k_fifo_put(&wr_free_list, work);
    atomic_inc(&wrcount);
}

static int do_wr_cmd(const struct shell *sh, int argc, char **argv)
{
    shell_print(sh, "%u free workrefs", (uint32_t)atomic_get(&wrcount));
    for (int i = 0; i < MAX_WORK_ITEMS; i++) {
        if (wrbuf[i].in_use) {
            shell_print(sh, "Index %d used for ref %p from line %d", i, wrbuf[i].reference, wrbuf[i].in_use);
        }
    }
    return 0;
}

SHELL_CMD_REGISTER(wr, NULL, "Print workref info", do_wr_cmd);