#ifndef WB_H
#define WB_H

#include <stdint.h>
#include "MemHier.h"

/*
 * Write buffer: 4 entries, FIFO, sits between L1 and main memory.
 * L1 is write-through and has no dirty bit, so every store is queued here
 * and drains to main memory.  Evictions do NOT pass through the buffer --
 * the L1<->L2 exchange is handled entirely by l2_promote.
 * WRITE COALESCING.  The unit of buffering is a 16 B BLOCK, per Q3's "write
 * buffer with 4 blocks as buffer" -- not one slot per store.  A store to a
 * block that is already queued merges into that entry and consumes no slot,
 * so the buffer holds up to WB_ENTRIES DISTINCT blocks and each block appears
 * at most once.  Only a store to a block that is not queued can stall.
 *
 * Consequences, accepted deliberately:
 *   - The buffer records WHICH BLOCKS have pending writes, not which bytes.
 *     A load of any byte of a queued block forwards from it.
 *   - Coalescing relaxes store order: a later store to an already-queued
 *     block drains at that block's original queue position.  This is a weak
 *     memory model, which is what real write-combining buffers provide.
 * Entry layout -- 22 bits: valid 1 + block_addr 21
 */

#define WB_ADDR_BITS    21

/* Convert between a physical address and a stored block address.
 * WB_TO_PA returns the BLOCK BASE, not the original byte address. */
#define WB_BLOCK_ADDR(pa)   (((pa) >> L1_OFFSET_BITS) & MASK(WB_ADDR_BITS))
#define WB_TO_PA(ba)        (((uint32_t)(ba)) << L1_OFFSET_BITS)

typedef struct {
    uint32_t valid      : 1;    /*  1 bit  */
    uint32_t block_addr : 21;   /* 21 bits -- PA with the offset dropped */
} WBEntry;                      /* 22 bits */

typedef struct {
    WBEntry entries[WB_ENTRIES];
    uint8_t count;
    /* statistics -- incremented by main, never by this module */
    uint64_t enqueued_stores;
    uint64_t drains;
    uint64_t full_stalls;       /* stores that arrived at a full buffer */
    uint64_t forwards;          /* reads satisfied by a buffer hit */
} WriteBuffer;

/*lifecycle DONE*/
void wb_init(WriteBuffer *wb);
void wb_reset_stats(WriteBuffer *wb);

/*capacity*/
static inline int wb_is_full(const WriteBuffer *wb) {
    return wb->count >= WB_ENTRIES;
}

static inline int wb_is_empty(const WriteBuffer *wb) {
    return wb->count == 0;
}

/* Queues a store.  If pa's block is already buffered the store COALESCES into
 * that entry -- no new slot, no stall, and the block keeps its queue position.
 * Otherwise the block is appended at the tail.  Returns 1 if the store was
 * absorbed (either way), 0 only if the block was new and all WB_ENTRIES slots
 * hold other blocks -- then the caller drains the head, counts the stall and
 * retries. DONE */
int wb_enqueue_store(WriteBuffer *wb, uint32_t pa);

/*Removes head entry into *out and shifts the rest down. DONE*/
int wb_drain_head(WriteBuffer *wb, WBEntry *out);

/*Drains every entry through `sink`, in order. Returns how many. DONE*/
int wb_flush_all(WriteBuffer *wb, void *ctx,
                 void (*sink)(void *ctx, const WBEntry *e));

/* Index of the entry holding pa's block, or -1.  Block granularity: a hit
 * means the block has pending writes, not that these exact bytes do. DONE */
int wb_probe(const WriteBuffer *wb, uint32_t pa);

/* Index of the oldest queued block living in physical frame `frame`, or -1.
 * Main memory uses this before reclaiming a frame: a pending store to it MUST
 * reach memory first, so the buffer is drained from the HEAD until this
 * returns -1.  Draining head-first is what keeps the FIFO order intact --
 * entries cannot be plucked from the middle. DONE */
int wb_probe_frame(const WriteBuffer *wb, uint32_t frame);

/* ---- debug ---- */
void wb_dump(const WriteBuffer *wb);

#endif /* WB_H */
