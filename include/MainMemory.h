#ifndef MAIN_MEMORY
#define MAIN_MEMORY

#include <stdint.h>
#include "MemHier.h"

/*
 * Main memory: 32 MB, pure paging, 1 KB frames -> 32768 frames.
 * Replacement: LFU with aging, GLOBAL.
 * Keep the lower and upper fram limits.
 * There is no cache coherence problem to solve here.
 */

/*protection flags*/
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

/* ============================================================
 * Page table entry -- 29 bits:
 *   present 1 + frame 15 + dirty 1 + referenced 1
 *   + aging 8 + protection 3
 * 256 entries x 29 bits = 928 B, which fits one 1 KB frame.
 * Page tables live in main memory and are never cached.
 * ============================================================ */

typedef struct {
    uint32_t present : 1;               /*  1 bit  */
    uint32_t frame : 15;                /* 15 bits */
    uint32_t dirty : 1;                 /*  1 bit  -- vs disk, not vs cache */
    uint32_t referenced : 1;            /*  1 bit  -- sampled into aging */
    uint32_t aging : 8;                 /*  8 bits -- LFU-with-aging history */
    uint32_t prot : 3;                  /*  3 bits -- PROT_* flags */
} PTE;

typedef struct {
    PTE entries[PAGES_PER_PROC];
} PageTable;

//not sure about this, so yall check karlena
typedef struct {
    uint8_t  allocated;
    uint16_t pid;                   /* 14 bits -- owning process */
    uint8_t  vpn;                   /* 8 bits -- which page sits here */
    uint8_t  is_page_table;         /* page-table frames are not evictable */
    uint32_t freq;                  /* LFU access count */
    uint8_t  aging;                 /* shifted reference-bit history */
} FrameDesc;

typedef struct {
    uint8_t   *storage;             /* malloc(MM_SIZE), freed on destroy */
    FrameDesc  frames[NUM_FRAMES];
    uint32_t   free_frames;

    /* statistics */
    uint64_t page_faults;
    uint64_t disk_reads;
    uint64_t disk_writebacks;       /* dirty victims flushed to disk */
    uint64_t writes;                /* write-through arrivals from L2 */
    uint64_t block_fetches;         /* 16 B blocks read out to L1 */
} MainMemory;


typedef struct {
    uint16_t   pid;                 /* 14 bits */
    uint8_t    active;

    PageTable *pt;                  /* resides in MM, never cached */
    uint16_t   pt_frame;            /* frame holding the page table */

    uint16_t frames_held;
    uint16_t lower_limit;           /* >= 2 pre-paged pages */
    uint16_t upper_limit;           /* <= PAGES_PER_PROC */
} Process;

/* ---- lifecycle ---- */
int  mm_init(MainMemory *mm);
void mm_destroy(MainMemory *mm);
void mm_reset_stats(MainMemory *mm);

/*frame allocation TODO*/
int  mm_alloc_frame(MainMemory *mm, uint16_t pid, uint8_t vpn,
                    int is_page_table);

/*
Global LFU-with-aging victim selection. Skips page-table frames.
Returns the victim frame number, or -1 if nothing is evictable. TODO
*/
int  mm_select_victim(MainMemory *mm, const Process *procs,
                      uint16_t num_procs);

/*
Ticks every aging counter: shift right, feed the reference
bit into the MSB, clear the reference bit. Called on the
replacement timer, not on every access.
*/
void mm_age_tick(MainMemory *mm, Process *procs, uint16_t num_procs);

/*
Paging oads the first two pages of a process before it runs, per
the pre-paging requirement. Everything else arrives on demand. TODO
 */
int  mm_prepage(MainMemory *mm, Process *proc);
int  mm_handle_fault(MainMemory *mm, Process *proc, uint8_t vpn);

/*data access TODO*/
void mm_read_block(MainMemory *mm, uint32_t pa, uint8_t *out_block);
void mm_write(MainMemory *mm, uint32_t pa,
              const uint8_t *bytes, uint32_t len);

/* Sets the PTE dirty bit for the page containing this address,
 * so the OS knows to flush it to disk before reclaiming the
 * frame. Called on every write-through arrival. */
void mm_mark_dirty(MainMemory *mm, Process *proc, uint32_t pa);

#endif