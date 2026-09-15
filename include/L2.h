#ifndef L2_H
#define L2_H

#include <stdint.h>
#include "MemHier.h"
#include "L1.h"

/*
 * L2 cache: 32 KB, 16 B block, 8-way set associative.
 * Physically indexed, physically tagged.
 * Replacement: FIFO, 8x8 bit matrix per set (triangular state matrix).
 * Writes: write-through. Every write reaches MM at write time.
 * no dirty check, no stall, no write-back.
 * Exclusive with L1: a block promoted to L1 is invalidated here.
 * Line layout -- 14 bits:
 *   valid 1 + tag 13
 * Set metadata:
 *   fifo_matrix 8 bytes (row i, bit j = 1 if way i younger than way j)
 */

typedef struct {
    uint16_t valid : 1;            /* 1 bit */
    uint16_t tag   : 13;           /* 13 bits */
} L2Line;

typedef struct {
    L2Line  ways[L2_WAYS];
    uint8_t fifo_matrix[L2_WAYS];   /* 8x8 bit matrix */
} L2Set;

typedef struct {
    L2Set sets[L2_SETS];

} L2Cache;

/* Invalidates every line. */
void l2_init(L2Cache *l2);

/* Returns the way holding pa, or WAY_NONE. */
int  l2_probe(L2Cache *l2, uint32_t pa);

/* Moves pa's block from L2 into L1, demoting L1's victim back into L2. */
void l2_promote(L2Cache *l2, L1Cache *l1, uint32_t pa);

/* Invalidates pa's line; returns pa if it was present, 0 otherwise. */
uint32_t l2_invalidate(L2Cache *l2, uint32_t pa);

/* Records `way` as the newest in its set's FIFO order. */
void l2_age(L2Cache *l2, uint32_t index, int way);

/* Installs pa; returns 1 if it displaced a valid line. */
int  l2_allocate(L2Cache *l2, uint32_t pa);

/* Applies a store to a resident line; returns 1 if one was updated. */
int  l2_write_through(L2Cache *l2, uint32_t pa, uint32_t len);

/* Returns an invalid way if the set has one, else the oldest in FIFO order. */
int  l2_select_victim(L2Cache *l2, uint32_t index);

/* Drops every line in physical frame `frame`; returns how many. */
int  l2_invalidate_frame(L2Cache *l2, uint32_t frame);

#endif