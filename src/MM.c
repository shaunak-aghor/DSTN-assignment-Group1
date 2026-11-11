#include <stdlib.h>
#include <string.h>
#include "MM.h"
#include "stats.h"

/* ---------------------------------------------------------------------------
 * Free frame list
 * ------------------------------------------------------------------------- */

/* Takes a free frame and gives it to pid. Returns -1 if none is free. */
static int alloc_frame(MM *mm, FrameKind kind, uint16_t pid, uint32_t vpn)
{
    if (mm->free_count == 0)
        return -1;

    uint32_t f = mm->free_list[--mm->free_count];
    mm->frames[f].kind  = kind;
    mm->frames[f].pid   = pid;
    mm->frames[f].vpn   = vpn;
    mm->frames[f].aging = 0x80;         /* a new page starts as recently used */
    mm->frames[f].pte   = NULL;
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
 * Page-table frames are never chosen. Ties go to the lowest frame number.
 * Returns -1 if nothing can be taken.
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
        if (!only_pid && owner_of(mm, f)->pages_held <= owner_of(mm, f)->lower_limit)
            continue;

        if (best < 0 || fd->aging < mm->frames[best].aging)
            best = (int)f;
    }
    return best;
}

/*
 * Takes data frame f away from the page in it:
 *   1. stores to f still in the write buffer are drained to memory first
 *   2. the page's PTE is cleared (and the page written back if dirty)
 *   3. f is recorded in mm->evicted and goes back on the free list
 */
static void evict_frame(MM *mm, uint32_t f, WriteBuffer *wb)
{
    PTE *pte = mm->frames[f].pte;

    while (wb && wb_probe_frame(wb, f) >= 0) {
        /* The drained block lands in memory, so its page is now dirty. */
        uint32_t df = PA_FRAME(WB_TO_PA(wb->entries[0].block_addr));
        if (mm->frames[df].kind == FRAME_DATA)
            mm->frames[df].pte->dirty = 1;
        wb_drain_head(wb);
    }

    if (pte->dirty)
        stats.disk_writebacks++;
    memset(pte, 0, sizeof(PTE));
    owner_of(mm, f)->pages_held--;

    stats.mm_evictions++;
    mm->evicted[mm->num_evicted++] = f;
    free_frame(mm, f);
}

/* Returns a frame for pid: a free one, or else the coldest page of any process. -1 if none. */
static int take_frame(MM *mm, FrameKind kind, uint16_t pid, uint32_t vpn, WriteBuffer *wb)
{
    if (mm->free_count == 0) {
        int v = pick_victim(mm, 0);
        if (v < 0)
            return -1;
        evict_frame(mm, (uint32_t)v, wb);
    }
    return alloc_frame(mm, kind, pid, vpn);
}

/* ---------------------------------------------------------------------------
 * Page tables
 * ------------------------------------------------------------------------- */

/* Allocates an empty page table in a new frame and points `entry` at it. */
static int new_table(MM *mm, Process *proc, PTE *entry, WriteBuffer *wb)
{
    int f = take_frame(mm, FRAME_PGTBL, proc->pid, 0, wb);
    if (f < 0)
        return -1;

    mm->table_at[f] = calloc(1, sizeof(PageTable));
    entry->present  = 1;
    entry->frame    = (uint32_t)f;
    proc->table_frames++;
    stats.pt_tables_created++;
    return 0;
}

PTE *mm_walk(MM *mm, Process *proc, uint32_t vpn, WriteBuffer *wb)
{
    PTE *entry;

    /* Level 1: outer table -> middle table */
    entry = &mm->table_at[proc->root_frame]->entries[VPN_OUTER(vpn)];
    if (!entry->present && new_table(mm, proc, entry, wb) != 0)
        return NULL;

    /* Level 2: middle table -> inner table */
    entry = &mm->table_at[entry->frame]->entries[VPN_MIDDLE(vpn)];
    if (!entry->present && new_table(mm, proc, entry, wb) != 0)
        return NULL;

    /* Level 3: inner table -> the page's PTE */
    return &mm->table_at[entry->frame]->entries[VPN_INNER(vpn)];
}

/* ---------------------------------------------------------------------------
 * Page faults and aging
 * ------------------------------------------------------------------------- */

int mm_handle_fault(MM *mm, Process *proc, uint32_t vpn, PTE *pte, WriteBuffer *wb)
{
    int f;

    /* At its upper limit: the process must give up one of its own pages. */
    if (proc->pages_held >= proc->upper_limit) {
        int v = pick_victim(mm, proc->pid);
        if (v < 0)
            return -1;
        evict_frame(mm, (uint32_t)v, wb);
    }

    /* Use a free frame, or take the coldest page from anyone (global replacement). */
    f = take_frame(mm, FRAME_DATA, proc->pid, vpn, wb);
    if (f < 0)
        return -1;

    /* The page is read from disk into frame f. */
    mm->frames[f].pte = pte;
    pte->present    = 1;
    pte->frame      = (uint32_t)f;
    pte->referenced = 1;
    pte->dirty      = 0;
    proc->pages_held++;
    return 0;
}

/*
 * Runs every AGE_TICK_INTERVAL accesses. For every page in memory:
 *     aging = (aging >> 1) | (referenced ? 0x80 : 0)
 * then clears the referenced bit. The page's TLB entry is dropped so the
 * next access walks the page table and sets the referenced bit again.
 */
void mm_age_tick(MM *mm, TLB *tlb)
{
    for (uint32_t f = 0; f < NUM_FRAMES; f++) {
        FrameDesc *fd = &mm->frames[f];
        if (fd->kind != FRAME_DATA)
            continue;

        fd->aging = (uint8_t)((fd->aging >> 1) | (fd->pte->referenced ? 0x80 : 0));

        if (fd->pte->referenced) {
            fd->pte->referenced = 0;
            tlb_invalidate_entry(tlb, fd->pid, fd->vpn);
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
    proc->lower_limit = (lower_limit < MIN_PAGES_PER_PROC) ? MIN_PAGES_PER_PROC : lower_limit;
    proc->upper_limit = (upper_limit == 0) ? NUM_FRAMES : upper_limit;
    if (proc->upper_limit < proc->lower_limit)
        proc->upper_limit = proc->lower_limit;

    /* The outer page table: created now and kept for the life of the process. */
    f = alloc_frame(mm, FRAME_PGTBL, pid, 0);
    mm->table_at[f]    = calloc(1, sizeof(PageTable));
    proc->root_frame   = (uint32_t)f;
    proc->table_frames = 1;
    stats.pt_tables_created++;

    /* Pre-page the first two pages (this also creates their middle and inner tables). */
    mm->num_evicted = 0;
    for (uint32_t vpn = 0; vpn < MIN_PAGES_PER_PROC; vpn++) {
        PTE *pte = mm_walk(mm, proc, vpn, NULL);
        if (pte == NULL || mm_handle_fault(mm, proc, vpn, pte, NULL) != 0)
            return -1;
    }
    return 0;
}
