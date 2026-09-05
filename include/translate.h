/* ============================================================================
 *  include/translate.h -- the MMU: virtual address -> physical address
 *
 *  This module owns the COMPLETE resolution path for one memory reference.
 *  It is additive: it does not modify TLB.h, MainMemory.h, L1.h, L2.h, WB.h
 *  or cache.h, and none of those need to know it exists.
 *
 *  =====================================================================
 *   THE FLOW
 *  =====================================================================
 *
 *   CPU issues an 18-bit virtual address
 *          |
 *   [0] VA >= 2^18 ?  ------------------------------> MMU_FAULT_ADDRESS
 *          |
 *   [1] split: VPN = VA[17:10] (8 b) , offset = VA[9:0] (10 b)
 *          |
 *   [2] process active and page table resident ? ---> MMU_FAULT_NO_PROCESS
 *          |
 *   [3] TLB lookup on the PAIR (pid, VPN)
 *          |
 *          +-- HIT --> frame number, 0 memory accesses ------> [8]
 *          |
 *          +-- MISS
 *               |
 *   [4]         page-table walk: read PTE #VPN from the process's
 *               page table, which lives in main memory and is never
 *               cached.  COSTS ONE REAL MAIN-MEMORY ACCESS.
 *               |
 *   [5]         PTE.prot permits this access type ? --> MMU_FAULT_PROTECTION
 *               |                                        (checked BEFORE any
 *               |                                         disk I/O is done)
 *   [6]         PTE.present ?
 *               |
 *               +-- yes --> frame number ---------------------> [7]
 *               |
 *               +-- no  --> PAGE FAULT
 *                            a. process at upper_limit ?
 *                                 -> evict one of ITS OWN pages
 *                            b. else a free frame exists ?
 *                                 -> take it
 *                            c. else global LFU-with-aging victim,
 *                               skipping page-table frames and any
 *                               process already at its lower_limit
 *                            d. no victim at all ----------> MMU_FAULT_NO_FRAME
 *                            e. victim dirty -> write to disk
 *                            f. clear the VICTIM OWNER's PTE
 *                            g. INVALIDATE the victim frame in the TLB
 *                               and in L1/L2   <-- see the note below
 *                            h. read the new page from disk
 *                            i. PTE.present = 1, frame = f, aging = 0x80
 *                            j. instruction restarts
 *               |
 *   [7]         install (pid, VPN) -> frame in the TLB, evicting the
 *               LRU entry if the TLB is full
 *               |
 *   [8] PA = (frame << 10) | offset          <-- 25 bits
 *       set PTE.referenced, bump the frame's LFU count,
 *       and on a write set PTE.dirty
 *          |
 *       hand the PA to the cache hierarchy
 *
 *  Because the L1 index and block offset together are exactly 10 bits --
 *  the page offset -- steps [3] and the L1 set lookup can run in parallel:
 *  L1 is virtually indexed and physically tagged with zero aliasing, and the
 *  15-bit L1 tag is literally the frame number this module returns.
 *
 *  =====================================================================
 *   KNOWN LIMITATION -- protection is only checked on a TLB MISS
 *  =====================================================================
 *  TLBImplEntry has no protection bits, so on a TLB hit there is nothing to
 *  check against.  Re-reading the PTE on every hit would mean a main-memory
 *  access per reference and would defeat the TLB entirely, so this module
 *  checks protection at fill time only.  Consequence: revoking write
 *  permission on a resident page is not observed until that page's TLB entry
 *  is invalidated.  The fix is one field -- add `prot : 3` to TLBImplEntry
 *  and carry it through tlb_impl_insert() -- but that means editing TLB.h,
 *  which this module deliberately leaves alone.
 *
 *  =====================================================================
 *   CACHE INVALIDATION ON FRAME REUSE -- REQUIRED, NOT OPTIONAL
 *  =====================================================================
 *  L1 and L2 are PHYSICALLY tagged.  When a frame is reclaimed and refilled
 *  with a different page, any surviving cache line at those physical
 *  addresses would return the OLD page's bytes.  This module invalidates the
 *  TLB itself; the cache side is delegated through
 *  mmu_set_cache_invalidator() because l1.c and l2.c are owned elsewhere.
 *
 *  Whoever implements it needs this address arithmetic.  A 1 KB frame holds
 *  PAGE_SIZE / BLOCK_SIZE = 64 blocks, and for frame f:
 *
 *      L1:  tag == f          (the L1 tag IS the frame number, 15 bits)
 *           scan all 64 sets x 4 ways, invalidate every line whose tag == f
 *
 *      L2:  tag   == f >> 2
 *           sets  == ((f & 0x3) << 6) | 0..63      (64 consecutive sets)
 *           because L2's 8-bit index takes its top 2 bits from the frame
 *
 *  Until that callback is registered the simulator still runs, but a frame
 *  reuse can hand back stale data -- so register it before trusting results.
 * ==========================================================================*/

#ifndef TRANSLATE_H
#define TRANSLATE_H

#include <stdint.h>
#include <stdio.h>

#include "MemHier.h"
#include "TLB.h"
#include "MainMemory.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------
 *  Hooks that MainMemory.c publishes
 *
 *  MainMemory.h has no fields to hold these and this project does not modify
 *  that header, so MainMemory.c keeps them in file scope.  There is one main
 *  memory in the simulator, so that is sufficient.  Both are declared here
 *  rather than in MainMemory.h to keep that header untouched.
 * --------------------------------------------------------------------- */

/* Called immediately before a frame is handed to a new page. */
typedef void (*MMEvictNotifyFn)(void *ctx, uint32_t frame,
                                uint16_t pid, uint32_t vpn);

void mm_set_evict_notifier(MMEvictNotifyFn fn, void *ctx);

/* Give MainMemory the full process table.  Without this, mm_handle_fault can
 * only see the faulting process, and GLOBAL replacement would neither honour
 * other processes' lower_limits nor clear a victim owner's PTE. Register it
 * once at start-up. mmu_init() does this for you. */
void mm_set_process_table(Process *procs, uint16_t num_procs);

/* ------------------------------------------------------------------------
 *  Access types and outcomes
 * --------------------------------------------------------------------- */

typedef enum {
    ACC_READ  = 0,
    ACC_WRITE = 1,
    ACC_EXEC  = 2
} AccessType;

typedef enum {
    MMU_OK              = 0,  /* TLB hit; *pa valid; 0 memory accesses      */
    MMU_OK_TLB_MISS     = 1,  /* PTE was present; TLB filled; *pa valid     */
    MMU_OK_PAGE_FAULT   = 2,  /* page brought in; *pa valid; restart the
                                 instruction and add the fault penalty      */
    MMU_FAULT_ADDRESS   = 3,  /* VA outside the 18-bit address space        */
    MMU_FAULT_PROTECTION= 4,  /* PTE.prot forbids this access type          */
    MMU_FAULT_NO_FRAME  = 5,  /* no evictable frame anywhere -- OOM         */
    MMU_FAULT_NO_PROCESS= 6   /* process inactive or page table not resident*/
} MMUStatus;

const char *mmu_status_name(MMUStatus s);

/* 1 for the three MMU_OK_* outcomes, 0 for every fault. */
static inline int mmu_ok(MMUStatus s) { return s <= MMU_OK_PAGE_FAULT; }

/* ------------------------------------------------------------------------
 *  The MMU
 * --------------------------------------------------------------------- */

/* Invalidate every L1 and L2 line belonging to a physical frame. */
typedef void (*CacheInvalidateFn)(void *ctx, uint32_t frame);

typedef struct {
    TLBImpl     tlb;
    MainMemory *mm;
    Process    *procs;
    uint16_t    num_procs;

    CacheInvalidateFn cache_inval;
    void             *cache_ctx;

    /* Aging timer: mm_age_tick() runs every `tick_interval` accesses.
     * Aging is a periodic sample of the reference bits, not a per-access
     * operation -- that is what makes it cheap. 0 disables it. */
    uint64_t tick_interval;
    uint64_t since_tick;

    /* statistics */
    uint64_t accesses;
    uint64_t tlb_hits;
    uint64_t tlb_misses;
    uint64_t pt_walks;          /* == tlb_misses; each is 1 memory access */
    uint64_t page_faults;
    uint64_t prot_faults;
    uint64_t addr_faults;
    uint64_t frame_failures;
    uint64_t evictions_seen;    /* frames reclaimed while this MMU ran     */
    uint64_t tlb_invalidations; /* TLB entries killed by frame reuse       */
} MMU;

/* Wires the MMU to memory and the process table, clears the TLB, and
 * registers itself as MainMemory's eviction notifier. */
void mmu_init(MMU *m, MainMemory *mm, Process *procs, uint16_t num_procs);

void mmu_set_cache_invalidator(MMU *m, CacheInvalidateFn fn, void *ctx);
void mmu_set_tick_interval(MMU *m, uint64_t accesses_per_tick);
void mmu_reset_stats(MMU *m);

/* THE call. Resolves `va` for process `pid`, writing the 25-bit physical
 * address through *pa_out on any MMU_OK_* result. *pa_out is untouched on a
 * fault. Safe to pass pa_out == NULL. */
MMUStatus mmu_translate(MMU *m, uint16_t pid, uint32_t va,
                        AccessType acc, uint32_t *pa_out);

/* Convenience: look up the Process record for a pid, or NULL. */
Process *mmu_find_process(MMU *m, uint16_t pid);

/* Process teardown: releases every frame the process holds (including its
 * page-table frame) and invalidates all of its TLB entries -- the event the
 * specification calls out for an identifier-based TLB. */
void mmu_process_exit(MMU *m, uint16_t pid);

void mmu_print_stats(const MMU *m, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* TRANSLATE_H */
