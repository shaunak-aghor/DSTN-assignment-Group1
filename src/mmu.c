#include "mmu.h"
#include "stats.h"

int va_to_pa(TLB *tlb, MM *mm, Process *proc, uint32_t va, AccessType acc,
             WriteBuffer *wb, uint32_t *pa_out, uint32_t *evicted_out)
{
    uint32_t vpn    = VA_VPN(va);
    uint32_t offset = VA_OFFSET(va);
    uint32_t pfn;
    PTE     *pte;

    *evicted_out = MM_NO_FRAME;

    /* 1. TLB hit: no memory access needed. */
    if (tlb_lookup(tlb, proc->pid, vpn, &pfn)) {
        *pa_out = (pfn << PAGE_OFFSET_BITS) | offset;
        return 0;
    }

    /* 2. TLB miss: walk the page table (costs one memory access). */
    stats.pt_walks++;
    pte = &proc->pt->entries[vpn];

    /* 3. Page not in memory: page fault, read it from disk. */
    if (!pte->present) {
        stats.page_faults++;
        if (mm_handle_fault(mm, proc, (uint8_t)vpn, wb, evicted_out) != 0)
            return -1;
        if (*evicted_out != MM_NO_FRAME)
            tlb_invalidate_frame(tlb, *evicted_out);
    }

    pte->referenced = 1;
    if (acc == ACC_WRITE)
        pte->dirty = 1;

    /* 4. Remember the translation in the TLB. */
    tlb_insert(tlb, proc->pid, vpn, pte->frame);
    *pa_out = (pte->frame << PAGE_OFFSET_BITS) | offset;
    return 0;
}
