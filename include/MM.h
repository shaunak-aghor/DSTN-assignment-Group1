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

/* What one fault did, as OR-able flags.  These are NOT mutually exclusive --
 * a single fault can read from disk, reclaim a frame and flush it -- which is
 * why this is a flag set rather than a plain enum.  The caller counts; this
 * module keeps no statistics. */
typedef enum {
    MM_OK          = 0,
    MM_DISK_READ   = 1 << 0,   /* a page was read in from disk            */
    MM_EVICTED     = 1 << 1,   /* a frame was reclaimed to make room      */
    MM_WROTE_BACK  = 1 << 2,   /* that frame was dirty and was flushed    */
    MM_OOM         = 1 << 3    /* no frame could be obtained: FAILURE     */
} MMResult;

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
} MM;

/* Frees every frame and zeroes the process table.  Returns 0. */
int  mm_init(MM *mm, Process *procs, uint16_t num_procs);

/* Releases the frame table and frees every process's page table. */
void mm_destroy(MM *mm);

/* Takes a free frame for (pid,vpn); returns the frame, or -1 if none is free. */
int  mm_alloc_frame(MM *mm, uint16_t pid, uint8_t vpn, FrameKind kind);

/* Returns the globally coldest evictable frame, or -1 if none can be taken. */
int  mm_select_victim(const MM *mm);

/* Samples every Accessed bit into its frame's aging register, clears the bit,
 * and invalidates that TLB entry so the next access is forced to walk. */
void mm_age_tick(MM *mm, TLB *tlb);

/* Gives the process a pinned page table and its two pre-paged pages.  Both
 * limits are in FRAMES and are clamped to MIN_FRAMES_PER_PROC..PAGES_PER_PROC.
 * Returns 0, or -1 if memory cannot seat it. */
int  mm_create_process(MM *mm, Process *proc, uint16_t pid,
                       uint32_t lower_limit, uint32_t upper_limit);

/* Makes vpn resident, evicting a frame if necessary.  Returns MM_* flags;
 * MM_OOM means it failed.  Any reclaimed frame is written to *out_frame and
 * MUST then be invalidated by the caller in the TLB, L1 and L2. */
MMResult mm_handle_fault(MM *mm, Process *proc, uint8_t vpn,
                         WriteBuffer *wb, uint32_t *out_frame);

/* Records a block fetch from memory. */
void mm_read_block(MM *mm, uint32_t pa);

/* Records a write arriving at memory. */
void mm_write(MM *mm, uint32_t pa);

/* Prints frame occupancy. */
void mm_dump(const MM *mm);

#endif /* MM_H */
