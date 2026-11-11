#include <string.h>
#include "L2.h"
#include "stats.h"

void l2_init(L2Cache *l2)
{
    memset(l2, 0, sizeof(*l2));
}

int l2_probe(L2Cache *l2, uint32_t pa)
{
    L2Set *set = &l2->sets[L2_INDEX(pa)];
    uint32_t tag = L2_TAG(pa);

    for (int w = 0; w < L2_WAYS; w++)
        if (set->ways[w].valid && set->ways[w].tag == tag)
            return w;
    return -1;
}

void l2_invalidate(L2Cache *l2, uint32_t pa)
{
    int way = l2_probe(l2, pa);
    if (way >= 0)
        l2->sets[L2_INDEX(pa)].ways[way].valid = 0;
}

/* Returns a free way if there is one, else the oldest way (the FIFO pointer),
 * and moves the pointer on to the next way. */
static int l2_select_victim(L2Set *set)
{
    int victim;

    for (int w = 0; w < L2_WAYS; w++)
        if (!set->ways[w].valid)
            return w;

    victim = set->fifo_next;
    set->fifo_next = (set->fifo_next + 1) % L2_WAYS;
    return victim;
}

void l2_allocate(L2Cache *l2, uint32_t pa)
{
    L2Set *set = &l2->sets[L2_INDEX(pa)];
    int way = l2_select_victim(set);

    /* L2 is write-through, so a replaced line is clean and can just be dropped. */
    if (set->ways[way].valid)
        stats.l2_evictions++;

    set->ways[way].valid = 1;
    set->ways[way].tag   = L2_TAG(pa);
}

/* L2's tag does not line up with the frame number, so rebuild each line's address and compare. */
void l2_invalidate_frame(L2Cache *l2, uint32_t frame)
{
    for (int s = 0; s < L2_SETS; s++)
        for (int w = 0; w < L2_WAYS; w++) {
            L2Line *line = &l2->sets[s].ways[w];
            if (line->valid && PA_FRAME(L2_MAKE_PA(line->tag, s)) == frame)
                line->valid = 0;
        }
}
