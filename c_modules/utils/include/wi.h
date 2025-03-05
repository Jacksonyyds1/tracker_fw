#ifndef __WI_H___
#define __WI_H__

#include <zephyr/kernel.h>

typedef struct
{
    uint32_t      fifo_reserved;
    struct k_work work;
    void         *reference;
    uint32_t      in_use;    // fopr debug only
} workref_t;

void       wr_init(void);
workref_t *wr_get(void *ref, int line);
void       wr_put(workref_t *work);

#endif