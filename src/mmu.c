/* ============================================================================
 *  src/mmu.c -- virtual to physical translation.  See include/mmu.h.
 * ==========================================================================*/

#include <string.h>
#include "mmu.h"

int va_to_pa(TLB *tlb, MM *mm, Process *proc, uint32_t va, AccessType acc,
             WriteBuffer *wb, uint32_t *pa_out, uint32_t *evicted_out,
             XlateInfo *info)
{
    uint32_t vpn, off, pfn;
    PTE     *pte;
    MMFault  fault;

    if (evicted_out)
        *evicted_out = MM_NO_FRAME;
    if (info)
        memset(info, 0, sizeof(*info));

    if (!tlb || !mm || !proc || !proc->pt)
        return -1;

    /* The virtual address space is VA_BITS wide.  Traces carry wider
     * addresses, so the low VA_BITS are kept: that preserves the page offset
     * and the cache index, which live in the low bits.  Taking the high bits
     * instead would collapse every address within a page onto one another. */
    va &= (uint32_t)MASK(VA_BITS);

    vpn = (uint32_t)VA_VPN(va);
    off = (uint32_t)VA_OFFSET(va);

    /* --- TLB hit: main memory is not touched at all --------------------- */
    if (tlb_lookup(tlb, proc->pid, vpn, &pfn)) {
        if (info) info->tlb_hit = 1;
        *pa_out = (pfn << PAGE_OFFSET_BITS) | off;
        return 0;
    }

    /* --- miss: the walker runs.  Page tables live in main memory and are
     *     never cached, so this is one genuine memory access on top of the
     *     data access that follows. ------------------------------------- */
    pte = &proc->pt->entries[vpn];

    if (!pte->present) {
        if (info) info->faulted = 1;

        if (mm_handle_fault(mm, proc, (uint8_t)vpn, wb, evicted_out, &fault) != 0)
            return -1;                  /* out of memory */

        if (info) {
            info->disk_read  = fault.disk_read;
            info->wrote_back = fault.wrote_back;
            info->evicted    = fault.evicted;
        }

        /* The reclaimed frame may still be named by a TLB entry.  That is the
         * dangerous one: a TLB hit skips the page table entirely and would
         * hand the dead frame straight back.  L1 and L2 are the caller's. */
        if (evicted_out && *evicted_out != MM_NO_FRAME)
            tlb_invalidate_frame(tlb, *evicted_out);
    }

    /* The walker sets Accessed, and Dirty on a store.  This is the ONLY place
     * either is written -- hits never reach here. */
    pte->referenced = 1;
    if (acc == ACC_WRITE)
        pte->dirty = 1;

    pfn = pte->frame;
    if (tlb_insert(tlb, proc->pid, vpn, pfn) && info)
        info->tlb_evict = 1;

    *pa_out = (pfn << PAGE_OFFSET_BITS) | off;
    return 0;
}
