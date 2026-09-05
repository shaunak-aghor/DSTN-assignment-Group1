#include "L1.h"
#include <stddef.h>
#include <string.h>

void l1_init(L1Cache *l1)
{
    if (l1 == NULL){
        /* Error case */
        return;
    }

    memset(l1, 0, sizeof(*l1));

    for (uint32_t s = 0; s < L1_SETS; s++){
        for (int j = 0; j < L1_WAYS; j++){
            l1->sets[s].ways[j].lru = L1_WAYS - 1;
            /* setting everything to INVALID and LRU(just in case, mostly not needed) */
            l1->sets[s].ways[j].valid = 0;
        }
    }
}

void l1_reset_stats(L1Cache *l1)
{
    if (l1 == NULL){
        /* Error case */
        return;
    }

    l1->read_hits = 0;
    l1->read_misses = 0;
    l1->write_hits = 0;
    l1->write_misses = 0;
    l1->evictions = 0;
}

int l1_probe(L1Cache *l1, uint32_t pa)
{
    uint32_t index;
    uint32_t tag;
    int way;

    if (l1 == NULL)
        return -1;

    index = L1_INDEX(pa);
    tag = L1_TAG(pa);

    for (way = 0; way < L1_WAYS; way++) {
        L1Line *line = &l1->sets[index].ways[way];

        if (line->valid && line->tag == tag) {
            return way;
        }
    }

    return -1;
}

/* The maximum rank value is the number of ranks, L1_WAYS-1.  On a hit the
 * ways ranked below the touched one are incremented and it drops to 0.
 * Invalid ways are neither promoted nor demoted, they are left the same. */
void l1_age(L1Cache *l1, uint32_t index, int way)
{
    L1Set   *set;
    uint32_t rank;

    if (l1 == NULL || index >= L1_SETS || way < 0 || way >= L1_WAYS)
        return;

    set  = &l1->sets[index];
    rank = set->ways[way].lru;

    for (int j = 0; j < L1_WAYS; j++)
        if (j != way && set->ways[j].valid && set->ways[j].lru < rank)
            set->ways[j].lru++;

    set->ways[way].lru = 0;
}

/* Checks for invalid first, if found, then it returns the index, else it
 * returns the LRU way. -1 means error, case handling will be done elsewhere. */
int l1_select_victim(L1Cache *l1, uint32_t index){
    L1Set *set;
    int way = 0;

    if (l1 == NULL || index >= L1_SETS){
        /* not possible, is an error*/
        return -1;
    }

    set = &l1->sets[index];

    for(int j = 0; j < L1_WAYS; j++){
        if(!set->ways[j].valid){
            /* found an invalid way, return it */
            return j;
        }
        if(set->ways[j].lru > set->ways[way].lru){
            way = j;
            /* found an LRU way, but don't return, wait for validity checks */
        }
    }
    return way;
}

/* Frees a way and closes the rank it vacated, so the surviving ranks stay
 * dense.  Without the closing loop a mid-range gap survives the next install
 * and two ways end up sharing a rank. */
static void l1_free_way(L1Set *set, int way)
{
    uint32_t rank = set->ways[way].lru;

    set->ways[way].valid = 0;
    set->ways[way].lru   = L1_WAYS - 1;

    for (int j = 0; j < L1_WAYS; j++)
        if (set->ways[j].valid && set->ways[j].lru > rank)
            set->ways[j].lru--;
}

/* Copies the victim out and frees the way.  Returns 1 if a line was evicted,
 * 0 if the way was already free.  The caller MUST then enqueue it into the
 * write buffer: L1 and L2 are exclusive, so a block dropped here and not
 * buffered is lost.  Victims are always clean, so they never go to memory. */
int l1_evict(L1Cache *l1, uint32_t index, int way,
             uint32_t *out_pa, uint8_t *out_block)
{
    L1Set  *set;
    L1Line *line;

    if (l1 == NULL || index >= L1_SETS || way < 0 || way >= L1_WAYS)
        return 0;

    set  = &l1->sets[index];
    line = &set->ways[way];

    if (!line->valid)
        return 0;

    if (out_pa)
        *out_pa = L1_MAKE_PA(line->tag, index);
    if (out_block)
        memcpy(out_block, line->data, BLOCK_SIZE);

    l1_free_way(set, way);
    l1->evictions++;
    return 1;
}