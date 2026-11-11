#ifndef STATS_H
#define STATS_H

#include <stdint.h>

/*
 * Every counter of the run lives in this one struct.
 * Each module increments the counters for its own level,
 * and main.c prints them all at the end.
 */
typedef struct {
    /* whole run */
    uint64_t accesses, reads, writes, switches, ticks;

    /* TLB and page table */
    uint64_t tlb_hits, tlb_misses, tlb_evictions;
    uint64_t pt_walks;                  /* page-table walks (one per TLB miss, 3 memory reads each) */
    uint64_t pt_tables_created;         /* page-table frames allocated */

    /* L1 */
    uint64_t l1_read_hits, l1_read_misses;
    uint64_t l1_write_hits, l1_write_misses;
    uint64_t l1_evictions;

    /* L2 */
    uint64_t l2_read_hits, l2_read_misses;
    uint64_t l2_write_hits, l2_write_misses;
    uint64_t l2_evictions;

    /* write buffer */
    uint64_t wb_enqueued;               /* stores accepted (new entry or merged) */
    uint64_t wb_drains;                 /* entries written out to memory */
    uint64_t wb_stalls;                 /* store had to wait: WB full */
    uint64_t wb_read_stalls;            /* read had to wait for a queued store to drain */
    uint64_t wb_swaps;                  /* L1 victim passed through the swap slot into L2 */

    /* main memory */
    uint64_t page_faults;               /* each one is a disk read */
    uint64_t disk_writebacks;           /* dirty pages written back on eviction */
    uint64_t mm_evictions;              /* frames taken from a page */
    uint64_t mm_block_fetches;          /* blocks read on a full cache miss */
    uint64_t mm_writes;                 /* block writes arriving at memory */
} Stats;

extern Stats stats;                     /* defined in main.c */

#endif
