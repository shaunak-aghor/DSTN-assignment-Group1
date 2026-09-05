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

/* Hands the victim's block base back through out_pa and frees the way.
 * Returns 1 if a line was evicted, 0 if the way was already free.  The caller
 * MUST give a returned block a home in L2: L1 and L2 are exclusive, so a block
 * dropped here and placed nowhere is lost. */
int l1_evict(L1Cache *l1, uint32_t index, int way, uint32_t *out_pa)
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

    l1_free_way(set, way);
    return 1;
}

/* Installs pa into its set.  Call after l1_evict has freed a way, which is
 * what makes the placement case the normal one.
 *   -1  nothing installed: NULL cache, or l1_select_victim failed
 *    0  placement   -- the chosen way was free
 *    1  replacement -- the chosen way held a valid line, now overwritten
 * A 1 means the caller skipped the evict, and the overwritten block was
 * handed to nobody.  Under the evict-first sequence it should never happen. */
int l1_install(L1Cache *l1, uint32_t pa)
{
    uint32_t index;
    int      way;
    int      replaced;
    L1Line  *line;

    if (l1 == NULL)
        return -1;

    index = L1_INDEX(pa);
    way   = l1_select_victim(l1, index);

    if (way < 0)
        return -1;

    line     = &l1->sets[index].ways[way];
    replaced = line->valid ? 1 : 0;

    line->valid = 1;
    line->tag   = L1_TAG(pa);

    line->lru = L1_WAYS - 1;    /* enter as oldest, then promote */
    l1_age(l1, index, way);

    return replaced;
}

/* A store that hit in L1.  With no data to patch and no dirty bit, all this
 * does is promote the line -- the store itself reaches memory through the
 * write buffer, which main enqueues.  The tag check makes the header's "donot
 * bring miss to this" enforceable rather than a convention. */
void l1_write_hit(L1Cache *l1, uint32_t pa, int way)
{
    uint32_t index;
    L1Line  *line;

    if (l1 == NULL || way < 0 || way >= L1_WAYS)
        return;

    index = L1_INDEX(pa);       /* masked to 6 bits, always a valid set */
    line  = &l1->sets[index].ways[way];

    if (!line->valid || line->tag != L1_TAG(pa))
        return;                 /* not a hit -- the caller must probe first */

    l1_age(l1, index, way);
}