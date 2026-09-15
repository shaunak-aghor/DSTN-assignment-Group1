/* ============================================================================
 *  src/MM.c -- main memory: 32 MB, pure paging, global LFU with aging.
 *  Metadata only; see include/MM.h for the model.
 * ==========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MM.h"

/* One page table must occupy exactly one frame, or the "page tables live in
 * main memory" accounting quietly stops meaning anything.  Fail the build. */
typedef char pt_fits_one_frame[(sizeof(PageTable) == PAGE_SIZE) ? 1 : -1];

/* Every vpn here is uint8_t, which is exactly the 8-bit page-number space, so
 * range checks on vpn are structurally unnecessary. */
typedef char vpn_fits_uint8[(PAGES_PER_PROC == 256 && VPN_BITS == 8) ? 1 : -1];

#define NO_FRAME    ((uint32_t)-1)
#define ANY_PID     (-1)

/* ------------------------------------------------------------------------
 *  Free list -- a stack of free frame numbers.
 *
 *  Allocation is a pop and release is a push, both O(1).  There is no scan
 *  and no rotating hint: `free_count` and the frame table cannot disagree,
 *  because a frame is on the list exactly when its kind is FRAME_FREE.
 * --------------------------------------------------------------------- */

static int frame_pop(MM *mm)
{
    if (mm->free_count == 0)
        return -1;
    return (int)mm->free_list[--mm->free_count];
}

static void frame_push(MM *mm, uint32_t f)
{
    memset(&mm->frames[f], 0, sizeof(FrameDesc));   /* kind = FRAME_FREE */
    mm->free_list[mm->free_count++] = f;
}

/* ------------------------------------------------------------------------
 *  Process lookup
 *
 *  Global replacement means the victim may belong to anyone, and two things
 *  need the whole table: skipping a process at its lower_limit, and clearing
 *  the VICTIM OWNER's PTE rather than the faulting process's.  Miss the
 *  second and the owner keeps a present PTE pointing at a frame someone else
 *  now holds -- silent, total corruption.
 * --------------------------------------------------------------------- */

static Process *proc_of(MM *mm, uint16_t pid)
{
    uint16_t i;

    if (!mm->procs)
        return NULL;

    /* pt != NULL IS the liveness test: mm_create_process sets it, and
     * mm_destroy_process clears it.  mm_init zeroes the whole table, so an
     * untouched slot cannot be mistaken for a live process. */
    for (i = 0; i < mm->num_procs; i++)
        if (mm->procs[i].pt && mm->procs[i].pid == pid)
            return &mm->procs[i];

    return NULL;                /* unowned, or the owner already exited --
                                   such a frame is garbage, freely evictable */
}

/* ------------------------------------------------------------------------
 *  Lifecycle
 * --------------------------------------------------------------------- */

int mm_init(MM *mm, Process *procs, uint16_t num_procs)
{
    uint32_t i;

    if (!mm)
        return -1;

    memset(mm, 0, sizeof(*mm));
    mm->procs     = procs;
    mm->num_procs = num_procs;

    /* Every slot starts empty (pt == NULL), so proc_of() can never match a
     * slot that was never created. */
    if (procs && num_procs)
        memset(procs, 0, (size_t)num_procs * sizeof(Process));

    /* Push in reverse so the first frames handed out are 0, 1, 2 ...
     * The post-decrement in the condition is what keeps the body in range:
     * it runs for i = NUM_FRAMES-1 down to 0, never for NUM_FRAMES itself. */
    for (i = NUM_FRAMES; i-- > 0; )
        frame_push(mm, i);

    return 0;
}

void mm_destroy(MM *mm)
{
    if (!mm) return;
    memset(mm->frames, 0, sizeof(mm->frames));
    mm->free_count = 0;
}


/* ------------------------------------------------------------------------
 *  Frame allocation
 * --------------------------------------------------------------------- */

int mm_alloc_frame(MM *mm, uint16_t pid, uint8_t vpn, FrameKind kind)
{
    int f;
    FrameDesc *fd;

    if (kind == FRAME_FREE)
        return -1;

    f = frame_pop(mm);
    if (f < 0)
        return -1;                      /* memory is full; caller evicts */

    fd = &mm->frames[f];
    fd->kind       = (uint8_t)kind;
    fd->pid        = pid;
    fd->vpn        = vpn;
    fd->aging      = 0x80;              /* MSB set -- newest possible */

    return f;
}

/* ------------------------------------------------------------------------
 *  Victim selection -- one ranking function for both callers.
 *
 *  pid_filter == ANY_PID  -> global: any process, honouring lower_limit.
 *  pid_filter >= 0        -> local:  only that process's pages, used when it
 *                            has hit its upper_limit and must displace itself.
 *
 *  Ranking: smallest aging register wins; ties break on the lowest frame
 *  number, so the choice is deterministic and reproducible across runs.
 * --------------------------------------------------------------------- */

static int pick_victim(const MM *mm, int pid_filter)
{
    uint32_t f;
    uint32_t best     = NO_FRAME;
    uint8_t  best_age = 0;

    for (f = 0; f < NUM_FRAMES; f++) {
        const FrameDesc *fd = &mm->frames[f];

        if (fd->kind != FRAME_DATA)                 /* free, or a page table */
            continue;

        if (pid_filter != ANY_PID) {
            if (fd->pid != (uint16_t)pid_filter)
                continue;
        } else {
            const Process *ow = proc_of((MM *)mm, fd->pid);
            if (ow && ow->frames_held <= ow->lower_limit)
                continue;                           /* at its floor */
        }

        /* Strictly less, so an equal aging keeps the earlier (lower) frame --
         * deterministic tie-breaking with no extra state. */
        if (best == NO_FRAME || fd->aging < best_age) {
            best     = f;
            best_age = fd->aging;
        }
    }

    return (best == NO_FRAME) ? -1 : (int)best;
}

int mm_select_victim(const MM *mm)
{
    return pick_victim(mm, ANY_PID);
}

/* ------------------------------------------------------------------------
 *  Aging tick
 *
 *  All replacement state lives on the frame, so this is one flat sweep and
 *  needs no process table.  Shift right, reference bit into the MSB, clear
 *  it; the page table is where the bit lives, so this reads it from memory.
 * --------------------------------------------------------------------- */

void mm_age_tick(MM *mm, TLB *tlb)
{
    uint16_t i;
    uint32_t v;

    if (!mm || !mm->procs)
        return;

    /* Walking the page tables rather than the frame table is both faithful --
     * this is the OS reading Accessed bits out of memory -- and cheaper: it
     * visits only resident pages (<= PAGES_PER_PROC each) instead of all
     * NUM_FRAMES frames. */
    for (i = 0; i < mm->num_procs; i++) {
        Process *proc = &mm->procs[i];

        if (!proc->pt)
            continue;

        for (v = 0; v < PAGES_PER_PROC; v++) {
            PTE       *pte = &proc->pt->entries[v];
            FrameDesc *fd;

            if (!pte->present || pte->frame >= NUM_FRAMES)
                continue;

            fd = &mm->frames[pte->frame];
            fd->aging = (uint8_t)((fd->aging >> 1) |
                                  (pte->referenced ? 0x80u : 0x00u));

            if (pte->referenced) {
                pte->referenced = 0;

                /* THE SHOOTDOWN.  The bit was just cleared in memory, but the
                 * translation is still cached -- every further access would
                 * hit the TLB, the walker would never run, and the bit would
                 * stay 0 no matter how hot the page is.  Dropping the entry
                 * forces the next access to walk and set it again. */
                if (tlb)
                    tlb_invalidate_entry(tlb, proc->pid, v);
            }
        }
    }
}

/* ------------------------------------------------------------------------
 *  Eviction
 *
 *  Order matters and is load bearing: the owner's PTE is cleared, THEN the
 *  MMU is told (so it can drop TLB entries and cache lines naming this
 *  frame), and only then is the frame released for reuse.  A surviving TLB
 *  entry would hand out the frame number again; a surviving cache line is
 *  physically tagged and would serve the previous page's bytes.
 * --------------------------------------------------------------------- */

static void evict_frame(MM *mm, uint32_t f, WriteBuffer *wb,
                        uint32_t *out_frame, MMFault *info)
{
    FrameDesc *fd = &mm->frames[f];
    uint16_t   pid;
    uint8_t    vpn;
    Process   *ow;
    WBEntry    e;
    int        guard = WB_ENTRIES + 1;  /* the buffer holds at most this many */

    /* The write buffer is DRAINED, never discarded.  A queued store to this
     * frame has not reached memory yet, and dropping it would lose a write the
     * program already performed.  Entries cannot be plucked from the middle
     * without breaking FIFO order, so drain from the HEAD until nothing names
     * this frame -- which also flushes the stores queued ahead of it, exactly
     * as the hardware would.  This precedes the write-back test below, because
     * a drained store can dirty the frame. */
    while (wb && wb_probe_frame(wb, f) >= 0 && guard-- > 0) {
        uint32_t pa, df;

        if (!wb_drain_head(wb, &e))
            break;

        pa = WB_TO_PA(e.block_addr);
        df = (uint32_t)PA_FRAME(pa);
        /* The store lands in memory now, so its page is dirty versus disk --
         * including pages queued ahead of the one being taken. */
        if (df < NUM_FRAMES && mm->frames[df].kind == FRAME_DATA) {
            Process *dow = proc_of(mm, mm->frames[df].pid);
            if (dow && dow->pt) {
                PTE *dpte = &dow->pt->entries[mm->frames[df].vpn];
                if (dpte->present && dpte->frame == df)
                    dpte->dirty = 1;
            }
        }

    }

    pid = fd->pid;
    vpn = fd->vpn;
    ow  = proc_of(mm, pid);

    if (ow && ow->pt) {
        PTE *pte = &ow->pt->entries[vpn];

        if (pte->present && pte->frame == f) {
            if (pte->dirty && info)
                info->wrote_back = 1;       /* flushed before reuse */

            pte->present    = 0;
            pte->frame      = 0;
            pte->referenced = 0;
            pte->dirty      = 0;

            if (ow->frames_held)
                ow->frames_held--;
        }
    }

    /* Handed back so the caller can scrub the TLB, L1 and L2. */
    if (out_frame)
        *out_frame = f;

    if (info)
        info->evicted = 1;
    frame_push(mm, f);
}

/* ------------------------------------------------------------------------
 *  Page fault -- the complete path.
 *
 *    1. already present               -> spurious fault, nothing to do
 *    2. process is at its upper_limit -> it evicts one of ITS OWN pages
 *    3. a free frame exists           -> take it
 *    4. otherwise                     -> global victim, evict, retry once
 * --------------------------------------------------------------------- */

int mm_handle_fault(MM *mm, Process *proc, uint8_t vpn,
                    WriteBuffer *wb, uint32_t *out_frame, MMFault *info)
{
    PTE *pte;
    int  f;

    if (out_frame)
        *out_frame = MM_NO_FRAME;
    if (info)
        memset(info, 0, sizeof(*info));

    if (!mm || !proc || !proc->pt)
        return -1;

    pte = &proc->pt->entries[vpn];

    /* 1. Another path already brought it in.  Treat as success rather than
     *    allocating a second frame for the same page. */
    if (pte->present)
        return 0;


    /* 2. At the cap: the process may not grow, so it displaces itself.  This
     *    is local replacement forced by the upper limit, inside an otherwise
     *    global policy.  The faulting page is not resident, so it owns no
     *    frame and cannot be chosen as its own victim. */
    if (proc->frames_held >= proc->upper_limit) {
        int v = pick_victim(mm, (int)proc->pid);
        if (v < 0)
            return -1;                  /* cap leaves no evictable page */
        evict_frame(mm, (uint32_t)v, wb, out_frame, info);
    }

    /* 3. A free frame. */
    f = mm_alloc_frame(mm, proc->pid, vpn, FRAME_DATA);

    /* 4. Otherwise evict globally and retry exactly once.  A second failure
     *    means every frame is a page table or is floor-protected: genuine
     *    exhaustion, not bad luck. */
    if (f < 0) {
        int v = mm_select_victim(mm);
        if (v < 0)
            return -1;
        evict_frame(mm, (uint32_t)v, wb, out_frame, info);

        f = mm_alloc_frame(mm, proc->pid, vpn, FRAME_DATA);
        if (f < 0)
            return -1;
    }

    if (info) info->disk_read = 1;      /* the page arrives from disk */

    pte->present    = 1;
    pte->frame      = (uint32_t)f;
    pte->referenced = 1;                /* the walker just brought it in */
    pte->dirty      = 0;                /* fresh from disk */
    proc->frames_held++;

    return 0;
}

/* ------------------------------------------------------------------------
 *  Processes
 * --------------------------------------------------------------------- */

int mm_create_process(MM *mm, Process *proc, uint16_t pid,
                      uint32_t lower_limit, uint32_t upper_limit)
{
    uint32_t v;
    int      f;

    if (!mm || !proc)
        return -1;

    /* A new process needs its page table plus two pre-paged pages, all FREE.
     * Demanding them up front is what lets pre-paging skip eviction entirely,
     * which is why this needs no write buffer and reports no victim.  Failing
     * here means too many processes for this memory: out of memory, and the
     * caller should stop the simulation. */
    if (mm->free_count < MIN_FRAMES_PER_PROC)
        return -1;

    memset(proc, 0, sizeof(*proc));
    proc->pid = pid;

    proc->lower_limit = (lower_limit < MIN_FRAMES_PER_PROC)
                      ? MIN_FRAMES_PER_PROC : lower_limit;
    proc->upper_limit = (upper_limit == 0 || upper_limit > PAGES_PER_PROC)
                      ? PAGES_PER_PROC : upper_limit;
    if (proc->upper_limit < proc->lower_limit)
        proc->upper_limit = proc->lower_limit;      /* a cap below the floor
                                                       is unsatisfiable */

    /* The page table's CONTENTS live in a normal allocation -- there is no
     * byte array to put them in -- but the FRAME it occupies is real. */
    proc->pt = (PageTable *)calloc(1, sizeof(PageTable));
    if (!proc->pt)
        return -1;

    f = mm_alloc_frame(mm, pid, 0, FRAME_PGTBL);
    if (f < 0) {
        free(proc->pt);
        proc->pt = NULL;
        return -1;
    }
    proc->pt_frame    = (uint32_t)f;
    proc->frames_held = 1;              /* the page table counts as a frame */

    /* Default protection for the whole space.  A real loader would set these
     * per segment from the executable header. */
    for (v = 0; v < PAGES_PER_PROC; v++)
        proc->pt->entries[v].prot = PROT_READ | PROT_WRITE | PROT_EXEC;

    /* Pre-page the first two pages.  These faults are planned, not demand
     * faults, so the counter is restored rather than decremented -- the pages
     * may already be present, in which case no fault occurred at all. */
    for (v = 0; v < 2; v++) {
        /* Frames are guaranteed free above, so these never evict. */
        if (mm_handle_fault(mm, proc, (uint8_t)v, NULL, NULL, NULL) != 0) {
            free(proc->pt);
            proc->pt = NULL;
            return -1;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------------
 *  Data access -- no bytes move, only the frame's metadata and the counters.
 * --------------------------------------------------------------------- */

/* Traffic counters only.  Reference and dirty state belong to the PTE and are
 * written by the MMU during translation, not by the data access -- a cache hit
 * never reaches here, and must not affect replacement either way. */
void mm_read_block(MM *mm, uint32_t pa)
{
    (void)pa;
    if (!mm) return;
}

void mm_write(MM *mm, uint32_t pa)
{
    (void)pa;
    if (!mm) return;
}

/* ------------------------------------------------------------------------ */

void mm_dump(const MM *mm)
{
    uint32_t f, data = 0, pgtbl = 0;

    if (!mm) return;

    for (f = 0; f < NUM_FRAMES; f++) {
        if (mm->frames[f].kind == FRAME_DATA)  data++;
        else if (mm->frames[f].kind == FRAME_PGTBL) pgtbl++;
    }

    printf("\n--- Main memory (%u frames of %d B, global LFU+aging) ---\n",
           (unsigned)NUM_FRAMES, PAGE_SIZE);
    printf("  free %u | data %u | page-table %u\n",
           (unsigned)mm->free_count, (unsigned)data, (unsigned)pgtbl);
}
