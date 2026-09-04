#include "L1.h"
#include <stddef.h>

int l1_probe(L1Cache *l1, uint32_t pa)
{
    uint32_t index;
    uint32_t tag;
    int way;

    if (l1 == NULL)
        return -1;

    index = L1_INDEX(pa);
    tag = L1_TAG(pa);

    for (way = 0; way < L1_WAYS; way++) {
        L1Line *line = &l1->sets[index].ways[way];

        if (line->valid && line->tag == tag)
            return way;
    }

    return -1;
}
