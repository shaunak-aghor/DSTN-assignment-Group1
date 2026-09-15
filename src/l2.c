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
            return way;
        }
    }

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

/* Promotes a block from L2 to L1 */
void l2_promote(L2Cache *l2, L1Cache *l1, uint32_t pa)
{
    uint32_t evicted_l1_pa = 0;
    int has_eviction = 0;

    // 1. Invalidate the promoted block from L2 to enforce exclusivity
    l2_invalidate(l2, pa);
    
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

/* Ages a line in L2 cache to align with FIFO */
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

/* selects a victim and returns its index */
int l2_select_victim(L2Cache *l2, uint32_t index)
{
    L2Set *set;
    int oldest_way = 0;
    int min_count = L2_WAYS;

    if (l2 == NULL || index >= L2_SETS)
        return -1;

    set = &l2->sets[index];

    /* If any way is invalid, return it immediately */
    for (int j = 0; j < L2_WAYS; j++) {
        if (!set->ways[j].valid) {
            return j;
        }
    }

    /* All ways valid: find the way with the fewest 1s in its row (oldest) */
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

/* Allocates a line in L2 cache from a given pa */
int l2_allocate(L2Cache *l2, uint32_t pa)
{
    uint32_t index;
    uint32_t tag;
    int way;
    int displaced;
    L2Line *line;

    if (l2 == NULL)
        return 0;

    index = L2_INDEX(pa);
    tag = L2_TAG(pa);
    way = l2_select_victim(l2, index);

    if (way < 0 || way >= L2_WAYS)
        return 0;

    line = &l2->sets[index].ways[way];
    displaced = line->valid ? 1 : 0;

    line->valid = 1;
    line->tag = tag;

    l2_age(l2, index, way);

    return displaced;
}


/* L2's tag and index do not line up with the page offset the way L1's do, so
 * each line's frame is reconstructed from (tag, index) and compared.  No FIFO
 * repair is needed: l2_select_victim skips invalid ways, and l2_age rewrites
 * the row and column when the way is refilled. */
int l2_invalidate_frame(L2Cache *l2, uint32_t frame)
{
    int dropped = 0;

    if (l2 == NULL)
        return 0;

    for (uint32_t s = 0; s < L2_SETS; s++)
        for (int w = 0; w < L2_WAYS; w++) {
            L2Line *line = &l2->sets[s].ways[w];

            if (line->valid &&
                (uint32_t)PA_FRAME(L2_MAKE_PA(line->tag, s)) == frame) {
                line->valid = 0;
                dropped++;
            }
        }

    return dropped;
}
