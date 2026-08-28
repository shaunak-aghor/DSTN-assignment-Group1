#ifndef L1_CACHE_H
#define L1_CACHE_H
#include "common.h"

#define L1_SETS 64
#define L1_WAYS 4
#define WRITE_BUFFER_SIZE 4

/*
    Some calculations:
        each line entry in the tag directory of L1 cache requires : 1 + 2 + 15 = 18 bits
        each set contains 4 entries : 18 * 4 = 72bits
        L1 cache has 64 sets : 72 * 64 = 4608 bits

*/


//one valid bit + 2 lru counter bits + 15 tag bits
typedef struct l1_line
{
    uint32_t valid : 1;
    uint32_t lru_counter : 2;
    uint32_t tag : 15;
} L1_line;


typedef struct l1_set
{
    L1_line L1_lines[L1_WAYS];
} L1_set;

typedef struct write_buffer
{
    uint32_t valid : 1;
    uint32_t block_addr : 21; //everything except the offset of the block
} Write_Buffer_Entry;



typedef struct l1
{
    L1_set L1_sets[L1_SETS];
    Write_Buffer_Entry Write_Buffer_Entries[WRITE_BUFFER_SIZE];
    uint8_t head : 2;
    uint8_t tail : 2;
} L1;


void init_L1_cache(L1* cache);

int L1_search(L1* cache, uint32_t physical_addr);
int write_buffer_search(L1* cache, uint32_t physical_addr);
void L1_update_lru(L1* cache, uint32_t index, uint8_t accessed_way);
void L1_invalidate_block(L1* cache, uint32_t physical_addr);
void L1_allocate_block(L1* cache, uint32_t physical_address);



#endif