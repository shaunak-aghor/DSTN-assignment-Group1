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

/*Load path: probes L1, the write buffer and L2, promoting on an L2 hit.*/
CacheSearchResult cache_read(L1Cache *l1, L2Cache *l2, WriteBuffer *write_buffer,
				 uint32_t pa);

/*Store path. Returns where the block was found; the caller owns main memory.
 *CACHE_HIT_L1 or CACHE_HIT_WB means the store was queued in the write buffer,
 *anything else means the caller must mm_write() pa itself. If *drained_out comes back valid,
 *the buffer was full and the caller must send that entry to memory as well.*/
CacheSearchResult cache_write(L1Cache *l1, L2Cache *l2, WriteBuffer *write_buffer,
				 uint32_t pa, WBEntry *drained_out);

#endif
