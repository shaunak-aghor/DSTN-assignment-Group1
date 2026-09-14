#ifndef MM_H
#define MM_H

#include <stdint.h>
#include "MemHier.h"
#include "WB.h"
#include "TLB.h"

/* ============================================================================
 *  Main memory -- 32 MB, pure paging, global LFU-with-aging replacement.
 *
 *  THE FRAME TABLE IS THE MEMORY.  There is no byte array: like L1, L2, the
 *  TLB and the write buffer, this module tracks metadata only.  Every frame is
 *  described by one FrameDesc, and that description IS the frame.
 *
 *  A frame is in exactly one of three states, which is what FrameKind encodes.
 *  Every policy question is a direct read of it:
 *      evictable?          kind == FRAME_DATA
 *      occupied?           kind != FRAME_FREE
 *      does it age?        kind == FRAME_DATA
 *
 *  PAGE TABLES occupy real frames.  A process's PageTable is malloc'd (there
 *  are no bytes to store it in), but mm_create_process still consumes a frame
 *  for it, marked FRAME_PGTBL, which is counted against free_frames, counted
 *  in frames_held, and never chosen as a victim -- a page table that could be
 *  paged out would need a page table to find it.  The accounting is identical
 *  to a real machine; only the storage of the contents differs.
 *
 *  METADATA IS SPLIT BY LIFETIME, so nothing is duplicated:
 *      PTE       survives eviction -- where the page is, what it may do
 *      FrameDesc lives only while occupied -- everything about the occupant,
 *                including all replacement state
 *  Replacement metadata only matters while a page is resident, and a resident
 *  page IS a frame, so it all lives on the frame.  That is why mm_age_tick()
 *  is one flat loop and needs no process table.
 *
 *  SIZE: sizeof(MM) is about 640 KB (frames[] + free_list[]).  Allocate it
 *  with malloc or declare it static -- never as a plain local.
 * ==========================================================================*/

/* No frame: what mm_handle_fault reports when it evicted nothing. */
#define MM_NO_FRAME  ((uint32_t)-1)

/* ---- protection flags ---- */
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

/* ---- what occupies a frame ---- */
typedef enum {
    FRAME_FREE = 0,     /* on the free list, nobody owns it            */
    FRAME_DATA,         /* holds process `pid`s page `vpn`  -- evictable */
    FRAME_PGTBL         /* holds process `pid`s page table  -- pinned    */
} FrameKind;

/* ---- page table entry: 19 bits, padded to 4 B ----
 * 256 entries x 4 B = 1024 B = exactly one frame.  Statically asserted in
 * MM.c, which is what keeps "one frame per page table" honest. */
/* Accessed and Dirty live HERE, beside `present`, exactly as a hardware PTE
 * keeps them -- and they are written by the MMU, not by the memory access:
 *
 *   miss -> the walker sets `referenced` (and `dirty` on a store) in memory
 *   hit  -> the TLB answers and MEMORY IS NEVER TOUCHED.  The page may be hit
 *           a million times and the bit does not change.  That is what makes
 *           a TLB worth having.
 *   tick -> the OS samples `referenced` into the frame's aging register,
 *           clears it, AND SHOOTS DOWN THE TLB ENTRY.  Without the shootdown
 *           the translation stays cached, no walk ever happens again, and the
 *           bit would never be set a second time -- the sample would be dead.
 */
typedef struct {
    uint32_t present    : 1;
    uint32_t frame      : FRAME_BITS;   /* 15 -- valid only when present */
    uint32_t referenced : 1;            /* the Accessed bit; set by the walker */
    uint32_t dirty      : 1;            /* set on a store; vs DISK, not cache  */
    uint32_t prot       : 3;            /* PROT_* */
} PTE;                                  /* 21 bits, padded to 4 B */

typedef struct {
    PTE entries[PAGES_PER_PROC];        /* 256 */
} PageTable;

/* ---- frame descriptor: all state of the current occupant ---- */
typedef struct {
    uint8_t  kind;          /* FrameKind                                   */
    uint16_t pid;           /* owning process                              */
    uint8_t  vpn;           /* which page  (FRAME_DATA only)               */
    uint8_t  aging;         /* 8-bit shift register: THE replacement key.
                             * Built by sampling the PTE's referenced bit;
                             * no raw access count exists, because no real
                             * MMU can supply one -- an OS gets one bit. */
} FrameDesc;

typedef struct {
    uint16_t   pid;
    PageTable *pt;              /* malloc'd; pt_frame is what memory counts.
                                 * NULL means the slot holds no live process --
                                 * this doubles as the liveness test, so there
                                 * is no separate `active` flag to fall out of
                                 * step with it. */
    uint32_t   pt_frame;
    uint32_t   frames_held;     /* ALL frames held: page table + data pages */
    uint32_t   lower_limit;     /* floor, in frames; >= MIN_FRAMES_PER_PROC */
    uint32_t   upper_limit;     /* cap,   in frames                         */
} Process;

typedef struct {
    FrameDesc frames[NUM_FRAMES];
    uint32_t  free_list[NUM_FRAMES];    /* stack of free frame numbers */
    uint32_t  free_count;

    Process  *procs;                    /* the OS process table          */
    uint16_t  num_procs;                /* global replacement needs it   */

    /* statistics */
    uint64_t page_faults;               /* demand faults only            */
    uint64_t disk_reads;
    uint64_t disk_writebacks;           /* dirty victims flushed         */
    uint64_t evictions;
    uint64_t writes;                    /* write arrivals from the cache */
    uint64_t block_fetches;             /* 16 B blocks read out to L1    */
} MM;

/* ---- lifecycle ----
 * mm_init ZEROES the process table it is given, so every slot starts with
 * pt == NULL and an uninitialised slot can never look like a live process. */
int  mm_init(MM *mm, Process *procs, uint16_t num_procs);
void mm_destroy(MM *mm);
void mm_reset_stats(MM *mm);

/* ---- frames ----
 * Takes a FREE frame only; returns -1 when memory is full.  Making a frame
 * free is mm_select_victim + the eviction inside mm_handle_fault, so that
 * policy stays in one place. */
int  mm_alloc_frame(MM *mm, uint16_t pid, uint8_t vpn, FrameKind kind);

/* Global LFU-with-aging victim, or -1 if nothing is evictable.  Skips free
 * frames, page-table frames, and any process already at its lower_limit.
 * Ranks on the aging register alone; ties break on the lowest frame number,
 * so the choice is deterministic and reproducible. */
int  mm_select_victim(const MM *mm);

/* The OS's sampling pass, run on the replacement timer -- NOT on every access,
 * which is the whole point of aging.  For every resident page:
 *     aging = (aging >> 1) | (referenced ? 0x80 : 0)
 *     referenced = 0
 *     tlb_invalidate_entry(pid, vpn)      <-- the shootdown
 * The shootdown is not optional.  Clearing the bit while the translation is
 * still in the TLB means no walk ever happens again, so the bit stays 0
 * forever and a hot page looks stone cold.  Passing tlb == NULL skips it,
 * which is only correct in tests that do not use a TLB. */
void mm_age_tick(MM *mm, TLB *tlb);

/* ---- processes ----
 * Allocates the page table (one pinned frame + the struct), sets the limits,
 * and pre-pages pages 0 and 1 per the pre-paging requirement -- into MAIN
 * MEMORY only, never the caches, so the first fetch is still a cache miss.
 * Those two faults are planned and are not counted as demand faults.
 * Requires MIN_FRAMES_PER_PROC frames to be FREE up front, so pre-paging can
 * never evict.  Returns -1 if they are not -- which means too many processes
 * for this memory, and the caller should stop: the simulation is out of
 * memory.  That is also why this needs no write buffer. */
int  mm_create_process(MM *mm, Process *proc, uint16_t pid,
                       uint32_t lower_limit, uint32_t upper_limit);

/* The demand-paging path.  0 on success, -1 if no frame could be obtained.
 *
 * At most ONE frame is evicted per call.  If one was, its number is written to
 * *out_frame (else MM_NO_FRAME), and THE CALLER MUST then invalidate it in the
 * TLB, L1 and L2 -- they are physically tagged, and a stale TLB hit skips the
 * page table entirely and hands the dead frame straight back.
 *
 * `wb` is the one structure main memory handles itself, because it must: a
 * queued store to the victim has not reached memory yet, and it has to land
 * BEFORE the write-back decision.  Both may be NULL. */
int  mm_handle_fault(MM *mm, Process *proc, uint8_t vpn,
                     WriteBuffer *wb, uint32_t *out_frame);

/* ---- data access ----
 * No bytes move and no reference state is touched -- the MMU owns that.
 * These only count traffic that genuinely reached main memory. */
void mm_read_block(MM *mm, uint32_t pa);    /* cache miss fill  */
void mm_write(MM *mm, uint32_t pa);         /* write arrival    */

void mm_dump(const MM *mm);

#endif /* MM_H */
