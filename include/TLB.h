#ifndef TLB_H
#define TLB_H

#include <stdint.h>
#include "MemHier.h"

/*
 * Identifier(PID)-based TLB.
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
} TLBEntry;

typedef struct {
    TLBEntry entries[TLB_ENTRIES];
    /* statistics */
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;             /* capacity evictions only */
} TLB;

/* lifecycle */
void tlb_init(TLB *t);
void tlb_reset_stats(TLB *t);

/* Returns the matching entry index, or -1 on a miss. Does NOT touch the LRU
 * ranks and does NOT count a hit or a miss -- use it for inspection. */
int  tlb_probe(const TLB *t, uint32_t pid, uint32_t vpn);

/* The real lookup: on a hit returns 1, writes the frame through pfn_out and
 * makes the entry most recently used. On a miss returns 0. Counts both. */
int  tlb_lookup(TLB *t, uint32_t pid, uint32_t vpn, uint32_t *pfn_out);

/* Returns an invalid entry if there is one, otherwise the least recently
 * used entry. Always returns a valid index in 0 .. TLB_ENTRIES-1. */
int  tlb_select_victim(const TLB *t);

/* Fill after a page-table walk. Updates in place if (pid,vpn) is already
 * present, so at most one entry ever exists for a given pair; and drops any
 * entry still claiming pfn, so at most one entry ever maps a given frame. */
void tlb_insert(TLB *t, uint32_t pid, uint32_t vpn, uint32_t pfn);

/* Invalidation -- each of these repairs a specific way the TLB can go stale */
void tlb_invalidate_entry(TLB *t, uint32_t pid, uint32_t vpn); /* page evicted   */
void tlb_invalidate_pid(TLB *t, uint32_t pid);                 /* process exited */
void tlb_invalidate_frame(TLB *t, uint32_t pfn);               /* frame reused   */

void tlb_dump(const TLB *t);

#endif /* TLB_H */
