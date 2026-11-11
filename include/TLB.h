#ifndef TLB_H
#define TLB_H

#include <stdint.h>
#include "MemHier.h"

/*
 * TLB: 32 entries, fully associative, tagged with the process ID so
 * a context switch does not need a flush.
 *
 * Entry = 43 bits: valid 1 + vpn 8 + pfn 15 + pid 14 + lru 5
 * Replacement: LRU counter per entry (0 = least recently used, 31 = most recently used).
 */

#define TLB_LRU_BITS 5                  /* log2(TLB_ENTRIES) */

typedef struct {
    uint32_t valid : 1;
    uint32_t vpn   : VPN_BITS;
    uint32_t pfn   : FRAME_BITS;
    uint32_t pid   : PID_BITS;
    uint32_t lru   : TLB_LRU_BITS;   /* 0 = LRU, TLB_ENTRIES-1 = MRU */
} TLBEntry;

typedef struct {
    TLBEntry entries[TLB_ENTRIES];
} TLB;

/* Clears every entry. */
void tlb_init(TLB *t);

/* Looks up (pid, vpn). On a hit writes the frame to *pfn_out and returns 1. */
int  tlb_lookup(TLB *t, uint32_t pid, uint32_t vpn, uint32_t *pfn_out);

/* Adds the mapping (pid, vpn) -> pfn, replacing the LRU entry if full. */
void tlb_insert(TLB *t, uint32_t pid, uint32_t vpn, uint32_t pfn);

/* Removes the entry for one page, if present. */
void tlb_invalidate_entry(TLB *t, uint32_t pid, uint32_t vpn);

/* Removes every entry that points at frame pfn. */
void tlb_invalidate_frame(TLB *t, uint32_t pfn);

#endif
