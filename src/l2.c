#include "L2.h"
#include <stddef.h>

int l2_probe(L2Cache *l2, uint32_t pa)
{
    uint32_t index;
    uint32_t tag;
    int way;

    if (l2 == NULL)
        return -1;

    index = L2_INDEX(pa);
    tag = L2_TAG(pa);

    for (way = 0; way < L2_WAYS; way++) {
        const L2Line *line = &l2->sets[index].ways[way];

        if (line->valid && line->tag == tag)
            return way;
    }

    return -1;
}
