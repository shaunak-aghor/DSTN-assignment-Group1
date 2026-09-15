#ifndef WB_H
#define WB_H

#include <stdint.h>
#include "MemHier.h"

/*
 * Write buffer: 4 entries, FIFO, sits between L1 and main memory.
 * L1 is write-through and has no dirty bit, so every store is queued here
 * and drains to main memory. Evictions do NOT pass through the buffer, they are handled by
 * and go straight to L2.
 *
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
} WriteBuffer;

/* Empties the buffer. */
void wb_init(WriteBuffer *wb);

/* Is the buffer full / empty? */
static inline int wb_is_full(const WriteBuffer *wb)  { return wb->count >= WB_ENTRIES; }
static inline int wb_is_empty(const WriteBuffer *wb) { return wb->count == 0; }

/* Queues a store, coalescing into the block's entry if it is already queued.
 * Returns 1 if absorbed, 0 if the block was new and the buffer was full. */
int wb_enqueue_store(WriteBuffer *wb, uint32_t pa);

/* Removes the oldest entry into *out; returns 1, or 0 if the buffer is empty. */
int wb_drain_head(WriteBuffer *wb, WBEntry *out);

/* Drains every entry through `sink`, oldest first; returns how many. */
int wb_flush_all(WriteBuffer *wb, void *ctx,
                 void (*sink)(void *ctx, const WBEntry *e));

/* Returns the entry holding pa's block, or WAY_NONE. */
int wb_probe(const WriteBuffer *wb, uint32_t pa);

/* Returns the oldest entry in physical frame `frame`, or WAY_NONE. */
int wb_probe_frame(const WriteBuffer *wb, uint32_t frame);

/* Prints the queued entries. */
void wb_dump(const WriteBuffer *wb);

#endif /* WB_H */
