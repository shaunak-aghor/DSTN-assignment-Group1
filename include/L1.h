#ifndef L1_H
#define L1_H

#include <stdint.h>
#include "MemHier.h"

/*Line layout = 18 bits = valid 1 + tag 15 + LRU 2
 *The hierarchy tracks tags only, there is no data payload anywhere.*/

typedef struct {
    uint32_t  valid : 1;            /*1 bit*/
    uint32_t  tag : 15;              /*15 bits*/
    uint32_t  lru : 2;              /*2 bits -- 0 = most recently used*/
} L1Line;

typedef struct {
    L1Line ways[L1_WAYS];
} L1Set;

typedef struct {
    L1Set sets[L1_SETS];
    /*statistics, remove if not needed*/
    uint64_t read_hits;
    uint64_t read_misses;
    uint64_t write_hits;
    uint64_t write_misses;          /* no-write-allocate: no fill follows */
    uint64_t evictions;             /* clean victims sent to the buffer */
} L1Cache;

/*lifecycle*/
void l1_init(L1Cache *l1);
void l1_reset_stats(L1Cache *l1);

/*Returns the matching way index, or -1 on a miss. TODO*/
// .h
int  l1_probe(L1Cache *l1, uint32_t pa);

/*Does the LRU aging and incrementation, i.e. increment if counter < old_value. TODO*/
void l1_age(L1Cache *l1, uint32_t index, int way);

/*Returns whichever index either is invalid/empty or is LRU, always returns something. TODO*/
int  l1_select_victim(L1Cache *l1, uint32_t index);

/*Adds a block and marks it as MRU, does the LRU math, to be used after evictions.
 *Returns -1 on failure, 0 for a placement (the way was free) and 1 for a
 *replacement (the way still held a valid line, which is overwritten). DONE*/
int  l1_install(L1Cache *l1, uint32_t pa);

/*Applies a store on a write hit, donot bring miss to this. TODO*/
void l1_write_hit(L1Cache *l1, uint32_t pa, int way);

/*Full eviction function, has to handle EVERYTHING with the l1 side of evicts. DONE*/
int  l1_evict(L1Cache *l1, uint32_t index, int way, uint32_t *out_pa);

#endif