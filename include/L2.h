#ifndef L2_H
#define L2_H

#include <stdint.h>
#include "MemHier.h"

/*
 * L2 cache: 32 KB, 16 B block, 8-way set associative.
 * Physically indexed, physically tagged.
 * Replacement: FIFO, one pointer PER SET (not per line).
 * Writes: write-through. Every write reaches MM at write time.
 * no dirty check, no stall, no write-back.
 * Exclusive with L1: a block promoted to L1 is invalidated here.
 * Line layout -- 142 bits:
 *   valid 1 + tag 13 + data 128
 * Set metadata -- 3 bits:
 *   fifo_ptr 3
 */

typedef struct {
    uint16_t valid : 1;            /*1 bit*/
    uint16_t tag : 13;              /*13 bits */
    uint8_t  data[BLOCK_SIZE];      /*128 bits */
} L2Line;

typedef struct {
    L2Line  ways[L2_WAYS];
    uint8_t fifo_ptr;               /* 3 bits -- next victim way, 0..7 */
} L2Set;

typedef struct {
    L2Set sets[L2_SETS];

    /* statistics */
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;             /*victims discarded, never written back*/
    uint64_t promotions;            /*blocks moved to L1 and invalidated here*/
    uint64_t passthrough_writes;    /*stores that found no resident line*/
    uint64_t updated_writes;        /*stores that updated a resident line*/
} L2Cache;

/*lifecycle TODO*/
void l2_init(L2Cache *l2);
void l2_reset_stats(L2Cache *l2);

/*Returns the matching way index, or -1 on a miss. TODO*/
int  l2_probe(L2Cache *l2, uint32_t pa);

/*Copies the block out and marks it invalid, returns 0 if address not present. TODO*/
int  l2_promote(L2Cache *l2, uint32_t pa, uint8_t *out_block);

/*Invalidates in case a promote isnt needed. TODO*/
void l2_invalidate(L2Cache *l2, uint32_t pa);

/*Handles allocation of way if invalid or FIFO, donot touch for stores. TODO*/
void l2_allocate(L2Cache *l2, uint32_t pa, const uint8_t *block);

/*Returns 1 if a line was updated, 0 otherwise. Call only on write misses from L1. TODO*/
int  l2_write_through(L2Cache *l2, uint32_t pa, const uint8_t *bytes, uint32_t len);

/*Returns an invalid way if the set has one, else the way the curr FIFO pointer index. TODO*/
int  l2_select_victim(L2Cache *l2, uint32_t index);

#endif