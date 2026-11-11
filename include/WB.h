#ifndef WB_H
#define WB_H

#include <stdint.h>
#include "MemHier.h"

/*
 * Write buffer: 4 entries, FIFO, between L1 and main memory.
 * L1 is write-through, so every store that hits L1 is queued here and
 * drains to memory later. A store to a block already queued is merged
 * into that entry (coalescing) instead of taking a new slot.
 *
 * Entry = 22 bits: valid 1 + block address 21 (the PA without its 4 offset bits)
 * Slot 0 is always the oldest entry.
 */

#define WB_ADDR_BITS        (PA_BITS - L1_OFFSET_BITS)

#define WB_BLOCK_ADDR(pa)   (((pa) >> L1_OFFSET_BITS) & MASK(WB_ADDR_BITS))
#define WB_TO_PA(ba)        (((uint32_t)(ba)) << L1_OFFSET_BITS)

typedef struct {
    uint32_t valid      : 1;
    uint32_t block_addr : WB_ADDR_BITS;
} WBEntry;

typedef struct {
    WBEntry entries[WB_ENTRIES];
    uint8_t count;                      /* how many slots are in use */
} WriteBuffer;

/* Empties the buffer. */
void wb_init(WriteBuffer *wb);

static inline int wb_is_full(const WriteBuffer *wb)  { return wb->count >= WB_ENTRIES; }

/* Queues a store (or merges it). Returns 0 only if it is a new block and the buffer is full. */
int  wb_enqueue_store(WriteBuffer *wb, uint32_t pa);

/* Writes the oldest entry to memory and removes it. Returns 0 if the buffer was empty. */
int  wb_drain_head(WriteBuffer *wb);

/* Returns the slot holding pa's block, or -1. */
int  wb_probe(const WriteBuffer *wb, uint32_t pa);

/* Returns the oldest slot holding a block from physical frame `frame`, or -1. */
int  wb_probe_frame(const WriteBuffer *wb, uint32_t frame);

#endif
