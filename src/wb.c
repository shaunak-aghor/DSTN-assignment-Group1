#include <stdio.h>
#include "WB.h"
#include <string.h>

void wb_init(WriteBuffer *wb)
{
    /* allocate and initialize the write buffer */
    memset(wb, 0, sizeof(*wb));
}

void wb_reset_stats(WriteBuffer *wb)
{
    /* clear the statistics counters */
    wb->enqueued_stores    = 0;
    wb->enqueued_evictions = 0;
    wb->drains             = 0;
    wb->full_stalls        = 0;
    wb->forwards           = 0;
}

static int wb_append(WriteBuffer *wb, uint32_t block_pa, WBType type, uint32_t off, 
    const uint8_t *src, uint32_t nbytes, void *ctx, void (*sink)(void *ctx, const WBEntry *e))
{
    WBEntry *e, victim;
 
    if (!src || nbytes == 0 || off + nbytes > BLOCK_SIZE)
        return 0;                       /* would run past the end of the block */
 
    if (wb_is_full(wb)) {
        wb->full_stalls++;              /* CPU blocks here */
        if (!wb_drain_head(wb, &victim))
            return 0;
        if (sink)
            sink(ctx, &victim);
    }
 
    e = &wb->entries[wb->count];        /* ALWAYS the tail, never a free-slot scan */
    memset(e, 0, sizeof(*e));
    e->valid      = 1;
    e->type       = (uint32_t)type;
    e->block_addr = WB_BLOCK_ADDR(block_pa);
    e->offset     = off;
    memcpy(e->data + off, src, nbytes);
    wb->count++;
    return 1;
}

int wb_enqueue_store(WriteBuffer *wb, uint32_t pa, const uint8_t *bytes,
                     void *ctx, void (*sink)(void *ctx, const WBEntry *e))
{
    uint32_t off = pa & (uint32_t)MASK(L1_OFFSET_BITS);
 
    if (!wb_append(wb, pa, WB_STORE, off, bytes, STORE_WIDTH, ctx, sink))
        return 0;
 
    wb->enqueued_stores++;
    return 1;
}

int wb_enqueue_eviction(WriteBuffer *wb, uint32_t pa, const uint8_t *block,
                        void *ctx, void (*sink)(void *ctx, const WBEntry *e))
{
    if (!wb_append(wb, pa, WB_EVICTION, 0, block, BLOCK_SIZE, ctx, sink))
        return 0;
 
    wb->enqueued_evictions++;
    return 1;
}

int wb_drain_head(WriteBuffer *wb, WBEntry *out)
{
    if (wb_is_empty(wb)) {
        return 0;
        /* nothing to drain */
    }

    if(out) {
        *out = wb->entries[0];
    }

    for(int i = 1; i < wb->count; i++) {
        wb->entries[i - 1] = wb->entries[i];
    }

    wb->count--;
    wb->drains++;

    memset(&wb->entries[wb->count], 0, sizeof(WBEntry));
    return 1; /* successfully drained */
}

void wb_flush_all(WriteBuffer *wb, void *ctx, void (*sink)(void *ctx, const WBEntry *e)) {
    WBEntry entry;
    while (!wb_is_empty(wb)) {
        wb_drain_head(wb, &entry);
        if(sink) {
            sink(ctx, &entry);
        }
    }
}

int wb_probe(const WriteBuffer *wb, uint32_t pa) {
    uint32_t ba = WB_BLOCK_ADDR(pa);
    for (int i = (int)wb->count - 1; i >= 0; i--) {
        if (wb->entries[i].valid && wb->entries[i].block_addr == ba) {
            return i;
            /* WB hit */
        }
    }
    return -1;
    /* WB miss */
}

void wb_dump(const WriteBuffer *wb)
{
    unsigned i, j;
 
    printf("\n--- Write Buffer (%d entries, FIFO, head = idx 0) ---\n", WB_ENTRIES);
    printf("  idx  V  type      blockPA   off  byte@     data[0..15]\n");
 
    for (i = 0; i < WB_ENTRIES; i++) {
        const WBEntry *e = &wb->entries[i];
 
        if (!e->valid) {
            printf("  %3u  0  --\n", i);
            continue;
        }
 
        printf("  %3u  1  %-8s  0x%06X  %2u   0x%06X  ", i,
               e->type == WB_STORE ? "store" : "eviction",
               (unsigned)WB_TO_PA(e->block_addr),
               (unsigned)e->offset,
               (unsigned)(WB_TO_PA(e->block_addr) | e->offset));
 
        for (j = 0; j < BLOCK_SIZE; j++)
            printf("%02X", e->data[j]);
        printf("\n");
    }
 
    printf("  count %u/%d %s\n", (unsigned)wb->count, WB_ENTRIES,
           wb_is_full(wb) ? "[FULL]" : wb_is_empty(wb) ? "[EMPTY]" : "");
    printf("  stores %llu | evictions %llu | drains %llu | stalls %llu | forwards %llu\n",
           (unsigned long long)wb->enqueued_stores,
           (unsigned long long)wb->enqueued_evictions,
           (unsigned long long)wb->drains,
           (unsigned long long)wb->full_stalls,
           (unsigned long long)wb->forwards);
}