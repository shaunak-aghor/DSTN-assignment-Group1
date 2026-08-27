#ifndef L2_CACHE_H
#define L2_CACHE_H

#include <stdint.h>

#define L2_SETS 256
#define L2_WAYS 8


typedef struct {
    uint32_t valid : 1; 
    uint32_t sequence : 3;
    uint32_t tag : 20;
    uint8_t data[16];
} L2_Line;

typedef struct {
    L2_Line lines[L2_WAYS];
} L2_Set;

typedef struct {
    L2_Set sets[L2_SETS];
} L2_Cache;

#endif