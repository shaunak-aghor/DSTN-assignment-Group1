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
    uint64_t pt_walks;                  /* page-table walks (one per TLB miss) */

    /* L1 */
    uint64_t l1_read_hits, l1_read_misses;
    uint64_t l1_write_hits, l1_write_misses;
    uint64_t l1_evictions;

    /* L2 */
    uint64_t l2_hits, l2_misses, l2_promotions, l2_evictions;
    uint64_t l2_updated_writes, l2_passthrough_writes;

    /* write buffer */
    uint64_t wb_enqueued, wb_drains, wb_stalls, wb_forwards;

    /* main memory */
    uint64_t page_faults;               /* each one is a disk read */
    uint64_t disk_writebacks;           /* dirty pages written back on eviction */
    uint64_t mm_evictions;              /* frames taken from a page */
    uint64_t mm_block_fetches;          /* blocks read on a full cache miss */
    uint64_t mm_writes;                 /* block writes arriving at memory */
} Stats;

extern Stats stats;                     /* defined in main.c */

#endif
