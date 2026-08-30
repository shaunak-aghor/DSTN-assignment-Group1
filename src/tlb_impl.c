#include <stdio.h>
#include <string.h>
#include "tlb_impl.h"

/* =====================================================================
 * LRU counter maintenance
 *
 * Ranks of the valid entries are a permutation of 0 .. (valid_count - 1),
 * with 0 = most recently used.  Making entry i the most recent means every
 * entry that was more recent than i ages by one, and i drops to 0.  Entries
 * that were already older than i keep their rank, so the permutation is
 * preserved and nothing can overflow TLB_LRU_BITS.
 *
 * A fresh fill sets lru = TLB_ENTRIES-1 first, which makes it the oldest,
 * and the same routine then promotes it -- so fill and hit share one path.
 * ===================================================================== */
static void tlb_impl_touch(TLBImpl *t, unsigned i)
{
    unsigned j;
    uint32_t rank = t->entries[i].lru;

    for (j = 0; j < TLB_ENTRIES; j++)
        if (j != i && t->entries[j].valid && t->entries[j].lru < rank)
            t->entries[j].lru++;

    t->entries[i].lru = 0;
}

/* The mirror of tlb_impl_touch: dropping entry i would leave a hole in the
 * rank sequence, so every entry older than i closes up by one.  Without this
 * the ranks stay correctly *ordered* but stop being a permutation, and the
 * invariant the rest of the file relies on would be only half true. */
static void tlb_impl_forget(TLBImpl *t, unsigned i)
{
    unsigned j;
    uint32_t rank = t->entries[i].lru;

    t->entries[i].valid = 0;
    t->entries[i].lru   = (uint32_t)(TLB_ENTRIES - 1);

    for (j = 0; j < TLB_ENTRIES; j++)
        if (t->entries[j].valid && t->entries[j].lru > rank)
            t->entries[j].lru--;
}

void tlb_impl_init(TLBImpl *t)
{
    unsigned i;

    memset(t, 0, sizeof(*t));
    for (i = 0; i < TLB_ENTRIES; i++)
        t->entries[i].lru = (uint32_t)(TLB_ENTRIES - 1);
}

void tlb_impl_reset_stats(TLBImpl *t)
{
    t->hits = t->misses = t->evictions = 0;
}

/* ---------------------------------------------------------------------
 * Lookup
 *
 * Three conditions decide a hit and all three matter: valid rejects stale
 * leftovers, pid keeps two processes that use the same VPN apart, and vpn
 * is the page being asked for.  Dropping the pid test is what would make
 * this an ordinary TLB that has to be flushed on every context switch.
 * ------------------------------------------------------------------ */
int tlb_impl_probe(const TLBImpl *t, uint32_t pid, uint32_t vpn)
{
    unsigned i;

    for (i = 0; i < TLB_ENTRIES; i++)
        if (t->entries[i].valid &&
            t->entries[i].pid == (pid & (uint32_t)MASK(PID_BITS)) &&
            t->entries[i].vpn == (vpn & (uint32_t)MASK(VPN_BITS)))
            return (int)i;
    return -1;
}

int tlb_impl_lookup(TLBImpl *t, uint32_t pid, uint32_t vpn, uint32_t *pfn_out)
{
    int i = tlb_impl_probe(t, pid, vpn);

    if (i < 0) {
        t->misses++;
        return 0;
    }
    if (pfn_out) *pfn_out = t->entries[i].pfn;
    tlb_impl_touch(t, (unsigned)i);
    t->hits++;
    return 1;
}

/* ---------------------------------------------------------------------
 * Replacement
 * ------------------------------------------------------------------ */
int tlb_impl_select_victim(const TLBImpl *t)
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

void tlb_impl_insert(TLBImpl *t, uint32_t pid, uint32_t vpn, uint32_t pfn)
{
    int i = tlb_impl_probe(t, pid, vpn);

    /* Already cached: refresh the mapping instead of creating a duplicate.
     * This is what guarantees at most one entry per (pid, vpn) whatever
     * order the caller uses. */
    if (i >= 0) {
        t->entries[i].pfn = pfn & (uint32_t)MASK(FRAME_BITS);
        tlb_impl_touch(t, (unsigned)i);
        return;
    }

    i = tlb_impl_select_victim(t);
    if (t->entries[i].valid)
        t->evictions++;                    /* a real capacity eviction */

    t->entries[i].valid = 1;
    t->entries[i].pid   = pid & (uint32_t)MASK(PID_BITS);
    t->entries[i].vpn   = vpn & (uint32_t)MASK(VPN_BITS);
    t->entries[i].pfn   = pfn & (uint32_t)MASK(FRAME_BITS);
    t->entries[i].lru   = (uint32_t)(TLB_ENTRIES - 1);
    tlb_impl_touch(t, (unsigned)i);        /* promote to most recently used */
}

/* ---------------------------------------------------------------------
 * Invalidation
 *
 * A valid TLB entry is an assertion that the page table still says the same
 * thing.  Every event that can falsify it has to reach in here, or a later
 * hit would hand out a frame the process no longer owns.
 * ------------------------------------------------------------------ */
void tlb_impl_invalidate_entry(TLBImpl *t, uint32_t pid, uint32_t vpn)
{
    int i = tlb_impl_probe(t, pid, vpn);
    if (i >= 0) tlb_impl_forget(t, (unsigned)i);
}

void tlb_impl_invalidate_pid(TLBImpl *t, uint32_t pid)
{
    unsigned i;
    for (i = 0; i < TLB_ENTRIES; i++)
        if (t->entries[i].valid &&
            t->entries[i].pid == (pid & (uint32_t)MASK(PID_BITS)))
            tlb_impl_forget(t, i);
}

void tlb_impl_invalidate_frame(TLBImpl *t, uint32_t pfn)
{
    unsigned i;
    for (i = 0; i < TLB_ENTRIES; i++)
        if (t->entries[i].valid &&
            t->entries[i].pfn == (pfn & (uint32_t)MASK(FRAME_BITS)))
            tlb_impl_forget(t, i);
}

/* ------------------------------------------------------------------ */
void tlb_impl_dump(const TLBImpl *t)
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
    printf("  valid %u/%d | hits %llu | misses %llu | evictions %llu\n",
           live, TLB_ENTRIES,
           (unsigned long long)t->hits,
           (unsigned long long)t->misses,
           (unsigned long long)t->evictions);
}
