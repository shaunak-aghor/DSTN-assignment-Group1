#include "WB.h"
#include <stddef.h>

int wb_probe(const WriteBuffer *wb, uint32_t pa)
{
    uint32_t block_addr;
    int entry;

    if (wb == NULL)
        return -1;

    block_addr = WB_BLOCK_ADDR(pa);

    for (entry = 0; entry < WB_ENTRIES; entry++) {
        const WBEntry *current = &wb->entries[entry];

        if (current->valid && current->block_addr == block_addr)
            return entry;
    }

    return -1;
}
