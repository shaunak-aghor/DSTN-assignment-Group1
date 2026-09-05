#ifndef WB_H
#define WB_H

#include <stdint.h>
#include "MemHier.h"

/*
 * Write buffer: 4 entries, FIFO, sits between L1 and main memory.
 * L1 is write-through and has no dirty bit, so every store is queued here
 * and drains to main memory.  Evictions do NOT pass through the buffer --
 * the L1<->L2 exchange is handled entirely by l2_promote.
 * The hierarchy tracks tags and addresses only, so an entry carries no data.
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

/* Appends at the tail.  Returns 1 if queued, 0 if the buffer was full and the
 * store was NOT queued -- the caller must drain the head and retry, and it is
 * the caller that counts the stall. DONE */
int wb_enqueue_store(WriteBuffer *wb, uint32_t pa);

/*Removes head entry into *out and shifts the rest down. DONE*/
int wb_drain_head(WriteBuffer *wb, WBEntry *out);

/*Drains every entry through `sink`, in order. Returns how many. DONE*/
int wb_flush_all(WriteBuffer *wb, void *ctx,
                 void (*sink)(void *ctx, const WBEntry *e));

/*Compare for a hit, else return -1, try parallel. DONE*/
int wb_probe(const WriteBuffer *wb, uint32_t pa);

/* ---- debug ---- */
void wb_dump(const WriteBuffer *wb);

#endif /* WB_H */
