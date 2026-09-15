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
} TLB;

/* Invalidates every entry. */
void tlb_init(TLB *t);

/* Returns the entry holding (pid,vpn), or WAY_NONE.  No side effects. */
int  tlb_probe(const TLB *t, uint32_t pid, uint32_t vpn);

/* Translates (pid,vpn) into *pfn_out; returns 1 on a hit, 0 on a miss. */
int  tlb_lookup(TLB *t, uint32_t pid, uint32_t vpn, uint32_t *pfn_out);

/* Returns an invalid entry if there is one, else the least recently used. */
int  tlb_select_victim(const TLB *t);

/* Caches (pid,vpn)->pfn; returns 1 if it displaced a valid entry. */
int  tlb_insert(TLB *t, uint32_t pid, uint32_t vpn, uint32_t pfn);

/* Invalidates the entry for one page. */
void tlb_invalidate_entry(TLB *t, uint32_t pid, uint32_t vpn);

/* Invalidates every entry belonging to one process. */
void tlb_invalidate_pid(TLB *t, uint32_t pid);

/* Invalidates every entry mapping one physical frame. */
void tlb_invalidate_frame(TLB *t, uint32_t pfn);

/* Prints the valid entries. */
void tlb_dump(const TLB *t);

#endif /* TLB_H */
