#include <stdlib.h>
#include <string.h>
#include "MM.h"
#include "stats.h"

/* ---------------------------------------------------------------------------
 * Free frame list
 * ------------------------------------------------------------------------- */

/* Takes a free frame and gives it to (pid, vpn). Returns -1 if none is free. */
static int alloc_frame(MM *mm, uint16_t pid, uint8_t vpn, FrameKind kind)
{
    if (mm->free_count == 0)
        return -1;

    uint32_t f = mm->free_list[--mm->free_count];
    mm->frames[f].kind  = kind;
    mm->frames[f].pid   = pid;
    mm->frames[f].vpn   = vpn;
    mm->frames[f].aging = 0x80;         /* a new page starts as recently used */
    return (int)f;
}

static void free_frame(MM *mm, uint32_t f)
{
    memset(&mm->frames[f], 0, sizeof(FrameDesc));
    mm->free_list[mm->free_count++] = f;
}

static Process *owner_of(MM *mm, uint32_t f)
{
    return &mm->procs[mm->frames[f].pid - 1];
}

void mm_init(MM *mm, Process *procs, uint16_t num_procs)
{
    memset(mm, 0, sizeof(*mm));
    memset(procs, 0, num_procs * sizeof(Process));
    mm->procs     = procs;
    mm->num_procs = num_procs;

    /* Push in reverse so frame 0 is handed out first. */
    for (int f = NUM_FRAMES - 1; f >= 0; f--)
        free_frame(mm, (uint32_t)f);
}

/* ---------------------------------------------------------------------------
 * Page replacement: LFU with aging
 * ------------------------------------------------------------------------- */

/*
 * Returns the data frame with the smallest aging value (the coldest page).
 * only_pid = 0 : search all processes, skipping any already at its lower limit.
 * only_pid > 0 : search only that process's pages.
 * Ties go to the lowest frame number. Returns -1 if nothing can be taken.
 */
static int pick_victim(MM *mm, uint16_t only_pid)
{
    int best = -1;

    for (uint32_t f = 0; f < NUM_FRAMES; f++) {
        FrameDesc *fd = &mm->frames[f];

        if (fd->kind != FRAME_DATA)
            continue;
        if (only_pid && fd->pid != only_pid)
            continue;
        if (!only_pid && owner_of(mm, f)->frames_held <= owner_of(mm, f)->lower_limit)
            continue;

        if (best < 0 || fd->aging < mm->frames[best].aging)
            best = (int)f;
    }
    return best;
}

/*
 * Takes frame f away from the page in it:
 *   1. stores to f still in the write buffer are drained to memory first
 *   2. the owner's PTE is cleared (and the page written back if dirty)
 *   3. f goes back on the free list
 */
static void evict_frame(MM *mm, uint32_t f, WriteBuffer *wb, uint32_t *out_frame)
{
    Process *owner = owner_of(mm, f);
    PTE     *pte   = &owner->pt->entries[mm->frames[f].vpn];

    while (wb && wb_probe_frame(wb, f) >= 0) {
        /* The drained block lands in memory, so its page is now dirty. */
        uint32_t df = PA_FRAME(WB_TO_PA(wb->entries[0].block_addr));
        if (mm->frames[df].kind == FRAME_DATA)
            owner_of(mm, df)->pt->entries[mm->frames[df].vpn].dirty = 1;
        wb_drain_head(wb);
    }

    if (pte->dirty)
        stats.disk_writebacks++;
    memset(pte, 0, sizeof(PTE));
    owner->frames_held--;

    stats.mm_evictions++;
    *out_frame = f;
    free_frame(mm, f);
}

int mm_handle_fault(MM *mm, Process *proc, uint8_t vpn,
                    WriteBuffer *wb, uint32_t *out_frame)
{
    PTE *pte = &proc->pt->entries[vpn];
    uint32_t dummy;
    int f;

    if (out_frame == NULL)
        out_frame = &dummy;
    *out_frame = MM_NO_FRAME;

    /* At its upper limit: the process must give up one of its own pages. */
    if (proc->frames_held >= proc->upper_limit) {
        int v = pick_victim(mm, proc->pid);
        if (v < 0)
            return -1;
        evict_frame(mm, (uint32_t)v, wb, out_frame);
    }

    /* Use a free frame, or take the coldest page from anyone (global replacement). */
    f = alloc_frame(mm, proc->pid, vpn, FRAME_DATA);
    if (f < 0) {
        int v = pick_victim(mm, 0);
        if (v < 0)
            return -1;
        evict_frame(mm, (uint32_t)v, wb, out_frame);
        f = alloc_frame(mm, proc->pid, vpn, FRAME_DATA);
    }

    /* The page is read from disk into frame f. */
    pte->present    = 1;
    pte->frame      = (uint32_t)f;
    pte->referenced = 1;
    pte->dirty      = 0;
    proc->frames_held++;
    return 0;
}

/*
 * Runs every AGE_TICK_INTERVAL accesses. For every resident page:
 *     aging = (aging >> 1) | (referenced ? 0x80 : 0)
 * then clears the referenced bit. The page's TLB entry is dropped so the
 * next access walks the page table and sets the referenced bit again.
 */
void mm_age_tick(MM *mm, TLB *tlb)
{
    for (int i = 0; i < mm->num_procs; i++) {
        Process *proc = &mm->procs[i];

        for (uint32_t v = 0; v < PAGES_PER_PROC; v++) {
            PTE *pte = &proc->pt->entries[v];
            if (!pte->present)
                continue;

            FrameDesc *fd = &mm->frames[pte->frame];
            fd->aging = (uint8_t)((fd->aging >> 1) | (pte->referenced ? 0x80 : 0));

            if (pte->referenced) {
                pte->referenced = 0;
                tlb_invalidate_entry(tlb, proc->pid, v);
            }
        }
    }
}

/* ---------------------------------------------------------------------------
 * Process creation
 * ------------------------------------------------------------------------- */

int mm_create_process(MM *mm, Process *proc, uint16_t pid,
                      uint32_t lower_limit, uint32_t upper_limit)
{
    int f;

    if (mm->free_count < MIN_FRAMES_PER_PROC)
        return -1;

    proc->pid = pid;
    proc->lower_limit = (lower_limit < MIN_FRAMES_PER_PROC) ? MIN_FRAMES_PER_PROC : lower_limit;
    proc->upper_limit = (upper_limit == 0 || upper_limit > PAGES_PER_PROC) ? PAGES_PER_PROC : upper_limit;
    if (proc->upper_limit < proc->lower_limit)
        proc->upper_limit = proc->lower_limit;

    /* The page table lives in a frame of its own, which is never evicted. */
    proc->pt = calloc(1, sizeof(PageTable));
    f = alloc_frame(mm, pid, 0, FRAME_PGTBL);
    proc->pt_frame    = (uint32_t)f;
    proc->frames_held = 1;

    /* Pre-page the first two pages. */
    for (uint8_t v = 0; v < 2; v++)
        if (mm_handle_fault(mm, proc, v, NULL, NULL) != 0)
            return -1;

    return 0;
}
