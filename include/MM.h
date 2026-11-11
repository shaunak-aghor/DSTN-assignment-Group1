#ifndef MM_H
#define MM_H

#include <stdint.h>
#include "MemHier.h"
#include "WB.h"
#include "TLB.h"

/*
 * Main memory: 32768 frames of 1 KB. We keep one descriptor per frame
 * (who owns it and how hot it is), not the data itself.
 *
 * Page tables: 3 levels (outer 6 | middle 8 | inner 8 bits of the VPN).
 * Every table fills exactly one frame. Tables are created only when a walk
 * needs them, and are pinned: they are never chosen for eviction.
 *
 * Page replacement: LFU with aging, global, with a per-process lower and
 * upper limit on how many data pages a process may hold.
 */

/* What a frame is being used for */
typedef enum {
    FRAME_FREE = 0,
    FRAME_DATA,                         /* a page of a process */
    FRAME_PGTBL                         /* one page table of a process */
} FrameKind;

/*
 * Page table entry = 20 bits: present 1 + frame 17 + referenced 1 + dirty 1
 * In the outer and middle tables, `frame` is where the next-level table is.
 * In the inner table, `frame` is where the page itself is.
 */
typedef struct {
    uint32_t present    : 1;
    uint32_t frame      : FRAME_BITS;
    uint32_t referenced : 1;            /* set on every access, cleared by the aging tick */
    uint32_t dirty      : 1;            /* set on a write, page must be written back on eviction */
} PTE;

/* One page table = one 1 KB frame */
typedef struct {
    PTE entries[PT_ENTRIES];
} PageTable;

/* One per physical frame */
typedef struct {
    uint8_t  kind;                      /* FrameKind */
    uint8_t  aging;                     /* LFU-with-aging register, higher = hotter */
    uint16_t pid;                       /* owner */
    uint32_t vpn;                       /* data frame: which of the owner's pages is in here */
    PTE     *pte;                       /* data frame: the PTE that maps it (reverse map) */
} FrameDesc;

typedef struct {
    uint16_t pid;                       /* PIDs are 1 .. num_procs */
    uint32_t root_frame;                /* frame holding the outer page table */
    uint32_t table_frames;              /* page-table frames in use (pinned) */
    uint32_t pages_held;                /* data pages in memory */
    uint32_t lower_limit;               /* never take pages below this */
    uint32_t upper_limit;               /* replace own pages above this */
} Process;

#define MAX_EVICTED 4

typedef struct {
    FrameDesc  frames[NUM_FRAMES];
    PageTable *table_at[NUM_FRAMES];    /* the page table stored in frame f, or NULL */
    uint32_t   free_list[NUM_FRAMES];   /* stack of free frame numbers */
    uint32_t   free_count;
    Process   *procs;
    uint16_t   num_procs;

    /* Frames taken from other pages during the current access. The caller
     * must drop them from the TLB and caches. Cleared at each translation. */
    uint32_t   evicted[MAX_EVICTED];
    int        num_evicted;
} MM;

/* Marks every frame free and clears the process table. */
void mm_init(MM *mm, Process *procs, uint16_t num_procs);

/* Creates the outer page table and pre-pages pages 0 and 1. Returns -1 if memory is full. */
int  mm_create_process(MM *mm, Process *proc, uint16_t pid,
                       uint32_t lower_limit, uint32_t upper_limit);

/* Walks proc's 3-level page table for vpn and returns its inner-level PTE.
 * Missing middle/inner tables are created on the way. NULL if memory is full. */
PTE *mm_walk(MM *mm, Process *proc, uint32_t vpn, WriteBuffer *wb);

/* Page fault: loads page vpn into a frame and fills in *pte. Returns -1 if memory is full. */
int  mm_handle_fault(MM *mm, Process *proc, uint32_t vpn, PTE *pte, WriteBuffer *wb);

/* OS aging pass: shifts every page's referenced bit into its frame's aging register. */
void mm_age_tick(MM *mm, TLB *tlb);

#endif
