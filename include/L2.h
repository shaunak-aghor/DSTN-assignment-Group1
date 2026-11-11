#ifndef L2_H
#define L2_H

#include <stdint.h>
#include "MemHier.h"

/*
 * L2 cache: 32 KB, 16 B blocks, 8-way set associative, 256 sets.
 * Write-through (every write also goes to memory), FIFO replacement.
 * Exclusive with L1: a block lives in L1 or L2, never both.
 *
 * Line = 16 bits: valid 1 + tag 15
 * Per set: a 3-bit FIFO pointer to the next way to replace. Lines are filled
 * in round-robin order, so the way it points at is the oldest one.
 * (An 8x8 FIFO/LRU matrix would cost 56 bits per set; matrices only pay
 * off for very small caches.)
 */

typedef struct {
    uint16_t valid : 1;
    uint16_t tag   : L2_TAG_BITS;
} L2Line;

typedef struct {
    L2Line  ways[L2_WAYS];
    uint8_t fifo_next;                  /* 3 bits used: 0..7, the oldest way */
} L2Set;

typedef struct {
    L2Set sets[L2_SETS];
} L2Cache;

/* Clears every line. */
void l2_init(L2Cache *l2);

/* Returns the way holding pa's block, or -1. */
int  l2_probe(L2Cache *l2, uint32_t pa);

/* Removes pa's block from L2 (used when it moves up to L1). */
void l2_invalidate(L2Cache *l2, uint32_t pa);

/* Puts pa's block into L2, replacing the oldest line if the set is full. */
void l2_allocate(L2Cache *l2, uint32_t pa);

/* Drops every line belonging to physical frame `frame`. */
void l2_invalidate_frame(L2Cache *l2, uint32_t frame);

#endif
