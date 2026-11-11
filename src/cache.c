#include <pthread.h>
#include <semaphore.h>
#include "cache.h"
#include "stats.h"

/* ---------------------------------------------------------------------------
 * Look-aside search: L1 and L2 are probed in parallel on two threads.
 * If L1 hits, it cancels the L2 search.
 * ------------------------------------------------------------------------- */

static void *search_l1_worker(void *argument)
{
    void **arguments = argument;
    L1Cache *l1 = arguments[0];
    uint32_t *pa = arguments[1];
    CacheSearchResult *result = arguments[2];
    pthread_t *l2_thread = arguments[3];
    sem_t *start_signal = arguments[4];

    /* Let the L2 thread start its search alongside us. */
    sem_post(start_signal);

    if (l1_probe(l1, *pa) >= 0) {
        *result = CACHE_HIT_L1;
        pthread_cancel(*l2_thread);
    } else {
        *result = CACHE_MISS;
    }
    return NULL;
}

static void *search_l2_worker(void *argument)
{
    void **arguments = argument;
    L2Cache *l2 = arguments[0];
    uint32_t *pa = arguments[1];
    CacheSearchResult *result = arguments[2];
    sem_t *start_signal = arguments[3];

    /* The L1 thread may cancel us at any point if it finds a hit. */
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    /* Wait for the L1 thread to start. */
    sem_wait(start_signal);

    *result = (l2_probe(l2, *pa) >= 0) ? CACHE_HIT_L2 : CACHE_MISS;
    return NULL;
}

/* Runs both searches and returns where pa was found (an L1 hit wins over L2). */
static CacheSearchResult search(L1Cache *l1, L2Cache *l2, uint32_t pa)
{
    pthread_t l1_thread, l2_thread;
    CacheSearchResult l1_result = CACHE_MISS;
    CacheSearchResult l2_result = CACHE_MISS;
    sem_t start_signal;

    sem_init(&start_signal, 0, 0);

    void *l2_arguments[4] = { l2, &pa, &l2_result, &start_signal };
    pthread_create(&l2_thread, NULL, search_l2_worker, l2_arguments);

    void *l1_arguments[5] = { l1, &pa, &l1_result, &l2_thread, &start_signal };
    pthread_create(&l1_thread, NULL, search_l1_worker, l1_arguments);

    pthread_join(l1_thread, NULL);
    pthread_join(l2_thread, NULL);
    sem_destroy(&start_signal);

    if (l1_result == CACHE_HIT_L1)
        return CACHE_HIT_L1;
    return l2_result;
}

/* ---------------------------------------------------------------------------
 * Moving blocks between L1 and L2 (exclusive caches)
 * ------------------------------------------------------------------------- */

/*
 * Brings pa's block into L1, from L2 (from_l2 = 1) or from memory.
 * L1 and L2 are exclusive, so L1's victim must go down into L2. The WB's
 * swap slot works as the temporary line, like swapping two variables:
 *   1. L1's victim  -> swap slot      (frees its L1 way)
 *   2. the block    -> L1             (if it came from L2, its L2 way is freed)
 *   3. swap slot    -> L2
 * Note: the victim shares pa's L1 set but not always pa's L2 set (L1 indexes
 * on PA bits 4-9, L2 on bits 4-11), so in step 3 it may land in a different
 * L2 set than the one the block just left.
 */
static void move_into_l1(L1Cache *l1, L2Cache *l2, WriteBuffer *wb, uint32_t pa, int from_l2)
{
    uint32_t index = L1_INDEX(pa);
    int      way   = l1_select_victim(l1, index);
    uint32_t victim_pa;

    /* 1. L1's victim moves into the swap slot. */
    if (l1_evict(l1, index, way, &victim_pa)) {
        stats.l1_evictions++;
        wb->swap.valid      = 1;
        wb->swap.block_addr = WB_BLOCK_ADDR(victim_pa);
    }

    /* 2. The requested block moves into L1. */
    if (from_l2)
        l2_invalidate(l2, pa);
    l1_install(l1, pa);

    /* 3. The swap slot is written into L2 and freed. */
    if (wb->swap.valid) {
        stats.wb_swaps++;
        l2_allocate(l2, WB_TO_PA(wb->swap.block_addr));
        wb->swap.valid = 0;
    }
}

/* ---------------------------------------------------------------------------
 * Read and write
 * ------------------------------------------------------------------------- */

void cache_read(L1Cache *l1, L2Cache *l2, WriteBuffer *wb, uint32_t pa)
{
    CacheSearchResult where = search(l1, l2, pa);

    if (where == CACHE_HIT_L1) {
        stats.l1_read_hits++;
        l1_age(l1, L1_INDEX(pa), l1_probe(l1, pa));
        return;
    }
    stats.l1_read_misses++;

    /* The CPU cannot read the WB. If a store to this block is still queued,
     * L2 and memory are out of date, so wait for the WB to drain past it. */
    if (wb_probe(wb, pa) >= 0) {
        stats.wb_read_stalls++;
        while (wb_probe(wb, pa) >= 0)
            wb_drain_head(wb);
    }

    if (where == CACHE_HIT_L2) {
        stats.l2_read_hits++;
    } else {
        stats.l2_read_misses++;
        stats.mm_block_fetches++;       /* the block is read from main memory */
    }
    move_into_l1(l1, l2, wb, pa, where == CACHE_HIT_L2);
}

/* If the store needs a new WB slot and none is free, the CPU stalls
 * until the oldest entry has drained. */
static void wait_for_wb_slot(WriteBuffer *wb, uint32_t pa)
{
    if (wb_is_full(wb) && wb_probe(wb, pa) < 0) {
        stats.wb_stalls++;
        wb_drain_head(wb);
    }
}

void cache_write(L1Cache *l1, L2Cache *l2, WriteBuffer *wb, uint32_t pa)
{
    CacheSearchResult where = search(l1, l2, pa);

    /* Every store goes through the WB, so first make sure it has room.
     * L1 is only written after this, so L1 never runs ahead of the WB. */
    wait_for_wb_slot(wb, pa);

    switch (where) {
    case CACHE_HIT_L1:                  /* update L1, the WB carries the store down */
        stats.l1_write_hits++;
        l1_age(l1, L1_INDEX(pa), l1_probe(l1, pa));
        break;

    case CACHE_HIT_L2:                  /* no-write-allocate: L2 is updated when the store drains */
        stats.l1_write_misses++;
        stats.l2_write_hits++;
        break;

    case CACHE_MISS:                    /* no-write-allocate: only memory is updated */
        stats.l1_write_misses++;
        stats.l2_write_misses++;
        break;
    }
    wb_enqueue_store(wb, pa);
}
