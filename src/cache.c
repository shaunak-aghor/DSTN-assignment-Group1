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

CacheSearchResult cache_search(L1Cache *l1, L2Cache *l2, WriteBuffer *write_buffer, uint32_t pa)
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

    if (l1_result != CACHE_MISS) {
        // return the l1_result. The LRU counter and stats are updated in probe. stats also updated in probe
        return l1_result; 
    }

    if (l2_result == CACHE_HIT_L2) {

        l2_promote(l2, l1, search_pa); 
        return CACHE_HIT_L2;
    }
        
    /* Total Cache Miss - Handled by CPU abstraction */
    return CACHE_MISS; 
}