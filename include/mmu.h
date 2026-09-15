#ifndef MMU_H
#define MMU_H

#include <stdint.h>
#include "MM.h"          /* pulls in MemHier.h, TLB.h and WB.h */

/* ============================================================================
 *  Address translation.  One function, no state of its own.
 *
 *  There is no MMU struct because there is nothing left to put in one: the TLB
 *  is owned by the driver, tlb->hits / tlb->misses already count the lookups,
 *  the number of page-table walks IS tlb->misses (a walk happens exactly on a
 *  miss), and the aging timer belongs to the driver that calls mm_age_tick().
 * ==========================================================================*/

typedef enum {
    ACC_READ  = 0,
    ACC_WRITE = 1,
    ACC_EXEC  = 2
} AccessType;

/* What one translation did, as OR-able flags.  These are NOT mutually
 * exclusive -- one access can miss the TLB, fault, read from disk, reclaim a
 * frame and flush it -- which is why this is a flag set rather than a plain
 * enum.  The caller counts; no module keeps statistics. */
typedef enum {
    XLATE_OK         = 0,
    XLATE_TLB_HIT    = 1 << 0,  /* answered by the TLB, memory untouched  */
    XLATE_TLB_EVICT  = 1 << 1,  /* the TLB fill displaced a valid entry   */
    XLATE_FAULT      = 1 << 2,  /* the page was not resident              */
    XLATE_DISK_READ  = 1 << 3,  /* a page was brought in from disk        */
    XLATE_WROTE_BACK = 1 << 4,  /* the victim was dirty and was flushed   */
    XLATE_EVICTED    = 1 << 5,  /* a frame was reclaimed; see *evicted_out*/
    XLATE_OOM        = 1 << 6   /* translation FAILED: out of memory      */
} XlateResult;

/* Translates va for `proc`, faulting the page in if it is not resident.
 * Returns XLATE_* flags describing what happened; XLATE_OOM means it failed
 * and *pa_out is not valid.  Any reclaimed frame is written to *evicted_out
 * and is already cleared from the TLB; the caller must clear it from L1/L2. */
XlateResult va_to_pa(TLB *tlb, MM *mm, Process *proc, uint32_t va,
                     AccessType acc, WriteBuffer *wb,
                     uint32_t *pa_out, uint32_t *evicted_out);

#endif /* MMU_H */
