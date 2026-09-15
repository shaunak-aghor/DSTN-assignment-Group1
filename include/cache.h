#ifndef CACHE_H
#define CACHE_H

#include <stdint.h>
#include "L1.h"
#include "L2.h"
#include "WB.h"

typedef enum {
    CACHE_MISS = 0,
    CACHE_HIT_L1,
    CACHE_HIT_WB,
    CACHE_HIT_L2
} CacheSearchResult;

/* Returns where PA was found, promoting from L2 into L1 on an L2 hit.
 * On CACHE_MISS, fetch the block and install it.
 * An L2 hit promotes, which displaces an L1 line and demotes it into L2.
 * *l1_evicted and *l2_evicted report those two events so the caller can count
 * them; both are cleared on every other outcome.  Either may be NULL. */
CacheSearchResult cache_read(L1Cache *l1, L2Cache *l2, WriteBuffer *write_buffer,
                             uint32_t pa, int *l1_evicted, int *l2_evicted);

/* Writes pa and returns where it was found. CACHE_HIT_L1 and CACHE_HIT_WB mean
 * the store was buffered, anything else demands a write to memory.
 * A displaced buffer entry is returned through *drained_out. */
CacheSearchResult cache_write(L1Cache *l1, L2Cache *l2, WriteBuffer *write_buffer,
                              uint32_t pa, WBEntry *drained_out);

#endif
