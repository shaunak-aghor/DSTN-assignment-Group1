#ifndef CACHE_H
#define CACHE_H

#include <stdint.h>
#include "L1.h"
#include "L2.h"
#include "WB.h"

/*
 * The cache side of one access. L1 and L2 are searched at the same time
 * on two threads (look-aside), then the result is applied here.
 * The write buffer sits in line with L1: every store goes through it,
 * and the CPU never reads from it.
 */

typedef enum {
    CACHE_MISS = 0,
    CACHE_HIT_L1,
    CACHE_HIT_L2
} CacheSearchResult;

/* Reads the block at physical address pa, moving it into L1 if it is not there. */
void cache_read(L1Cache *l1, L2Cache *l2, WriteBuffer *wb, uint32_t pa);

/* Writes to physical address pa (write-through, no-write-allocate). */
void cache_write(L1Cache *l1, L2Cache *l2, WriteBuffer *wb, uint32_t pa);

#endif
