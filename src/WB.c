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

/*
 * The entry keeps its original queue position, so a later store to an older
 * block drains before an earlier store to a newer one.
 */
int wb_enqueue_store(WriteBuffer *wb, uint32_t pa)
{
    WBEntry *e;

    if (wb == NULL)
        return 0;

    if (wb_probe(wb, pa) >= 0)
        return 1;               /* comes into the block already queued */

    if (wb_is_full(wb))
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

int wb_probe(const WriteBuffer *wb, uint32_t pa)
{
    uint32_t ba;

    if (wb == NULL)
        return -1;

    ba = WB_BLOCK_ADDR(pa);

    for (int i = (int)wb->count - 1; i >= 0; i--)
        if (wb->entries[i].valid && wb->entries[i].block_addr == ba)
            return i;               /* WB hit */

    return -1;                      /* WB miss */
}

/* Oldest first: the caller drains from the head, so it wants the earliest
 * entry that still names this frame. */
int wb_probe_frame(const WriteBuffer *wb, uint32_t frame)
{
    int i;

    if (wb == NULL)
        return -1;

    for (i = 0; i < (int)wb->count; i++)
        if (wb->entries[i].valid &&
            (uint32_t)PA_FRAME(WB_TO_PA(wb->entries[i].block_addr)) == frame)
            return i;

    return -1;
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
}
