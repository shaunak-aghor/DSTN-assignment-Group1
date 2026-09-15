#ifndef MMU_H
#define MMU_H

#include <stdint.h>
#include "MM.h"          /* pulls in MemHier.h, TLB.h and WB.h */

/* Address translation.  One function, no state of its own. */

typedef enum {
    ACC_READ  = 0,
    ACC_WRITE = 1,
    ACC_EXEC  = 2
} AccessType;

/* What one translation did, as OR-able flags. NOT MUTUALLY EXCLUSIVE.*/
typedef enum {
    XLATE_OK         = 0,
    XLATE_TLB_HIT    = 1 << 0,  /* answered by the TLB, memory untouched */
    XLATE_TLB_EVICT  = 1 << 1,  /* the TLB fill displaced a valid entry */
    XLATE_FAULT      = 1 << 2,  /* the page was not resident */
    XLATE_DISK_READ  = 1 << 3,  /* a page was brought in from disk */
    XLATE_WROTE_BACK = 1 << 4,  /* the victim was dirty and was flushed */
    XLATE_EVICTED    = 1 << 5,  /* a frame was reclaimed; see *evicted_out */
    XLATE_OOM        = 1 << 6   /* translation FAILED: out of memory */
} XlateResult;

/* Translates va for proc, faulting the page in if it is not resident.
 * Returns XLATE_* flags describing what happened. Any reclaimed frame is written 
 * to *evicted_out and is already cleared from the TLB, must clear it from L1/L2. */
XlateResult va_to_pa(TLB *tlb, MM *mm, Process *proc, uint32_t va,
                     AccessType acc, WriteBuffer *wb,
                     uint32_t *pa_out, uint32_t *evicted_out);

#endif /* MMU_H */
