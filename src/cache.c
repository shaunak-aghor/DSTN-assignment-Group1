#include <pthread.h>
#include <semaphore.h>
#include "cache.h"
#include "stats.h"

/* ---------------------------------------------------------------------------
 * Look-aside search: L1 (plus the write buffer) and L2 are probed in parallel
 * on two threads. If L1 hits, it cancels the L2 search.
 * ------------------------------------------------------------------------- */

static void *search_l1_worker(void *argument)
{
    void **arguments = argument;
    L1Cache *l1 = arguments[0];
    WriteBuffer *write_buffer = arguments[1];
    uint32_t *pa = arguments[2];
    CacheSearchResult *result = arguments[3];
    pthread_t *l2_thread = arguments[4];
    sem_t *start_signal = arguments[5];

    /* Let the L2 thread start its search alongside us. */
    sem_post(start_signal);

    if (l1_probe(l1, *pa) >= 0) {
        *result = CACHE_HIT_L1;
        pthread_cancel(*l2_thread);
    } else if (wb_probe(write_buffer, *pa) >= 0) {
        *result = CACHE_HIT_WB;
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

/* Runs both searches and returns where pa was found (L1/WB win over L2). */
static CacheSearchResult search(L1Cache *l1, L2Cache *l2, WriteBuffer *wb, uint32_t pa)
{
    pthread_t l1_thread, l2_thread;
    CacheSearchResult l1_result = CACHE_MISS;
    CacheSearchResult l2_result = CACHE_MISS;
    sem_t start_signal;

    sem_init(&start_signal, 0, 0);

    void *l2_arguments[4] = { l2, &pa, &l2_result, &start_signal };
    pthread_create(&l2_thread, NULL, search_l2_worker, l2_arguments);

    void *l1_arguments[6] = { l1, wb, &pa, &l1_result, &l2_thread, &start_signal };
    pthread_create(&l1_thread, NULL, search_l1_worker, l1_arguments);

    pthread_join(l1_thread, NULL);
    pthread_join(l2_thread, NULL);
    sem_destroy(&start_signal);

    if (l1_result != CACHE_MISS)
        return l1_result;
    return l2_result;
}

/* ---------------------------------------------------------------------------
 * Moving blocks between levels
 * ------------------------------------------------------------------------- */

/*
 * Brings pa's block into L1 (from L2 or from memory).
 * L1 and L2 are exclusive, so the line L1 throws out moves down into L2.
 */
static void move_into_l1(L1Cache *l1, L2Cache *l2, uint32_t pa)
{
    uint32_t index = L1_INDEX(pa);
    int      way   = l1_select_victim(l1, index);
    uint32_t victim_pa;

    if (l1_evict(l1, index, way, &victim_pa)) {
        stats.l1_evictions++;
        l1_install(l1, pa);
        l2_allocate(l2, victim_pa);     /* demote L1's victim into L2 */
    } else {
        l1_install(l1, pa);             /* the way was free, nothing to demote */
    }
}

/* ---------------------------------------------------------------------------
 * Read and write
 * ------------------------------------------------------------------------- */

void cache_read(L1Cache *l1, L2Cache *l2, WriteBuffer *wb, uint32_t pa)
{
    switch (search(l1, l2, wb, pa)) {
    case CACHE_HIT_L1:
        stats.l1_read_hits++;
        l1_age(l1, L1_INDEX(pa), l1_probe(l1, pa));
        break;

    case CACHE_HIT_WB:                  /* a store to this block is still queued */
        stats.l1_read_misses++;
        stats.wb_forwards++;
        break;

    case CACHE_HIT_L2:                  /* move the block up from L2 to L1 */
        stats.l1_read_misses++;
        stats.l2_hits++;
        stats.l2_promotions++;
        l2_invalidate(l2, pa);
        move_into_l1(l1, l2, pa);
        break;

    case CACHE_MISS:                    /* fetch the block from main memory */
        stats.l1_read_misses++;
        stats.l2_misses++;
        stats.mm_block_fetches++;
        move_into_l1(l1, l2, pa);
        break;
    }
}

/* If the buffer is full the CPU stalls while the oldest entry drains, then the store is queued. */
static void queue_store(WriteBuffer *wb, uint32_t pa)
{
    if (wb_is_full(wb)) {
        stats.wb_stalls++;
        wb_drain_head(wb);
    }
    wb_enqueue_store(wb, pa);
    stats.wb_enqueued++;
}

void cache_write(L1Cache *l1, L2Cache *l2, WriteBuffer *wb, uint32_t pa)
{
    switch (search(l1, l2, wb, pa)) {
    case CACHE_HIT_L1:                  /* update L1, the store reaches memory via the WB */
        stats.l1_write_hits++;
        l1_age(l1, L1_INDEX(pa), l1_probe(l1, pa));
        queue_store(wb, pa);
        break;

    case CACHE_HIT_WB:                  /* join the store already queued for this block */
        stats.l1_write_misses++;
        queue_store(wb, pa);
        break;

    case CACHE_HIT_L2:                  /* write-through: L2 updated, memory written, CPU stalls */
        stats.l1_write_misses++;
        stats.l2_hits++;
        stats.l2_updated_writes++;
        stats.mm_writes++;
        break;

    case CACHE_MISS:                    /* no-write-allocate: straight to memory, CPU stalls */
        stats.l1_write_misses++;
        stats.l2_misses++;
        stats.l2_passthrough_writes++;
        stats.mm_writes++;
        break;
    }
}
