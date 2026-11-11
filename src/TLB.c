#include <string.h>
#include "TLB.h"
#include "stats.h"

/*
 * LRU counters: 0 = least recently used, TLB_ENTRIES-1 = most recently used.
 * The k valid entries always hold the top k ranks (N-k .. N-1), so when the
 * TLB is full the ranks are exactly 0 .. N-1 and the victim is the one at 0.
 */

/* Makes entry i the most recently used: everyone above it moves down one. */
static void tlb_touch(TLB *t, int i)
{
    uint32_t old_rank = t->entries[i].lru;

    for (int j = 0; j < TLB_ENTRIES; j++)
        if (j != i && t->entries[j].valid && t->entries[j].lru > old_rank)
            t->entries[j].lru--;

    t->entries[i].lru = TLB_ENTRIES - 1;
}

/* Invalidates entry i: everyone below it moves up one to close the gap. */
static void tlb_remove(TLB *t, int i)
{
    uint32_t old_rank = t->entries[i].lru;

    for (int j = 0; j < TLB_ENTRIES; j++)
        if (t->entries[j].valid && t->entries[j].lru < old_rank)
            t->entries[j].lru++;

    t->entries[i].valid = 0;
    t->entries[i].lru   = 0;
}

/* Returns the index of the entry for (pid, vpn), or -1. */
static int tlb_find(const TLB *t, uint32_t pid, uint32_t vpn)
{
    for (int i = 0; i < TLB_ENTRIES; i++)
        if (t->entries[i].valid && t->entries[i].pid == pid && t->entries[i].vpn == vpn)
            return i;
    return -1;
}

/* Picks a free entry if there is one, otherwise the least recently used. */
static int tlb_pick_victim(const TLB *t)
{
    int victim = 0;

    for (int i = 0; i < TLB_ENTRIES; i++) {
        if (!t->entries[i].valid)
            return i;
        if (t->entries[i].lru < t->entries[victim].lru)
            victim = i;
    }
    return victim;
}

void tlb_init(TLB *t)
{
    memset(t, 0, sizeof(*t));           /* all invalid, all ranks 0 */
}

int tlb_lookup(TLB *t, uint32_t pid, uint32_t vpn, uint32_t *pfn_out)
{
    int i = tlb_find(t, pid, vpn);

    if (i < 0) {
        stats.tlb_misses++;
        return 0;
    }
    stats.tlb_hits++;
    *pfn_out = t->entries[i].pfn;
    tlb_touch(t, i);
    return 1;
}

void tlb_insert(TLB *t, uint32_t pid, uint32_t vpn, uint32_t pfn)
{
    int i;

    /* A frame maps to only one page, so drop any stale entry for it first. */
    tlb_invalidate_frame(t, pfn);

    /* Already cached: just refresh it (never keep two entries for one page). */
    i = tlb_find(t, pid, vpn);
    if (i >= 0) {
        t->entries[i].pfn = pfn;
        tlb_touch(t, i);
        return;
    }

    i = tlb_pick_victim(t);
    if (t->entries[i].valid)
        stats.tlb_evictions++;

    t->entries[i].valid = 1;
    t->entries[i].pid   = pid;
    t->entries[i].vpn   = vpn;
    t->entries[i].pfn   = pfn;
    t->entries[i].lru   = 0;            /* enter below everyone, then promote */
    tlb_touch(t, i);
}

void tlb_invalidate_entry(TLB *t, uint32_t pid, uint32_t vpn)
{
    int i = tlb_find(t, pid, vpn);
    if (i >= 0)
        tlb_remove(t, i);
}

void tlb_invalidate_frame(TLB *t, uint32_t pfn)
{
    for (int i = 0; i < TLB_ENTRIES; i++)
        if (t->entries[i].valid && t->entries[i].pfn == pfn)
            tlb_remove(t, i);
}
