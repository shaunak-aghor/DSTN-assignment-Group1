#include <stdio.h>
#include "WB.h"
#include <string.h>

/* Valid entries occupy slots [0, count) contiguously; slot 0 is the oldest.
 * Every function here relies on that.  Enqueue appends at wb->count -- never
 * scan for a free slot the way the TLB and the caches do, because this is a
 * queue and position IS the ordering. */

void wb_init(WriteBuffer *wb)
{
    if (wb == NULL)
        return;

    memset(wb, 0, sizeof(*wb));
}

void wb_reset_stats(WriteBuffer *wb)
{
    if (wb == NULL)
        return;

    wb->enqueued_stores = 0;
    wb->drains          = 0;
    wb->full_stalls     = 0;
    wb->forwards        = 0;
}

int wb_enqueue_store(WriteBuffer *wb, uint32_t pa)
{
    WBEntry *e;

    if (wb == NULL || wb_is_full(wb))
        return 0;               /* caller drains the head and counts the stall */

    e = &wb->entries[wb->count];
    e->valid      = 1;
    e->block_addr = WB_BLOCK_ADDR(pa);
    wb->count++;

    return 1;
}

int wb_drain_head(WriteBuffer *wb, WBEntry *out)
{
    if (wb == NULL || wb_is_empty(wb))
        return 0;

    if (out)
        *out = wb->entries[0];

    for (int i = 1; i < wb->count; i++)
        wb->entries[i - 1] = wb->entries[i];

    wb->count--;
    memset(&wb->entries[wb->count], 0, sizeof(WBEntry));

    return 1;
}

int wb_flush_all(WriteBuffer *wb, void *ctx, void (*sink)(void *ctx, const WBEntry *e))
{
    WBEntry entry;
    int drained = 0;

    while (wb_drain_head(wb, &entry)) {
        if (sink)
            sink(ctx, &entry);
        drained++;
    }

    return drained;
}

/* Searches newest-first: a later store to the same block supersedes an
 * earlier one, so the highest index is the authoritative entry. */
int wb_probe(const WriteBuffer *wb, uint32_t pa)
{
    uint32_t ba;

    if (wb == NULL)
        return -1;

    ba = WB_BLOCK_ADDR(pa);

    for (int i = (int)wb->count - 1; i >= 0; i--) {
        if (wb->entries[i].valid && wb->entries[i].block_addr == ba)
            return i;
        /* WB hit */
    }

    return -1;
    /* WB miss */
}

void wb_dump(const WriteBuffer *wb)
{
    if (wb == NULL)
        return;

    printf("\n--- Write Buffer (%d entries, FIFO, head = idx 0) ---\n", WB_ENTRIES);
    printf("  idx  V  blockPA\n");

    for (unsigned i = 0; i < WB_ENTRIES; i++) {
        const WBEntry *e = &wb->entries[i];

        if (!e->valid) {
            printf("  %3u  0  --\n", i);
            continue;
        }

        printf("  %3u  1  0x%06X\n", i, (unsigned)WB_TO_PA(e->block_addr));
    }

    printf("  count %u/%d %s\n", (unsigned)wb->count, WB_ENTRIES,
           wb_is_full(wb) ? "[FULL]" : wb_is_empty(wb) ? "[EMPTY]" : "");
    printf("  stores %llu | drains %llu | stalls %llu | forwards %llu\n",
           (unsigned long long)wb->enqueued_stores,
           (unsigned long long)wb->drains,
           (unsigned long long)wb->full_stalls,
           (unsigned long long)wb->forwards);
}
