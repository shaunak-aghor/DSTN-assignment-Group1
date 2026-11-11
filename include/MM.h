#ifndef MM_H
#define MM_H

#include <stdint.h>
#include "MemHier.h"
#include "WB.h"
#include "TLB.h"

/*
 * Main memory: 32768 frames of 1 KB. We keep one descriptor per frame
 * (who owns it and how hot it is), not the data itself.
 * Page replacement: LFU with aging, global, with a per-process lower and
 * upper limit on how many frames a process may hold.
 */

#define MM_NO_FRAME  ((uint32_t)-1)

/* What a frame is being used for */
typedef enum {
    FRAME_FREE = 0,
    FRAME_DATA,                         /* a page of a process */
    FRAME_PGTBL                         /* a process's page table */
} FrameKind;

/* Page table entry = 18 bits: present 1 + frame 15 + referenced 1 + dirty 1 */
typedef struct {
    uint32_t present    : 1;
    uint32_t frame      : FRAME_BITS;
    uint32_t referenced : 1;            /* set on every access, cleared by the aging tick */
    uint32_t dirty      : 1;            /* set on a write, page must be written back on eviction */
} PTE;

typedef struct {
    PTE entries[PAGES_PER_PROC];
} PageTable;

/* One per physical frame */
typedef struct {
    uint8_t  kind;                      /* FrameKind */
    uint16_t pid;                       /* owner */
    uint8_t  vpn;                       /* which of the owner's pages is in here */
    uint8_t  aging;                     /* LFU-with-aging register, higher = hotter */
} FrameDesc;

typedef struct {
    uint16_t   pid;                     /* PIDs are 1 .. num_procs */
    PageTable *pt;
    uint32_t   pt_frame;                /* frame holding the page table */
    uint32_t   frames_held;
    uint32_t   lower_limit;             /* never take frames below this */
    uint32_t   upper_limit;             /* replace own pages above this */
} Process;

typedef struct {
    FrameDesc frames[NUM_FRAMES];
    uint32_t  free_list[NUM_FRAMES];    /* stack of free frame numbers */
    uint32_t  free_count;
    Process  *procs;
    uint16_t  num_procs;
} MM;

/* Marks every frame free and clears the process table. */
void mm_init(MM *mm, Process *procs, uint16_t num_procs);

/* Gives a process its page table frame and pre-pages pages 0 and 1. Returns -1 if memory is full. */
int  mm_create_process(MM *mm, Process *proc, uint16_t pid,
                       uint32_t lower_limit, uint32_t upper_limit);

/* Brings page vpn of proc into a frame. If a frame had to be taken from
 * another page, its number is written to *out_frame (else MM_NO_FRAME).
 * Returns 0, or -1 if no frame could be found. */
int  mm_handle_fault(MM *mm, Process *proc, uint8_t vpn,
                     WriteBuffer *wb, uint32_t *out_frame);

/* OS aging pass: shifts every page's referenced bit into its frame's aging register. */
void mm_age_tick(MM *mm, TLB *tlb);

#endif
