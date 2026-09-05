/* ============================================================================
 *  src/translate.c -- MMU: the complete virtual-to-physical resolution path
 *
 *  See include/translate.h for the flow diagram and the two documented
 *  limitations (protection is checked on TLB fill only; cache invalidation
 *  is delegated through a callback because l1.c / l2.c are owned elsewhere).
 * ==========================================================================*/

#include <string.h>

#include "translate.h"

/* ------------------------------------------------------------------------
 *  Names
 * --------------------------------------------------------------------- */

const char *mmu_status_name(MMUStatus s)
{
    switch (s) {
    case MMU_OK:                return "OK (TLB hit)";
    case MMU_OK_TLB_MISS:       return "OK (TLB miss, PTE present)";
    case MMU_OK_PAGE_FAULT:     return "OK (page fault serviced)";
    case MMU_FAULT_ADDRESS:     return "FAULT: address out of range";
    case MMU_FAULT_PROTECTION:  return "FAULT: protection violation";
    case MMU_FAULT_NO_FRAME:    return "FAULT: no evictable frame";
    case MMU_FAULT_NO_PROCESS:  return "FAULT: process not runnable";
    }
    return "FAULT: unknown";
}

/* ------------------------------------------------------------------------
 *  Frame reuse -- the callback MainMemory invokes just before a frame is
 *  handed to a different page.
 *
 *  Order matters.  The TLB is cleared first because it is the structure that
 *  can hand out the frame number again; then the caches, which hold the
 *  bytes.  Both must happen before the frame is refilled.
 * --------------------------------------------------------------------- */
static void mmu_on_frame_evicted(void *ctx, uint32_t frame,
                                 uint16_t pid, uint32_t vpn)
{
    MMU *m = (MMU *)ctx;
    unsigned before, after;
    unsigned i;

    (void)pid; (void)vpn;
    if (!m) return;

    before = 0;
    for (i = 0; i < TLB_ENTRIES; i++) before += m->tlb.entries[i].valid;

    /* Every process's mapping of this frame, not just the owner's -- a
     * shared frame can be reachable from several page tables. */
    tlb_impl_invalidate_frame(&m->tlb, frame);

    after = 0;
    for (i = 0; i < TLB_ENTRIES; i++) after += m->tlb.entries[i].valid;

    m->tlb_invalidations += (before - after);
    m->evictions_seen++;

    /* L1 and L2 are physically tagged: without this, a line surviving in
     * the frame's 64 blocks returns the previous page's bytes. */
    if (m->cache_inval)
        m->cache_inval(m->cache_ctx, frame);
}

/* ------------------------------------------------------------------------
 *  Setup
 * --------------------------------------------------------------------- */

void mmu_init(MMU *m, MainMemory *mm, Process *procs, uint16_t num_procs)
{
    if (!m) return;

    memset(m, 0, sizeof(*m));
    tlb_impl_init(&m->tlb);

    m->mm            = mm;
    m->procs         = procs;
    m->num_procs     = num_procs;
    m->tick_interval = 1000;            /* aging sample every 1000 accesses */

    /* Both registrations are required for correct GLOBAL replacement.
     * mm_set_process_table lets a fault clear the VICTIM OWNER's PTE;
     * mm_set_evict_notifier lets us flush the TLB and caches for the frame. */
    mm_set_process_table(procs, num_procs);
    mm_set_evict_notifier(mmu_on_frame_evicted, m);
}

void mmu_set_cache_invalidator(MMU *m, CacheInvalidateFn fn, void *ctx)
{
    if (!m) return;
    m->cache_inval = fn;
    m->cache_ctx   = ctx;
}

void mmu_set_tick_interval(MMU *m, uint64_t accesses_per_tick)
{
    if (!m) return;
    m->tick_interval = accesses_per_tick;
    m->since_tick    = 0;
}

void mmu_reset_stats(MMU *m)
{
    if (!m) return;
    m->accesses = m->tlb_hits = m->tlb_misses = m->pt_walks = 0;
    m->page_faults = m->prot_faults = m->addr_faults = 0;
    m->frame_failures = m->evictions_seen = m->tlb_invalidations = 0;
    tlb_impl_reset_stats(&m->tlb);
}

Process *mmu_find_process(MMU *m, uint16_t pid)
{
    uint16_t i;
    if (!m || !m->procs) return NULL;
    for (i = 0; i < m->num_procs; i++)
        if (m->procs[i].active && m->procs[i].pid == pid)
            return &m->procs[i];
    return NULL;
}

/* ------------------------------------------------------------------------
 *  Protection
 * --------------------------------------------------------------------- */
static int prot_allows(uint32_t prot, AccessType acc)
{
    switch (acc) {
    case ACC_READ:  return (prot & PROT_READ)  != 0;
    case ACC_WRITE: return (prot & PROT_WRITE) != 0;
    case ACC_EXEC:  return (prot & PROT_EXEC)  != 0;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 *  THE TRANSLATION
 *
 *  Step numbers match the diagram at the top of include/translate.h.
 * --------------------------------------------------------------------- */
MMUStatus mmu_translate(MMU *m, uint16_t pid, uint32_t va,
                        AccessType acc, uint32_t *pa_out)
{
    Process  *proc;
    PTE      *pte;
    uint32_t  vpn, off, pfn, pa;
    int       faulted = 0;              /* a page fault was serviced      */
    int       walked  = 0;              /* the page table had to be read  */

    if (!m || !m->mm) return MMU_FAULT_NO_PROCESS;

    m->accesses++;

    /* --- periodic aging sample ---------------------------------------
     * Done here, before the access, so a tick can never land in the middle
     * of a fault and change the aging values the victim search is reading. */
    if (m->tick_interval && ++m->since_tick >= m->tick_interval) {
        m->since_tick = 0;
        mm_age_tick(m->mm, m->procs, m->num_procs);
    }

    /* --- [0] address range -------------------------------------------
     * The virtual address space is 18 bits.  Anything above 0x3FFFF has no
     * page number at all -- this is a segmentation fault, and it must be
     * caught before VA_VPN() silently truncates it to 8 bits. */
    if (va >= (1u << VA_BITS)) {
        m->addr_faults++;
        return MMU_FAULT_ADDRESS;
    }

    /* --- [1] split ---------------------------------------------------- */
    vpn = (uint32_t)VA_VPN(va);
    off = (uint32_t)VA_OFFSET(va);

    /* --- [2] runnable process with a resident page table -------------- */
    proc = mmu_find_process(m, pid);
    if (!proc || !proc->pt)
        return MMU_FAULT_NO_PROCESS;

    /* --- [3] TLB lookup on the PAIR (pid, vpn) ------------------------
     * The PID half of the comparison is what lets entries from several
     * processes coexist, so a context switch needs no flush. */
    if (tlb_impl_lookup(&m->tlb, pid, vpn, &pfn)) {
        m->tlb_hits++;
        pte = &proc->pt->entries[vpn];

        /* Protection is NOT re-checked here -- TLBImplEntry carries no prot
         * bits, and reading the PTE would cost a memory access per
         * reference. See the limitation note in translate.h. */
        goto have_frame;
    }

    m->tlb_misses++;

    /* --- [4] page-table walk ------------------------------------------
     * Page tables live in main memory and are never cached (assumption 5),
     * so this is one genuine main-memory access on top of the data access
     * that follows.  With a 256-entry table it is a single indexed read --
     * no multi-level walk. */
    m->pt_walks++;
    walked = 1;
    pte    = &proc->pt->entries[vpn];

    /* --- [5] protection, checked BEFORE any disk work ------------------
     * Faulting a page in and only then discovering the access is illegal
     * would waste a frame and a disk read on an access that cannot succeed. */
    if (!prot_allows(pte->prot, acc)) {
        m->prot_faults++;
        return MMU_FAULT_PROTECTION;
    }

    /* --- [6] present? ------------------------------------------------- */
    if (!pte->present) {
        m->page_faults++;
        faulted = 1;

        /* mm_handle_fault does the whole thing: honours upper_limit, takes
         * a free frame or picks a global LFU-with-aging victim, writes the
         * victim back if dirty, clears the VICTIM OWNER's PTE, fires the
         * eviction callback above (TLB + cache invalidation), reads the page
         * from disk and publishes the new mapping. */
        if (mm_handle_fault(m->mm, proc, (uint8_t)vpn) != 0) {
            m->frame_failures++;
            return MMU_FAULT_NO_FRAME;  /* every frame is a page table or is
                                           pinned by a lower_limit */
        }

        /* Re-read: mm_handle_fault wrote through the same pointer, but a
         * later multi-level or shared page table would not, and a fault that
         * reports success while leaving the PTE absent must never reach the
         * cache hierarchy as a garbage address. */
        if (!pte->present) {
            m->frame_failures++;
            return MMU_FAULT_NO_FRAME;
        }
    }

    pfn = pte->frame;

    /* --- [7] fill the TLB ---------------------------------------------
     * Inserting an existing (pid, vpn) refreshes it rather than duplicating,
     * so this is safe on the spurious-fault path too. */
    tlb_impl_insert(&m->tlb, pid, vpn, pfn);

have_frame:
    /* --- [8] build the physical address -------------------------------
     * The 10-bit page offset passes through untranslated.  Because L1's
     * index+offset is also 10 bits, the L1 set was already being read while
     * the TLB was translating, and this frame number IS the L1 tag. */
    pa = (pfn << PAGE_OFFSET_BITS) | off;

    pte->referenced = 1;                    /* feeds the next aging tick */
    if (pte->frame < NUM_FRAMES && m->mm->frames[pte->frame].allocated &&
        m->mm->frames[pte->frame].freq != 0xFFFFFFFFu)
        m->mm->frames[pte->frame].freq++;   /* the LFU count */

    if (acc == ACC_WRITE)
        mm_mark_dirty(m->mm, proc, pa);     /* dirty vs DISK, not vs cache:
                                               both caches are write-through */

    if (pa_out) *pa_out = pa & (uint32_t)MASK(PA_BITS);

    if (faulted) return MMU_OK_PAGE_FAULT;   /* cost: walk + disk + restart */
    if (walked)  return MMU_OK_TLB_MISS;     /* cost: one memory access     */
    return MMU_OK;                           /* cost: none beyond the TLB   */
}

/* ------------------------------------------------------------------------
 *  Process teardown
 *
 *  The specification is explicit that TLB entries are invalidated when a
 *  process terminates.  With PID-tagged entries that is the ONLY time a bulk
 *  invalidation is needed -- and it is not optional, because PIDs are 14 bits
 *  and get recycled: a new process reusing this PID would otherwise inherit
 *  the dead one's translations.
 * --------------------------------------------------------------------- */
void mmu_process_exit(MMU *m, uint16_t pid)
{
    Process *proc;
    uint32_t v;

    if (!m || !m->mm) return;

    proc = mmu_find_process(m, pid);

    if (proc && proc->pt) {
        /* Release the data pages first, while the page table is still
         * readable -- it is the only record of which frames they are. */
        for (v = 0; v < PAGES_PER_PROC; v++) {
            PTE *pte = &proc->pt->entries[v];
            uint32_t f;

            if (!pte->present) continue;
            f = pte->frame;

            pte->present = 0;
            pte->dirty   = 0;

            if (f < NUM_FRAMES && m->mm->frames[f].allocated) {
                /* No write-back: the address space is gone, so its dirty
                 * pages have nowhere meaningful to go. */
                if (m->cache_inval) m->cache_inval(m->cache_ctx, f);
                memset(&m->mm->frames[f], 0, sizeof(FrameDesc));
                m->mm->free_frames++;
            }
        }

        /* Then the page-table frame, which is pinned against replacement and
         * so can only ever be released here. */
        if (proc->pt_frame < NUM_FRAMES &&
            m->mm->frames[proc->pt_frame].allocated) {
            if (m->cache_inval) m->cache_inval(m->cache_ctx, proc->pt_frame);
            memset(&m->mm->frames[proc->pt_frame], 0, sizeof(FrameDesc));
            m->mm->free_frames++;
        }

        proc->pt          = NULL;
        proc->pt_frame    = 0;
        proc->frames_held = 0;
        proc->active      = 0;
    }

    /* Last: drop every TLB entry carrying this PID. */
    tlb_impl_invalidate_pid(&m->tlb, pid);
}

/* ------------------------------------------------------------------------
 *  Reporting
 * --------------------------------------------------------------------- */
void mmu_print_stats(const MMU *m, FILE *out)
{
    double tlb_hr, fault_rate;
    uint64_t served;

    if (!m || !out) return;

    served     = m->tlb_hits + m->tlb_misses;
    tlb_hr     = served ? 100.0 * (double)m->tlb_hits / (double)served : 0.0;
    fault_rate = m->accesses ? 100.0 * (double)m->page_faults /
                               (double)m->accesses : 0.0;

    fprintf(out,
        "\n--- MMU ---\n"
        "  accesses ................ %llu\n"
        "  TLB hits / misses ....... %llu / %llu   (hit ratio %.2f%%)\n"
        "  page-table walks ........ %llu   (= %llu uncached memory accesses)\n"
        "  page faults ............. %llu   (%.3f%% of accesses)\n"
        "  protection faults ....... %llu\n"
        "  address faults .......... %llu\n"
        "  out-of-frame failures ... %llu\n"
        "  frames reclaimed ........ %llu\n"
        "  TLB entries invalidated . %llu   (by frame reuse)\n",
        (unsigned long long)m->accesses,
        (unsigned long long)m->tlb_hits, (unsigned long long)m->tlb_misses,
        tlb_hr,
        (unsigned long long)m->pt_walks, (unsigned long long)m->pt_walks,
        (unsigned long long)m->page_faults, fault_rate,
        (unsigned long long)m->prot_faults,
        (unsigned long long)m->addr_faults,
        (unsigned long long)m->frame_failures,
        (unsigned long long)m->evictions_seen,
        (unsigned long long)m->tlb_invalidations);

    if (m->mm)
        fprintf(out,
        "--- Main memory ---\n"
        "  free frames ............. %u / %u\n"
        "  page faults ............. %llu\n"
        "  disk reads .............. %llu\n"
        "  disk write-backs ........ %llu\n"
        "  write-through arrivals .. %llu\n"
        "  16 B block fetches ...... %llu\n",
        m->mm->free_frames, NUM_FRAMES,
        (unsigned long long)m->mm->page_faults,
        (unsigned long long)m->mm->disk_reads,
        (unsigned long long)m->mm->disk_writebacks,
        (unsigned long long)m->mm->writes,
        (unsigned long long)m->mm->block_fetches);
}
