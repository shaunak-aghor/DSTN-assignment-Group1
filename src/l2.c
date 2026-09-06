#include "L2.h"
#include <string.h>
#include <stddef.h>
#include "L1.h"

void l2_init(L2Cache *l2)
{
    if (l2 == NULL)
        return;

    memset(l2, 0, sizeof(*l2));
}

void l2_reset_stats(L2Cache *l2)
{
    if (l2 == NULL)
        return;

    l2->hits = 0;
    l2->misses = 0;
    l2->evictions = 0;
    l2->promotions = 0;
    l2->passthrough_writes = 0;
    l2->updated_writes = 0;
}

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

    index = L2_INDEX(pa);
    tag = L2_TAG(pa);

    for (way = 0; way < L2_WAYS; way++) {
        L2Line *line = &l2->sets[index].ways[way];

        if (line->valid && line->tag == tag) 
        {
            line->valid = 0;
            return pa;
        }
    }

    return 0;
}

void l2_promote(L2Cache *l2, L1Cache *l1, uint32_t pa)
{
    uint32_t evicted_l1_pa = 0;
    int has_eviction = 0;

    // 1. Invalidate the promoted block from L2 to enforce exclusivity
    if (l2_invalidate(l2, pa)) {
        l2->promotions++;
    }
    
    // 2. Install the promoted block into L1
    // 2. Extract victim from L1 safely before overwriting
    uint32_t index = L1_INDEX(pa);
    int way = l1_select_victim(l1, index);
    has_eviction = l1_evict(l1, index, way, &evicted_l1_pa);

    // 3. Install the promoted block into L1 safely
    l1_install(l1, pa);

    // 3. Demote any evicted L1 block down to L2
    if (has_eviction) {
        l2_allocate(l2, evicted_l1_pa);
    }
}

void l2_age(L2Cache *l2, uint32_t index, int way)
{
    L2Set *set;

    if (l2 == NULL || index >= L2_SETS || way < 0 || way >= L2_WAYS)
        return;

    set = &l2->sets[index];

    /* Set row 'way' to all 1s (except diagonal): way is younger than all other ways */
    set->fifo_matrix[way] = (uint8_t)(0xFF & ~(1 << way));

    /* Clear column 'way' in all other rows: no other way is younger than way */
    for (int i = 0; i < L2_WAYS; i++) {
        if (i != way) {
            set->fifo_matrix[i] &= (uint8_t)~(1 << way);
        }
    }
}

int l2_select_victim(L2Cache *l2, uint32_t index)
{
    L2Set *set;
    int oldest_way = 0;
    int min_count = L2_WAYS;

    if (l2 == NULL || index >= L2_SETS)
        return -1;

    set = &l2->sets[index];

    /* 1. If any way is invalid, return it immediately */
    for (int j = 0; j < L2_WAYS; j++) {
        if (!set->ways[j].valid) {
            return j;
        }
    }

    /* 2. All ways valid: find the way with the fewest 1s in its row (oldest) */
    for (int i = 0; i < L2_WAYS; i++) {
        int count = 0;
        for (int j = 0; j < L2_WAYS; j++) {
            if (j != i && set->ways[j].valid && ((set->fifo_matrix[i] >> j) & 1)) {
                count++;
            }
        }
        if (count == 0) {
            return i; /* Oldest line is younger than no other valid line */
        }
        if (count < min_count) {
            min_count = count;
            oldest_way = i;
        }
    }

    return oldest_way;
}

void l2_allocate(L2Cache *l2, uint32_t pa)
{
    uint32_t index;
    uint32_t tag;
    int way;
    L2Line *line;

    if (l2 == NULL)
        return;

    index = L2_INDEX(pa);
    tag = L2_TAG(pa);
    way = l2_select_victim(l2, index);

    if (way < 0 || way >= L2_WAYS)
        return;

    line = &l2->sets[index].ways[way];

    if (line->valid) {
        l2->evictions++;
    }

    line->valid = 1;
    line->tag = tag;

    l2_age(l2, index, way);
}

