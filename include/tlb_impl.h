#ifndef TLB_IMPL_H
#define TLB_IMPL_H

#include <stdint.h>
#include "MemHier.h"

/*
 * Working implementation of the identifier(PID)-based TLB.
 *
 * Kept in its own header so that include/TLB.h is left untouched; merge the
 * two by renaming TLBImplEntry -> TLB_Entry, TLBImpl -> TLB and dropping the
 * tlb_impl_ prefix.  Note that TLB.h currently declares vpn as 22 bits, which
 * does not agree with VPN_BITS (8) in MemHier.h -- everything below follows
 * MemHier.h.
 *
 * Entry layout = 43 bits
 *     valid 1 + vpn VPN_BITS(8) + pfn FRAME_BITS(15) + pid PID_BITS(14) + lru 5
 *
 * Fully associative: all TLB_ENTRIES entries are compared on the pair
 * (pid, vpn).  Because every entry carries its owner's PID, the TLB is NOT
 * flushed on a context switch; entries die only when the page they map is
 * evicted or when the owning process terminates.
 *
 * Replacement: LRU counter, same convention as L1Line.lru -- 0 = most
 * recently used, TLB_ENTRIES-1 = least recently used.  The ranks of the
 * valid entries are always a permutation of 0 .. (number of valid - 1).
 */

#define TLB_LRU_BITS 5              /* log2(TLB_ENTRIES) = log2(32) */

typedef struct {
    uint32_t valid : 1;
    uint32_t vpn   : VPN_BITS;      /* 8  */
    uint32_t pfn   : FRAME_BITS;    /* 15 */
    uint32_t pid   : PID_BITS;      /* 14 */
    uint32_t lru   : TLB_LRU_BITS;  /* 5, 0 = most recently used */
} TLBImplEntry;

typedef struct {
    TLBImplEntry entries[TLB_ENTRIES];
    /* statistics */
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;             /* capacity evictions only */
} TLBImpl;

/* lifecycle */
void tlb_impl_init(TLBImpl *t);
void tlb_impl_reset_stats(TLBImpl *t);

/* Returns the matching entry index, or -1 on a miss. Does NOT touch the LRU
 * ranks and does NOT count a hit or a miss -- use it for inspection. */
int  tlb_impl_probe(const TLBImpl *t, uint32_t pid, uint32_t vpn);

/* The real lookup: on a hit returns 1, writes the frame through pfn_out and
 * makes the entry most recently used. On a miss returns 0. Counts both. */
int  tlb_impl_lookup(TLBImpl *t, uint32_t pid, uint32_t vpn, uint32_t *pfn_out);

/* Returns an invalid entry if there is one, otherwise the least recently
 * used entry. Always returns a valid index in 0 .. TLB_ENTRIES-1. */
int  tlb_impl_select_victim(const TLBImpl *t);

/* Fill after a page-table walk. Updates in place if (pid,vpn) is already
 * present, so at most one entry ever exists for a given pair. */
void tlb_impl_insert(TLBImpl *t, uint32_t pid, uint32_t vpn, uint32_t pfn);

/* Invalidation -- each of these repairs a specific way the TLB can go stale */
void tlb_impl_invalidate_entry(TLBImpl *t, uint32_t pid, uint32_t vpn); /* page evicted   */
void tlb_impl_invalidate_pid(TLBImpl *t, uint32_t pid);                 /* process exited */
void tlb_impl_invalidate_frame(TLBImpl *t, uint32_t pfn);               /* frame reused   */

void tlb_impl_dump(const TLBImpl *t);

#endif /* TLB_IMPL_H */
