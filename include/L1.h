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
} L1Cache;

/* Invalidates every line. */
void l1_init(L1Cache *l1);

/* Returns the way holding pa, or WAY_NONE. */
int  l1_probe(L1Cache *l1, uint32_t pa);

/* Makes `way` the most recently used line of its set. */
void l1_age(L1Cache *l1, uint32_t index, int way);

/* Returns an invalid way if the set has one, else the least recently used. */
int  l1_select_victim(L1Cache *l1, uint32_t index);

/* Installs pa as most recently used; returns 1 if it overwrote a valid line. */
int  l1_install(L1Cache *l1, uint32_t pa);

/* Marks a store that hit; promotes the line to most recently used. */
void l1_write_hit(L1Cache *l1, uint32_t pa, int way);

/* Frees `way` into *out_pa; returns 1 if a line was there, 0 if already free. */
int  l1_evict(L1Cache *l1, uint32_t index, int way, uint32_t *out_pa);

/* Drops every line in physical frame `frame`; returns how many. */
int  l1_invalidate_frame(L1Cache *l1, uint32_t frame);

#endif