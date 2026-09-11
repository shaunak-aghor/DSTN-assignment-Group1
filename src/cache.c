#include "cache.h"
#include <pthread.h>
#include <semaphore.h>
#include <stddef.h>
#include <stdint.h>

static void *search_l1_worker(void *argument)
{
    void **arguments = argument;
    L1Cache *l1 = arguments[0];
    WriteBuffer *write_buffer = arguments[1];
    uint32_t *pa = arguments[2];
    CacheSearchResult *result = arguments[3]; 
    pthread_t *l2_thread = arguments[4];
    sem_t *start_signal = arguments[5]; 
    
    /* Unblock the L2 thread to begin concurrent Look Aside search */
    sem_post(start_signal);

    if (l1_probe(l1, *pa) >= 0) {
        *result = CACHE_HIT_L1; 
        pthread_cancel(*l2_thread); 
    } 
    else if (write_buffer != NULL && wb_probe(write_buffer, *pa) >= 0) {
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

    /* Allow asynchronous cancellation during probe execution */
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    /* Block until L1 thread is initialized and signals start */
    sem_wait(start_signal);

    if (l2 != NULL && l2_probe(l2, *pa) >= 0) {
        *result = CACHE_HIT_L2; 
    } else {
        *result = CACHE_MISS; 
    }
    
    return NULL; 
}

CacheSearchResult cache_read(L1Cache *l1, L2Cache *l2, WriteBuffer *write_buffer, uint32_t pa)
{
    pthread_t l1_thread;
    pthread_t l2_thread;
    
    uint32_t search_pa = pa;
    CacheSearchResult l1_result = CACHE_MISS;
    CacheSearchResult l2_result = CACHE_MISS;

    sem_t start_signal;
    sem_init(&start_signal, 0, 0);

    void *l2_arguments[4] = { l2, &search_pa, &l2_result, &start_signal };
    pthread_create(&l2_thread, NULL, search_l2_worker, l2_arguments);

    void *l1_arguments[6] = { l1, write_buffer, &search_pa, &l1_result, &l2_thread, &start_signal };
    pthread_create(&l1_thread, NULL, search_l1_worker, l1_arguments);

    pthread_join(l1_thread, NULL);
    pthread_join(l2_thread, NULL);

    sem_destroy(&start_signal);

    if (l1_result == CACHE_HIT_L1) {
        l1_age(l1, L1_INDEX(pa), l1_probe(l1, pa));  /* a hit is a use: promote */
        l1->read_hits++;
        return CACHE_HIT_L1;
    }

    l1->read_misses++;

    if (l1_result == CACHE_HIT_WB) {
        write_buffer->forwards++;
        return CACHE_HIT_WB;
    }

    if (l2_result == CACHE_HIT_L2) {
        l2->hits++;
        l2_promote(l2, l1, search_pa); 
        return CACHE_HIT_L2;
    }

    l2->misses++;
    /* Total Cache Miss - Handled by CPU abstraction */
    return CACHE_MISS; 
}

/* Store path.  Same look-aside search as cache_read, then:
 *   L1 hit   line stays valid and becomes MRU, store is queued in the write
 *            buffer, CPU does not stall.
 *   WB hit   an older store to this block is still pending.  It must not be
 *            overtaken, so this store joins the queue behind it instead of
 *            racing ahead down the unbuffered path.
 *   L1 miss  no-write-allocate: nothing filled, promoted or demoted.  The store
 *            goes straight to memory and STALLS, since L2 has no buffer.
 * Nothing here changes what is resident.  Main memory is the caller's job:
 * CACHE_HIT_L1 means the store was buffered, anything else means the caller
 * must mm_write() pa itself.  If *drained_out comes back valid the buffer was
 * full and the caller must send that entry to memory too -- passing NULL
 * DISCARDS a displaced store. */
CacheSearchResult cache_write(L1Cache *l1, L2Cache *l2, WriteBuffer *write_buffer,
                              uint32_t pa, WBEntry *drained_out)
{
    pthread_t l1_thread;
    pthread_t l2_thread;

    uint32_t search_pa = pa;
    CacheSearchResult l1_result = CACHE_MISS;
    CacheSearchResult l2_result = CACHE_MISS;

    sem_t start_signal;
    sem_init(&start_signal, 0, 0);

    if (drained_out != NULL)
        drained_out->valid = 0;     /* nothing displaced unless we say so */

    void *l2_arguments[4] = { l2, &search_pa, &l2_result, &start_signal };
    pthread_create(&l2_thread, NULL, search_l2_worker, l2_arguments);

    void *l1_arguments[6] = { l1, write_buffer, &search_pa, &l1_result, &l2_thread, &start_signal };
    pthread_create(&l1_thread, NULL, search_l1_worker, l1_arguments);

    pthread_join(l1_thread, NULL);
    pthread_join(l2_thread, NULL);

    sem_destroy(&start_signal);

    if (l1_result == CACHE_HIT_L1) {
        l1_write_hit(l1, pa, l1_probe(l1, pa));   /* no data: ages to MRU */
        l1->write_hits++;

        if (wb_is_full(write_buffer)) {
            /* One drain always suffices -- it frees exactly one slot. */
            write_buffer->full_stalls++;
            if (wb_drain_head(write_buffer, drained_out))
                write_buffer->drains++;
        }
        wb_enqueue_store(write_buffer, pa);
        write_buffer->enqueued_stores++;

        return CACHE_HIT_L1;        /* exclusive: L2 cannot hold it */
    }

    l1->write_misses++;             /* no-write-allocate: L1 left as it was */

    if (l1_result == CACHE_HIT_WB) {
        if (wb_is_full(write_buffer)) {
            /* One drain always suffices -- it frees exactly one slot. */
            write_buffer->full_stalls++;
            if (wb_drain_head(write_buffer, drained_out))
                write_buffer->drains++;
        }
        wb_enqueue_store(write_buffer, pa);
        write_buffer->enqueued_stores++;

        return CACHE_HIT_WB;
    }

    if (l2_result == CACHE_HIT_L2) {
        /* Write-through: the line is already correct and stays valid.
         * FIFO order is fixed at insertion, so it is NOT re-aged. */
        l2->hits++;
        l2->updated_writes++;
        return CACHE_HIT_L2;
    }

    l2->misses++;
    l2->passthrough_writes++;
    return CACHE_MISS;
}