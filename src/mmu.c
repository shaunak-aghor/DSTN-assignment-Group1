

#include "mmu.h"
/*MMU logic for resolving (pid,vpn) request */
XlateResult va_to_pa(TLB *tlb, MM *mm, Process *proc, uint32_t va,
                     AccessType acc, WriteBuffer *wb,
                     uint32_t *pa_out, uint32_t *evicted_out)
{
    XlateResult r = XLATE_OK;
    uint32_t    vpn, off, pfn;
    PTE        *pte;

    if (evicted_out)
        *evicted_out = MM_NO_FRAME;

    if (!tlb || !mm || !proc || !proc->pt)
        return XLATE_OOM;

  
    va &= (uint32_t)MASK(VA_BITS);
   /* Extracting virtual page number bits(8-bits) from virtual address */
   /* Extracting page offset(10 bits) from virtual address*/
    vpn = (uint32_t)VA_VPN(va);
    off = (uint32_t)VA_OFFSET(va);

    /* TLB lookup first see if (pid,vpn) mapping exists */
    if (tlb_lookup(tlb, proc->pid, vpn, &pfn)) {
        *pa_out = (pfn << PAGE_OFFSET_BITS) | off;
        return XLATE_TLB_HIT;
    }

   /* TLB miss -> go for page table walk*/
    pte = &proc->pt->entries[vpn];
    /* TLB MISS -> PAGE FAULT -> call m/m handle method to handle page fault(page has to brought from disk->main m/m)*/
    if (!pte->present) {
        MMResult f = mm_handle_fault(mm, proc, (uint8_t)vpn, wb, evicted_out);

        if (f & MM_OOM)
            return XLATE_OOM;

        r |= XLATE_FAULT;
        if (f & MM_DISK_READ)  r |= XLATE_DISK_READ;
        if (f & MM_WROTE_BACK) r |= XLATE_WROTE_BACK;
        if (f & MM_EVICTED)    r |= XLATE_EVICTED;

        /*invalidate evicted frame from m/m if it is referenced in TLB*/
        if (evicted_out && *evicted_out != MM_NO_FRAME)
            tlb_invalidate_frame(tlb, *evicted_out);
    }

    /* set referenced bit for pte access*/
    pte->referenced = 1;
    if (acc == ACC_WRITE)
        pte->dirty = 1;

    pfn = pte->frame;
    if (tlb_insert(tlb, proc->pid, vpn, pfn))
        r |= XLATE_TLB_EVICT;

        /* Construct physical address using frame number */
    *pa_out = (pfn << PAGE_OFFSET_BITS) | off;
    return r;
}
