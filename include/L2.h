#ifndef L2_CACHE_H
#define L2_CACHE_H

#include <stdint.h>

#define L2_SETS 256
#define L2_WAYS 8

typedef struct l2_line {
    uint32_t valid : 1;
    uint32_t tag   : 13;
} L2_Line;

typedef struct l2_set{
    L2_Line lines[L2_WAYS];
    uint32_t fifo_pointer : 3; 
} L2_Set;


typedef struct l2{
    L2_Set sets[L2_SETS];
} L2;

void init_L2_cache(L2* cache);
int L2_search(L2* cache, uint32_t physical_address);
void L2_invalidate_block(L2* cache, uint32_t physical_address);
uint32_t L2_allocate_block(L2* cache, uint32_t physical_address);


#endif