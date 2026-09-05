#include "L2.h"
#include <stddef.h>
#include "L1.h"

int l2_probe(L2Cache *l2, uint32_t pa)
{
    uint32_t index;
    uint32_t tag;
    int way;

    if (l2 == NULL)
        return -1;

    
    index = L2_INDEX(pa);
    tag = L2_TAG(pa);

    
    for (way = 0; way < L2_WAYS; way++) {
        const L2Line *line = &l2->sets[index].ways[way];

        
        if (line->valid && line->tag == tag)
        {
            
            l2->hits++;
            return way;
        }
    }
    
   l2->misses++;

    return -1;
}

uint32_t l2_invalidate(L2Cache *l2, uint32_t pa)
{
    uint32_t index;
    uint32_t tag;
    int way;

    if (l2 == NULL)
        return 0;

    // Calculate index and tag from physical address
    index = L2_INDEX(pa);
    tag = L2_TAG(pa);

    // Search through all ways in the set to find the block
    for (way = 0; way < L2_WAYS; way++) {
        L2Line *line = &l2->sets[index].ways[way];

        // If found, invalidate the line and return the evicted physical address
        if (line->valid && line->tag == tag) 
        {
            line->valid = 0;
            return pa;
        }
    }

    // Return 0 to indicate no block was found/invalidated
    return 0;
}

void l2_promote(L2Cache *l2, L1Cache *l1, uint32_t pa)
{
    uint32_t evicted_l1_pa = 0;
    int has_eviction = 0;

    // 1. Invalidate the promoted block from L2 to enforce exclusivity
    l2_invalidate(l2, pa);
    
    // 2. Install the promoted block into L1
    // l1_install populates evicted_l1_pa if a block is displaced
    has_eviction = l1_install(l1, pa, &evicted_l1_pa);

    // 3. Demote any evicted L1 block down to L2
    if (has_eviction) {
        l2_allocate(l2, evicted_l1_pa);
    }
}

