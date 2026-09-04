#ifndef CACHE_H
#define CACHE_H

#include <stdint.h>
#include "L1.h"
#include "L2.h"
#include "WB.h"

int cache_search(L1Cache *l1, L2Cache *l2, WriteBuffer *write_buffer,
				 uint32_t pa);

#endif
