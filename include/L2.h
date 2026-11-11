#ifndef L2_H
#define L2_H

#include <stdint.h>
#include "MemHier.h"

/*
 * L2 cache: 32 KB, 16 B blocks, 8-way set associative, 256 sets.
 * Write-through (every write also goes to memory), FIFO replacement.
 * Exclusive with L1: a block lives in L1 or L2, never both.
 *
 * Line = 14 bits: valid 1 + tag 13
 * Per set: 8x8 FIFO bit matrix (row i, bit j = 1 if way i is younger than way j)
 */

typedef struct {
    uint16_t valid : 1;
    uint16_t tag   : L2_TAG_BITS;
} L2Line;

typedef struct {
    L2Line  ways[L2_WAYS];
    uint8_t fifo_matrix[L2_WAYS];
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
