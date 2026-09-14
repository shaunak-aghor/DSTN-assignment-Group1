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

/*
 * Virtual address -> physical address, faulting the page in if it is missing.
 *
 *   0   success, *pa_out is valid
 *  -1   no such process, or main memory could not supply a frame at all --
 *       the caller should stop: the simulation is out of memory
 *
 * THE ACCESSED BIT.  On a TLB hit main memory is NEVER touched -- no walk, no
 * referenced bit, nothing.  The page may be hit a million times and the PTE
 * does not change; that is what makes a TLB worth having.  `referenced` is
 * written only by the walker, on a miss.  mm_age_tick() is what closes the
 * loop: it samples the bit, clears it, and SHOOTS DOWN the TLB entry so the
 * next access is forced to walk and set it again.
 *
 * EVICTION.  A fault can reclaim one frame.  If it did, its number is written
 * to *evicted_out (else MM_NO_FRAME).  The TLB is invalidated here, because
 * the TLB is in hand -- but L1 and L2 are physically tagged and hold that
 * frame's lines, so THE CALLER MUST invalidate them:
 *
 *     l1_invalidate_frame(&l1, evicted);
 *     l2_invalidate_frame(&l2, evicted);
 *
 * The write buffer is passed through to mm_handle_fault, which drains any
 * queued store to the victim before the frame is taken.
 */
int va_to_pa(TLB *tlb, MM *mm, Process *proc, uint32_t va, AccessType acc,
             WriteBuffer *wb, uint32_t *pa_out, uint32_t *evicted_out);

#endif /* MMU_H */
