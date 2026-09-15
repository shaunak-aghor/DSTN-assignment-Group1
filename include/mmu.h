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

/* What one translation did.  The caller counts; no module keeps statistics. */
typedef struct {
    unsigned tlb_hit    : 1;   /* answered by the TLB, memory untouched   */
    unsigned tlb_evict  : 1;   /* the TLB fill displaced a valid entry     */
    unsigned faulted    : 1;   /* the page was not resident                */
    unsigned disk_read  : 1;   /* a page was brought in from disk          */
    unsigned wrote_back : 1;   /* the victim was dirty and was flushed     */
    unsigned evicted    : 1;   /* a frame was reclaimed; see *evicted_out  */
} XlateInfo;

/* Translates va for `proc`, faulting the page in if it is not resident.
 * Returns 0 with *pa_out set, or -1 when memory is exhausted.  Any reclaimed
 * frame is written to *evicted_out and is already cleared from the TLB; the
 * caller must clear it from L1 and L2.  *info reports what the access did. */
int va_to_pa(TLB *tlb, MM *mm, Process *proc, uint32_t va, AccessType acc,
             WriteBuffer *wb, uint32_t *pa_out, uint32_t *evicted_out,
             XlateInfo *info);

#endif /* MMU_H */
