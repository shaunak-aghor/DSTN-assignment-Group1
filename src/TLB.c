#include <stdio.h>
#include <string.h>
#include "TLB.h"


/*
tlb.h is used to touch the TLB at particular index.Touching it sets lru of entry i to 0 and all entries having rank<tlb[i].rank 
shifts one place to right and all entries with rank>tlb[i].lru keeps its rank
*/
static void tlb_touch(TLB *t, unsigned i)
{
    unsigned j;
    uint32_t rank = t->entries[i].lru;

    for (j = 0; j < TLB_ENTRIES; j++)
        if (j != i && t->entries[j].valid && t->entries[j].lru < rank)
            t->entries[j].lru++;

    t->entries[i].lru = 0;
}


/*
Deleting an entry needs to shift all entries having lru>rank to move one place to left and invalidating entry i
*/
static void tlb_forget(TLB *t, unsigned i)
{
    unsigned j;
    uint32_t rank = t->entries[i].lru;

    t->entries[i].valid = 0;
    t->entries[i].lru   = (uint32_t)(TLB_ENTRIES - 1);

    for (j = 0; j < TLB_ENTRIES; j++)
        if (t->entries[j].valid && t->entries[j].lru > rank)
            t->entries[j].lru--;
}

/* 
this method initializes the tlb and sets lru counter of all entries to TLB_ENTRIES-1
*/
void tlb_init(TLB *t)
{
    unsigned i;

    memset(t, 0, sizeof(*t));
    for (i = 0; i < TLB_ENTRIES; i++)
        t->entries[i].lru = (uint32_t)(TLB_ENTRIES - 1);
}


/*
tlb_probe checks if particular vpn->pfn mapping is present.Returns -1 if search unsuccessful
*/
int tlb_probe(const TLB *t, uint32_t pid, uint32_t vpn)
{
    unsigned i;

    for (i = 0; i < TLB_ENTRIES; i++)
        if (t->entries[i].valid &&
            t->entries[i].pid == (pid & (uint32_t)MASK(PID_BITS)) &&
            t->entries[i].vpn == (vpn & (uint32_t)MASK(VPN_BITS)))
            return (int)i;
    return -1;
}

/* 
check if vpn->pfn mapping is present for a process identified by pid and if present populate pfn_out with physical frame number
*/
int tlb_lookup(TLB *t, uint32_t pid, uint32_t vpn, uint32_t *pfn_out)
{
    int i = tlb_probe(t, pid, vpn);

    if (i < 0)
        return 0;
    if (pfn_out) *pfn_out = t->entries[i].pfn;
    tlb_touch(t, (unsigned)i);
    return 1;
}


/*
Choose a victim for eviction. The entry that has highest value for lru counter is chosen for eviction.
If an invalid entry is found->that entry gets selected as victim
*/
int tlb_select_victim(const TLB *t)
{
    unsigned i;
    int      victim = 0;
    uint32_t oldest = 0;

    for (i = 0; i < TLB_ENTRIES; i++)
        if (!t->entries[i].valid)
            return (int)i;                 /* a free entry costs nothing */

    for (i = 0; i < TLB_ENTRIES; i++)
        if (t->entries[i].lru >= oldest) { /* largest rank = least recent */
            oldest = t->entries[i].lru;
            victim = (int)i;
        }
    return victim;
}

/* This method is used to insert an new vpn->pfn mapping for a given process pid */
int tlb_insert(TLB *t, uint32_t pid, uint32_t vpn, uint32_t pfn)
{
    int displaced;
    int i;

    tlb_invalidate_frame(t, pfn);

    i = tlb_probe(t, pid, vpn);

     /*case when pid vpn combination already exists in TLB */
    if (i >= 0) {
        t->entries[i].pfn = pfn & (uint32_t)MASK(FRAME_BITS);
        tlb_touch(t, (unsigned)i);
        return 0;
    }

    /* CASE when (pid,vpn) combination is not present in TLB in which case victim needs to be chosen*/
    i = tlb_select_victim(t);
    displaced = t->entries[i].valid ? 1 : 0;

    /* populating alloted index i with new (pid,vpn) mapping*/
    t->entries[i].valid = 1;
    t->entries[i].pid   = pid & (uint32_t)MASK(PID_BITS);
    t->entries[i].vpn   = vpn & (uint32_t)MASK(VPN_BITS);
    t->entries[i].pfn   = pfn & (uint32_t)MASK(FRAME_BITS);
    t->entries[i].lru   = (uint32_t)(TLB_ENTRIES - 1);
    tlb_touch(t, (unsigned)i);        /* promote to most recently used */

    return displaced;
}


/* used to invalidate a particular (pid,vpn) entry in TLB */
void tlb_invalidate_entry(TLB *t, uint32_t pid, uint32_t vpn)
{
    int i = tlb_probe(t, pid, vpn);
    if (i >= 0) tlb_forget(t, (unsigned)i);
}

/* Invalidate all entries for a given pid - primarily used when process exits */
void tlb_invalidate_pid(TLB *t, uint32_t pid)
{
    unsigned i;
    for (i = 0; i < TLB_ENTRIES; i++)
        if (t->entries[i].valid &&
            t->entries[i].pid == (pid & (uint32_t)MASK(PID_BITS)))
            tlb_forget(t, i);
}

void tlb_invalidate_frame(TLB *t, uint32_t pfn)
{
    unsigned i;
    for (i = 0; i < TLB_ENTRIES; i++)
        if (t->entries[i].valid &&
            t->entries[i].pfn == (pfn & (uint32_t)MASK(FRAME_BITS)))
            tlb_forget(t, i);
}

/*TLB dump at end of program execution*/
void tlb_dump(const TLB *t)
{
    unsigned i, live = 0;

    printf("\n--- TLB (%d entries, PID-tagged, LRU counter) ---\n", TLB_ENTRIES);
    printf("  idx  V  PID   VPN    PFN   LRU\n");
    for (i = 0; i < TLB_ENTRIES; i++) {
        if (!t->entries[i].valid) continue;
        printf("  %3u  %u  %3u  %4u  %5u  %4u\n", i,
               (unsigned)t->entries[i].valid, (unsigned)t->entries[i].pid,
               (unsigned)t->entries[i].vpn,   (unsigned)t->entries[i].pfn,
               (unsigned)t->entries[i].lru);
        live++;
    }
    if (!live) printf("  (all entries invalid)\n");
    printf("  valid %u/%d\n", live, TLB_ENTRIES);
}
