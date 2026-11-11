#ifndef WB_H
#define WB_H

#include <stdint.h>
#include "MemHier.h"

/*
 * Write buffer: 4 block-sized slots, sitting in line with L1. The CPU
 * never reads from it; only L1 writes into it.
 *
 *   Slots 0..2  store queue (FIFO). L1 is write-through + no-write-allocate,
 *               so every store is queued here and later drains to L2/memory.
 *               A store to a block already queued merges into that entry.
 *   Slot 3      swap slot. Holds L1's victim for a moment while a block
 *               moves into L1, so it can then be written down into L2.
 *
 * Entry = 24 bits: valid 1 + block address 23 (the PA without its 4 offset bits)
 */

#define WB_ADDR_BITS        (PA_BITS - L1_OFFSET_BITS)

#define WB_BLOCK_ADDR(pa)   (((pa) >> L1_OFFSET_BITS) & MASK(WB_ADDR_BITS))
#define WB_TO_PA(ba)        (((uint32_t)(ba)) << L1_OFFSET_BITS)

typedef struct {
    uint32_t valid      : 1;
    uint32_t block_addr : WB_ADDR_BITS;
} WBEntry;

typedef struct {
    WBEntry entries[WB_STORE_SLOTS];    /* store queue, slot 0 is the oldest */
    uint8_t count;                      /* store slots in use */
    WBEntry swap;                       /* the 4th slot, reserved for L1 <-> L2 swaps */
} WriteBuffer;

/* Empties the buffer. */
void wb_init(WriteBuffer *wb);

/* Is every store slot taken? */
static inline int wb_is_full(const WriteBuffer *wb) { return wb->count >= WB_STORE_SLOTS; }

/* Queues a store (or merges it into the entry for the same block).
 * The caller must make room first if the buffer is full. */
void wb_enqueue_store(WriteBuffer *wb, uint32_t pa);

/* Writes the oldest entry to memory and removes it. Returns 0 if the queue was empty. */
int  wb_drain_head(WriteBuffer *wb);

/* Returns the slot holding pa's block, or -1. */
int  wb_probe(const WriteBuffer *wb, uint32_t pa);

/* Returns the oldest slot holding a block from physical frame `frame`, or -1. */
int  wb_probe_frame(const WriteBuffer *wb, uint32_t frame);

#endif
