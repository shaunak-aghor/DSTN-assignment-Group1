#ifndef TLB_H
#define TLB_H

#include <stdint.h>

#define TLB_ENTRIES 32

//28 bit struct entry 
typedef struct {
    uint32_t valid : 1; 
    uint32_t vpn : 22;
    uint32_t pfn : 15;
    uint16_t pid;       
} TLB_Entry;

typedef struct {
    TLB_Entry entries[TLB_ENTRIES];
} TLB;

#endif