#ifndef L1_H
#define L1_H

#include <stdint.h>
#include "MemHier.h"

/*
 * L1 cache: 4 KB, 16 B blocks, 4-way set associative, 64 sets.
 * Write-through + no-write-allocate (so no dirty bit), LRU replacement.
 * We only track tags, there is no data stored anywhere.
 *
 * Line = 18 bits: valid 1 + tag 15 + lru 2
 */

typedef struct {
    uint32_t valid : 1;
    uint32_t tag   : L1_TAG_BITS;
    uint32_t lru   : 2;                 /* 0 = least recently used, 3 = most recently used */
} L1Line;

typedef struct {
    L1Line ways[L1_WAYS];
} L1Set;

typedef struct {
    L1Set sets[L1_SETS];
} L1Cache;

/* Clears every line. */
void l1_init(L1Cache *l1);

/* Returns the way holding pa's block, or -1. */
int  l1_probe(L1Cache *l1, uint32_t pa);

/* Marks `way` as the most recently used line in its set. */
void l1_age(L1Cache *l1, uint32_t index, int way);

/* Returns a free way in the set if there is one, else the LRU way. */
int  l1_select_victim(L1Cache *l1, uint32_t index);

/* Puts pa's block into its set as the most recently used line. */
void l1_install(L1Cache *l1, uint32_t pa);

/* Empties `way`. If it held a block, writes its address to *out_pa and returns 1. */
int  l1_evict(L1Cache *l1, uint32_t index, int way, uint32_t *out_pa);

/* Drops every line belonging to physical frame `frame`. */
void l1_invalidate_frame(L1Cache *l1, uint32_t frame);

#endif
