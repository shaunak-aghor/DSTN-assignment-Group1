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

/* Reads pa.  Returns where it was found, promoting from L2 into L1 on an L2
 * hit.  On CACHE_MISS the caller must fetch the block and install it. */
CacheSearchResult cache_read(L1Cache *l1, L2Cache *l2, WriteBuffer *write_buffer,
                             uint32_t pa);

/* Writes pa.  Returns where it was found.  CACHE_HIT_L1 and CACHE_HIT_WB mean
 * the store was buffered; anything else means the caller must write it to
 * memory.  A displaced buffer entry is returned through *drained_out. */
CacheSearchResult cache_write(L1Cache *l1, L2Cache *l2, WriteBuffer *write_buffer,
                              uint32_t pa, WBEntry *drained_out);

#endif
