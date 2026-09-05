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

int cache_search(L1Cache *l1, L2Cache *l2, WriteBuffer *write_buffer,
				 uint32_t pa);

#endif
