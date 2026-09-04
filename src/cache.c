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
    int *result = arguments[3]; 
    pthread_t *l2_thread = arguments[4];
    sem_t *start_signal = arguments[5];
    

    sem_post(start_signal);

    if (l1_probe(l1, *pa) >= 0) {
        *result = 1;
        pthread_cancel(*l2_thread); 
    } 
    else if (write_buffer != NULL && wb_probe(write_buffer, *pa) >= 0) {
        *result = 2;
        pthread_cancel(*l2_thread); 
    } else {
        *result = 0;
    }

    return NULL;
}

static void *search_l2_worker(void *argument)
{
    void **arguments = argument;
    L2Cache *l2 = arguments[0];
    uint32_t *pa = arguments[1];
    int *result = arguments[2]; 
    sem_t *start_signal = arguments[3]; 

   
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    
    sem_wait(start_signal);

    if (l2 != NULL && l2_probe(l2, *pa) >= 0) {
        *result = 3;
    } else {
        *result = 0;
    }
    
    return NULL; 
}

int cache_search(L1Cache *l1, L2Cache *l2, WriteBuffer *write_buffer,
                 uint32_t pa)
{
    pthread_t l1_thread;
    pthread_t l2_thread;
    
    uint32_t search_pa = pa;
    int l1_result = 0;
    int l2_result = 0;


    sem_t start_signal;
    sem_init(&start_signal, 0, 0);

    
    void *l2_arguments[4] = { l2, &search_pa, &l2_result, &start_signal };
    pthread_create(&l2_thread, NULL, search_l2_worker, l2_arguments);

    
    void *l1_arguments[6] = { l1, write_buffer, &search_pa, &l1_result, &l2_thread, &start_signal };
    pthread_create(&l1_thread, NULL, search_l1_worker, l1_arguments);

    
    pthread_join(l1_thread, NULL);
    pthread_join(l2_thread, NULL);

    
    sem_destroy(&start_signal);

    if (l1_result != 0)
        return l1_result; 
    if (l2_result != 0)
        return l2_result; 
        
    return 0; 
}