#ifndef WB_H
#define WB_H

#include <stdint.h>
#include "MemHier.h"


#define WB_ADDR_BITS    21

/* Convert between a physical address and a stored block address */
#define WB_BLOCK_ADDR(pa)   (((pa) >> L1_OFFSET_BITS) & MASK(WB_ADDR_BITS))
#define WB_TO_PA(ba)        (((uint32_t)(ba)) << L1_OFFSET_BITS)

typedef enum {
    WB_STORE    = 0,
    WB_EVICTION = 1
} WBType;

typedef struct {
    uint32_t valid      : 1;    /*  1 bit  */
    uint32_t type       : 1;    /*  1 bit  -- WBType */
    uint32_t block_addr : 21;   /* 21 bits -- PA with offset dropped */
} WBEntry;

typedef struct {
    WBEntry entries[WB_ENTRIES];
    uint8_t count;         
    /* statistics */
    uint64_t enqueued_stores;
    uint64_t enqueued_evictions;
    uint64_t drains;
    uint64_t full_stalls;       /* forced drains caused by a full buffer */
    uint64_t forwards;          /* reads satisfied by a buffer hit */
} WriteBuffer;

/*lifecycle TODO*/
void wb_init(WriteBuffer *wb);
void wb_reset_stats(WriteBuffer *wb);

/*capacity*/
static inline int wb_is_full(const WriteBuffer *wb) {
    return wb->count >= WB_ENTRIES;
}

static inline int wb_is_empty(const WriteBuffer *wb) {
    return wb->count == 0;
}

/*TODO*/
void wb_enqueue_store(WriteBuffer *wb, uint32_t pa);
void wb_enqueue_eviction(WriteBuffer *wb, uint32_t pa);

/*Removes head entry into *out and shifts the rest down. TODO*/
int wb_drain_head(WriteBuffer *wb, WBEntry *out);

/*Drains every entry through `sink`, in order. TODO*/
void wb_flush_all(WriteBuffer *wb, void *ctx,
                  void (*sink)(void *ctx, const WBEntry *e));

/*Compare for a hit, else return -1, try parallel. TODO*/
int wb_probe(const WriteBuffer *wb, uint32_t pa);

/* ---- debug ---- */
void wb_dump(const WriteBuffer *wb);

#endif /* WB_H */