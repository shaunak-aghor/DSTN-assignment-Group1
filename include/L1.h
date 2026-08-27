#ifndef L1_CACHE_H
#define L1_CACHE_H

#include <stdint.h>

#define L1_SETS 64
#define L1_WAYS 4
#define WRITE_BUFFER_SIZE 4


typedef struct {
    uint32_t valid : 1;  
    uint32_t dirty : 1;  
    uint32_t lru_counter : 2;  
    uint32_t tag : 22;
    uint8_t data[16];
} L1_Line;

typedef struct {
    L1_Line lines[L1_WAYS];
} L1_Set;


typedef struct {
    uint32_t valid : 1;
    uint32_t dirty : 1;
    uint32_t block_number : 28;
    uint8_t  data[16];
} Write_Buffer_Entry;

typedef struct {
    L1_Set sets[L1_SETS];
    Write_Buffer_Entry write_buffer[WRITE_BUFFER_SIZE];
    uint8_t write_buffer_count; 
    uint8_t write_buffer_head;
    uint8_t write_buffer_tail;
} L1_Cache;

#endif