#include <string.h>
#include "L1.h"

/*
 * LRU counters: 0 = least recently used, 3 = most recently used.
 * The k valid ways of a set always hold the top k ranks, so in a full
 * set the ranks are exactly 0..3 and the victim is the way at 0.
 */

void l1_init(L1Cache *l1)
{
    memset(l1, 0, sizeof(*l1));         /* all invalid, all ranks 0 */
}

int l1_probe(L1Cache *l1, uint32_t pa)
{
    L1Set *set = &l1->sets[L1_INDEX(pa)];
    uint32_t tag = L1_TAG(pa);

    for (int w = 0; w < L1_WAYS; w++)
        if (set->ways[w].valid && set->ways[w].tag == tag)
            return w;
    return -1;
}

/* Makes `way` the most recently used: every way above it moves down one. */
void l1_age(L1Cache *l1, uint32_t index, int way)
{
    L1Set *set = &l1->sets[index];
    uint32_t old_rank = set->ways[way].lru;

    for (int w = 0; w < L1_WAYS; w++)
        if (w != way && set->ways[w].valid && set->ways[w].lru > old_rank)
            set->ways[w].lru--;

    set->ways[way].lru = L1_WAYS - 1;
}

int l1_select_victim(L1Cache *l1, uint32_t index)
{
    L1Set *set = &l1->sets[index];
    int victim = 0;

    for (int w = 0; w < L1_WAYS; w++) {
        if (!set->ways[w].valid)
            return w;                   /* free way: nothing has to leave */
        if (set->ways[w].lru < set->ways[victim].lru)
            victim = w;
    }
    return victim;
}

/* Invalidates `way`: every way below it moves up one to close the gap. */
static void l1_free_way(L1Set *set, int way)
{
    uint32_t old_rank = set->ways[way].lru;

    for (int w = 0; w < L1_WAYS; w++)
        if (set->ways[w].valid && set->ways[w].lru < old_rank)
            set->ways[w].lru++;

    set->ways[way].valid = 0;
    set->ways[way].lru   = 0;
}

int l1_evict(L1Cache *l1, uint32_t index, int way, uint32_t *out_pa)
{
    L1Line *line = &l1->sets[index].ways[way];

    if (!line->valid)
        return 0;

    *out_pa = L1_MAKE_PA(line->tag, index);
    l1_free_way(&l1->sets[index], way);
    return 1;
}

void l1_install(L1Cache *l1, uint32_t pa)
{
    uint32_t index = L1_INDEX(pa);
    int way = l1_select_victim(l1, index);
    L1Line *line = &l1->sets[index].ways[way];

    line->valid = 1;
    line->tag   = L1_TAG(pa);
    line->lru   = 0;                    /* enter below everyone, then promote */
    l1_age(l1, index, way);
}

/* L1's index + offset is exactly the 10-bit page offset, so the L1 tag IS the frame number. */
void l1_invalidate_frame(L1Cache *l1, uint32_t frame)
{
    for (int s = 0; s < L1_SETS; s++)
        for (int w = 0; w < L1_WAYS; w++)
            if (l1->sets[s].ways[w].valid && l1->sets[s].ways[w].tag == frame)
                l1_free_way(&l1->sets[s], w);
}
