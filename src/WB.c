#include <string.h>
#include "WB.h"
#include "stats.h"

/*
 * The store queue: slots [0, count) are in use and slot 0 is the oldest.
 * New stores always go at the back, drains always take from the front.
 */

void wb_init(WriteBuffer *wb)
{
    memset(wb, 0, sizeof(*wb));
}

void wb_enqueue_store(WriteBuffer *wb, uint32_t pa)
{
    stats.wb_enqueued++;

    if (wb_probe(wb, pa) >= 0)
        return;                         /* block already queued: merge into it */

    wb->entries[wb->count].valid      = 1;
    wb->entries[wb->count].block_addr = WB_BLOCK_ADDR(pa);
    wb->count++;
}

int wb_drain_head(WriteBuffer *wb)
{
    if (wb->count == 0)
        return 0;

    /* The oldest block is written to memory (and to L2, if L2 holds it). */
    stats.wb_drains++;
    stats.mm_writes++;

    /* Shift everyone forward by one slot. */
    for (int i = 1; i < wb->count; i++)
        wb->entries[i - 1] = wb->entries[i];

    wb->count--;
    memset(&wb->entries[wb->count], 0, sizeof(WBEntry));
    return 1;
}

int wb_probe(const WriteBuffer *wb, uint32_t pa)
{
    uint32_t ba = WB_BLOCK_ADDR(pa);

    for (int i = 0; i < wb->count; i++)
        if (wb->entries[i].block_addr == ba)
            return i;
    return -1;
}

int wb_probe_frame(const WriteBuffer *wb, uint32_t frame)
{
    for (int i = 0; i < wb->count; i++)
        if (PA_FRAME(WB_TO_PA(wb->entries[i].block_addr)) == frame)
            return i;
    return -1;
}
