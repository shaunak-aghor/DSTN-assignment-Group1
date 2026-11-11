#ifndef MMU_H
#define MMU_H

#include <stdint.h>
#include "MM.h"

typedef enum {
    ACC_READ = 0,
    ACC_WRITE
} AccessType;

/*
 * Translates virtual address va of proc into a physical address in *pa_out:
 * TLB first, then the page table, then a page fault if the page is not in memory.
 * If the fault took a frame from another page, that frame is written to
 * *evicted_out (else MM_NO_FRAME) so the caller can clear it from L1 and L2.
 * Returns 0, or -1 if memory is full.
 */
int va_to_pa(TLB *tlb, MM *mm, Process *proc, uint32_t va, AccessType acc,
             WriteBuffer *wb, uint32_t *pa_out, uint32_t *evicted_out);

#endif
