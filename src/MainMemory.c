/* ============================================================================
 *  src/MainMemory.c -- 32 MB main memory, pure paging, LFU-with-aging
 *
 *  Implements every function declared in include/MainMemory.h.  That header
 *  is NOT modified by this file.
 *
 *  Model
 *  -----
 *  Memory is one flat malloc'd array of MM_SIZE bytes.  Frame f owns the
 *  bytes [f * PAGE_SIZE, (f+1) * PAGE_SIZE).  A physical address is used
 *  directly as an index into it, so PA -> byte is exact, not simulated.
 *
 *  A process's page table is 256 PTEs x 4 B = 1024 B = exactly one frame,
 *  so it is stored IN main memory like everything else: Process.pt points
 *  into mm->storage at Process.pt_frame * PAGE_SIZE.  Page-table frames are
 *  marked is_page_table and are never chosen as replacement victims -- a
 *  page table that could be paged out would need a page table to find it.
 *
 *  Replacement -- LFU with aging, GLOBAL
 *  ------------------------------------
 *  Two counters per page, and they answer different questions:
 *
 *      PTE.aging   8-bit shift register.  Every mm_age_tick() shifts it
 *                  right and drops the reference bit into the MSB.  Recent
 *                  use dominates; old use decays exponentially.  This is
 *                  the PRIMARY key.
 *      FrameDesc.freq  raw access count, HALVED at each tick so it cannot
 *                  grow without bound.  Used only to break ties between
 *                  pages with identical aging history.
 *
 *  FrameDesc.aging mirrors PTE.aging so that victim selection can sweep the
 *  frame table linearly without chasing a page-table pointer per frame.
 *  mm_age_tick() is the single place that writes both, so they cannot drift.
 *
 *  Global means the victim may belong to any process.  The per-process
 *  lower_limit is enforced here: a process already at its floor is skipped,
 *  which is what stops global replacement from squeezing a process into
 *  thrashing.  upper_limit is enforced in mm_handle_fault, which is the only
 *  function that grows a resident set.
 *
 *  Cache and TLB invalidation
 *  --------------------------
 *  When a frame is reclaimed, any L1/L2 line or TLB entry still referring to
 *  it becomes a landmine: the caches are physically tagged, so once the frame
 *  holds a different page those lines would return the OLD page's bytes.
 *  MainMemory cannot include TLB.h/L1.h/L2.h without creating a dependency
 *  cycle, so it publishes the event instead -- see mm_set_evict_notifier()
 *  in include/translate.h.  The MMU registers a callback and performs the
 *  invalidations.
 *
 *  Disk
 *  ----
 *  There is no real backing store.  disk_read_page() synthesises a
 *  deterministic, verifiable pattern from (pid, vpn) so tests can prove that
 *  the right page landed in the right frame; disk_write_page() only counts.
 *  Swap both for real file I/O and nothing above them changes.
 *
 *  NOTE ON SIZE: sizeof(MainMemory) is about 512 KB because FrameDesc
 *  frames[32768] is embedded in it.  Allocate it with malloc or declare it
 *  static -- a plain local will blow the default 8 MB stack on some systems
 *  and is well past the 1 MB default on others.
 * ==========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MainMemory.h"
#include "translate.h"          /* only for mm_set_evict_notifier's prototype */

/* The page table must occupy exactly one frame, or the "page tables live in
 * main memory" model quietly breaks.  Fail the build, not a test run. */
typedef char pt_fits_one_frame[(sizeof(PageTable) == PAGE_SIZE) ? 1 : -1];

/* Every vpn parameter here is uint8_t, which covers exactly 0..255 -- the
 * whole 8-bit page number space.  Range checks on vpn are therefore
 * structurally unnecessary and are omitted rather than written as always-true
 * comparisons.  If VPN_BITS ever grows, this assert fires and every uint8_t
 * vpn must widen with it. */
typedef char vpn_fits_uint8[(PAGES_PER_PROC == 256 && VPN_BITS == 8) ? 1 : -1];

#define NO_FRAME    ((uint32_t)-1)

/* ------------------------------------------------------------------------
 *  Eviction notification
 *
 *  File-scope because MainMemory.h has no field to hold it and this file
 *  does not modify that header.  There is one main memory in the simulator,
 *  so a single hook is sufficient.  If MainMemory.h ever becomes editable,
 *  move these two values into struct MainMemory and delete these statics.
 * --------------------------------------------------------------------- */
static MMEvictNotifyFn g_evict_fn  = NULL;
static void           *g_evict_ctx = NULL;

void mm_set_evict_notifier(MMEvictNotifyFn fn, void *ctx)
{
    g_evict_fn  = fn;
    g_evict_ctx = ctx;
}

static void notify_evict(uint32_t frame, uint16_t pid, uint32_t vpn)
{
    if (g_evict_fn) g_evict_fn(g_evict_ctx, frame, pid, vpn);
}

/* ------------------------------------------------------------------------
 *  The process table
 *
 *  mm_handle_fault's signature gives it only the FAULTING process, but
 *  replacement here is GLOBAL: the victim may belong to anyone, and two
 *  things then require the whole table --
 *
 *      1. skipping processes that are already at their lower_limit, and
 *      2. clearing the VICTIM OWNER's PTE, not the faulting process's.
 *
 *  Miss (2) and the owner keeps a present PTE pointing at a frame that now
 *  holds someone else's page, which is silent, total data corruption.  So
 *  the table is registered once at start-up; mmu_init() does it.  Without
 *  it this file falls back to the single process it was handed, which is
 *  correct only in a single-process run.
 * --------------------------------------------------------------------- */
static Process *g_procs     = NULL;
static uint16_t g_num_procs = 0;

void mm_set_process_table(Process *procs, uint16_t num_procs)
{
    g_procs     = procs;
    g_num_procs = num_procs;
}

/* Prefer the registered table; fall back to the caller's single process. */
static Process *os_table(Process *fallback, uint16_t *n_out)
{
    if (g_procs && g_num_procs) { *n_out = g_num_procs; return g_procs; }
    *n_out = (uint16_t)(fallback ? 1 : 0);
    return fallback;
}

/* ------------------------------------------------------------------------
 *  Simulated backing store
 * --------------------------------------------------------------------- */

static void disk_read_page(uint16_t pid, uint8_t vpn, uint8_t *dst)
{
    /* Deterministic and collision-free per (pid, vpn) for the first bytes,
     * so a test can assert that frame contents match the page requested. */
    uint32_t i;
    dst[0] = (uint8_t)(pid & 0xFF);
    dst[1] = (uint8_t)(pid >> 8);
    dst[2] = vpn;
    dst[3] = 0x5A;
    for (i = 4; i < PAGE_SIZE; i++)
        dst[i] = (uint8_t)((pid * 31u + vpn * 7u + i) & 0xFF);
}

static void disk_write_page(uint16_t pid, uint8_t vpn, const uint8_t *src)
{
    (void)pid; (void)vpn; (void)src;    /* counted by the caller */
}

/* ------------------------------------------------------------------------
 *  Small helpers
 * --------------------------------------------------------------------- */

static uint8_t *frame_bytes(MainMemory *mm, uint32_t frame)
{
    return mm->storage + (size_t)frame * PAGE_SIZE;
}

/* Locate the live process that owns a frame.  Returns NULL when the frame is
 * unowned or its owner has exited without the frame being released -- such a
 * frame is garbage and is freely evictable. */
static const Process *owner_of(const FrameDesc *fd,
                               const Process *procs, uint16_t num_procs)
{
    uint16_t i;
    if (!procs) return NULL;
    for (i = 0; i < num_procs; i++)
        if (procs[i].active && procs[i].pid == fd->pid)
            return &procs[i];
    return NULL;
}

/* ------------------------------------------------------------------------
 *  Lifecycle
 * --------------------------------------------------------------------- */

int mm_init(MainMemory *mm)
{
    if (!mm) return -1;

    memset(mm, 0, sizeof(*mm));

    mm->storage = (uint8_t *)calloc(MM_SIZE, 1);
    if (!mm->storage)
        return -1;                       /* 32 MB request refused */

    mm->free_frames = NUM_FRAMES;
    return 0;
}

void mm_destroy(MainMemory *mm)
{
    if (!mm) return;
    free(mm->storage);
    mm->storage     = NULL;
    mm->free_frames = 0;
    memset(mm->frames, 0, sizeof(mm->frames));
}

void mm_reset_stats(MainMemory *mm)
{
    if (!mm) return;
    mm->page_faults     = 0;
    mm->disk_reads      = 0;
    mm->disk_writebacks = 0;
    mm->writes          = 0;
    mm->block_fetches   = 0;
}

/* ------------------------------------------------------------------------
 *  Frame allocation
 *
 *  Allocates a FREE frame only.  Returns -1 when memory is full; making a
 *  frame free is mm_select_victim's job plus the eviction in
 *  mm_handle_fault, so that policy stays in one place.
 *
 *  The rotating hint keeps a full sweep from starting at frame 0 every time,
 *  which matters at 32768 frames.
 * --------------------------------------------------------------------- */
int mm_alloc_frame(MainMemory *mm, uint16_t pid, uint8_t vpn, int is_page_table)
{
    static uint32_t hint = 0;
    uint32_t scanned;

    if (!mm || !mm->storage || mm->free_frames == 0)
        return -1;

    for (scanned = 0; scanned < NUM_FRAMES; scanned++) {
        uint32_t f = (hint + scanned) % NUM_FRAMES;
        FrameDesc *fd = &mm->frames[f];

        if (fd->allocated) continue;

        fd->allocated     = 1;
        fd->pid           = pid;
        fd->vpn           = vpn;
        fd->is_page_table = (uint8_t)(is_page_table ? 1 : 0);
        fd->freq          = 0;
        fd->aging         = 0;

        memset(frame_bytes(mm, f), 0, PAGE_SIZE);   /* never leak the last
                                                       tenant's bytes */
        mm->free_frames--;
        hint = (f + 1) % NUM_FRAMES;
        return (int)f;
    }

    return -1;                          /* free_frames disagreed with reality */
}

/* Release a frame without touching any page table.  Internal: callers are
 * responsible for the PTE and for the invalidation notification. */
static void frame_release(MainMemory *mm, uint32_t f)
{
    memset(&mm->frames[f], 0, sizeof(FrameDesc));
    mm->free_frames++;
}

/* ------------------------------------------------------------------------
 *  Victim selection -- global LFU with aging
 *
 *  Skipped, in order of importance:
 *    - unallocated frames    (nothing to evict; caller should allocate)
 *    - page-table frames     (paging one out is unrecoverable)
 *    - frames whose owner is already at its lower_limit
 *
 *  Ranking: smallest aging register wins.  Ties break on smallest freq, then
 *  on lowest frame number so the choice is deterministic and reproducible
 *  across runs -- important for a simulator you have to defend.
 * --------------------------------------------------------------------- */
int mm_select_victim(MainMemory *mm, const Process *procs, uint16_t num_procs)
{
    uint32_t f;
    uint32_t best      = NO_FRAME;
    uint8_t  best_age  = 0;
    uint32_t best_freq = 0;

    if (!mm || !mm->storage) return -1;

    for (f = 0; f < NUM_FRAMES; f++) {
        const FrameDesc *fd = &mm->frames[f];
        const Process   *ow;

        if (!fd->allocated)     continue;
        if (fd->is_page_table)  continue;

        ow = owner_of(fd, procs, num_procs);
        if (ow && ow->frames_held <= ow->lower_limit)
            continue;                   /* at its floor -- protected */

        if (best == NO_FRAME ||
            fd->aging < best_age ||
            (fd->aging == best_age && fd->freq < best_freq)) {
            best      = f;
            best_age  = fd->aging;
            best_freq = fd->freq;
        }
    }

    return (best == NO_FRAME) ? -1 : (int)best;
}

/* ------------------------------------------------------------------------
 *  Aging tick
 *
 *  Called by the replacement timer, NOT on every access -- that is the whole
 *  point of aging: the reference bit accumulates cheaply between ticks and
 *  is sampled once per interval.
 * --------------------------------------------------------------------- */
void mm_age_tick(MainMemory *mm, Process *procs, uint16_t num_procs)
{
    uint16_t p;
    uint32_t v;

    if (!mm || !procs) return;

    for (p = 0; p < num_procs; p++) {
        Process *proc = &procs[p];
        if (!proc->active || !proc->pt) continue;

        for (v = 0; v < PAGES_PER_PROC; v++) {
            PTE *pte = &proc->pt->entries[v];
            uint32_t f;

            if (!pte->present) continue;

            /* shift right, reference bit into the MSB, then clear it */
            pte->aging      = (uint8_t)((pte->aging >> 1) |
                                        (pte->referenced ? 0x80u : 0x00u));
            pte->referenced = 0;

            f = pte->frame;
            if (f < NUM_FRAMES && mm->frames[f].allocated) {
                mm->frames[f].aging = (uint8_t)pte->aging;   /* keep in step */
                mm->frames[f].freq >>= 1;                    /* decay the LFU
                                                                count too    */
            }
        }
    }
}

/* ------------------------------------------------------------------------
 *  Pre-paging
 *
 *  Brings a process to the point where it can execute its first instruction:
 *  a resident page table plus its first 2 pages.  That is where
 *  MIN_FRAMES_PER_PROC = 3 comes from.
 *
 *  Returns 0 on success, -1 if memory could not supply the frames.
 * --------------------------------------------------------------------- */
int mm_prepage(MainMemory *mm, Process *proc)
{
    uint8_t v;

    if (!mm || !mm->storage || !proc) return -1;

    /* 1. the page table itself, if it is not resident yet */
    if (!proc->pt) {
        int f = mm_alloc_frame(mm, proc->pid, 0, 1 /* is_page_table */);
        if (f < 0) return -1;

        proc->pt_frame = (uint16_t)f;
        proc->pt       = (PageTable *)frame_bytes(mm, (uint32_t)f);
        memset(proc->pt, 0, sizeof(PageTable));      /* all PTEs not present */

        /* Default protection for the whole address space.  A real loader
         * would set these per segment from the executable header. */
        for (v = 0; v < PAGES_PER_PROC - 1; v++)
            proc->pt->entries[v].prot = PROT_READ | PROT_WRITE | PROT_EXEC;
        proc->pt->entries[PAGES_PER_PROC - 1].prot =
            PROT_READ | PROT_WRITE | PROT_EXEC;
    }

    if (proc->lower_limit < MIN_FRAMES_PER_PROC)
        proc->lower_limit = MIN_FRAMES_PER_PROC;
    if (proc->upper_limit == 0 || proc->upper_limit > PAGES_PER_PROC)
        proc->upper_limit = PAGES_PER_PROC;

    proc->active = 1;

    /* 2. the first two pages, per the pre-paging requirement.  These are
     *    loaded into MAIN MEMORY only -- not into L1 or L2 -- so the very
     *    first instruction fetch is still a guaranteed cache miss. */
    for (v = 0; v < 2; v++)
        if (mm_handle_fault(mm, proc, v) != 0)
            return -1;

    /* Pre-paging is not demand paging: these two faults were planned, so do
     * not let them inflate the demand-fault statistic. */
    if (mm->page_faults >= 2) mm->page_faults -= 2;

    return 0;
}

/* ------------------------------------------------------------------------
 *  Page fault handling -- the complete path
 *
 *  Returns 0 on success (the page is resident and proc->pt is updated),
 *         -1 if no frame could be obtained.
 *
 *  Order of preference for a frame:
 *    1. already present               -> spurious fault, nothing to do
 *    2. process is at its upper_limit -> evict one of ITS OWN pages
 *    3. a free frame exists           -> take it
 *    4. otherwise                     -> global LFU-with-aging victim
 * --------------------------------------------------------------------- */

/* Evict frame f: write back if dirty, clear the owner's PTE, tell the MMU to
 * invalidate caches and TLB, then free the frame. */
static void evict_frame(MainMemory *mm, uint32_t f, Process *procs,
                        uint16_t num_procs)
{
    FrameDesc *fd = &mm->frames[f];
    uint16_t   pid = fd->pid;
    uint8_t    vpn = fd->vpn;
    Process   *ow  = NULL;
    uint16_t   i;

    for (i = 0; procs && i < num_procs; i++)
        if (procs[i].active && procs[i].pid == pid) { ow = &procs[i]; break; }

    if (ow && ow->pt) {
        PTE *pte = &ow->pt->entries[vpn];

        if (pte->present && pte->frame == f) {
            if (pte->dirty) {
                disk_write_page(pid, vpn, frame_bytes(mm, f));
                mm->disk_writebacks++;
            }
            pte->present    = 0;
            pte->frame      = 0;
            pte->dirty      = 0;
            pte->referenced = 0;
            pte->aging      = 0;

            if (ow->frames_held) ow->frames_held--;
        }
    }

    /* MUST happen before the frame is handed to anyone else: the caches are
     * physically tagged, so a surviving line would serve the old page. */
    notify_evict(f, pid, vpn);

    frame_release(mm, f);
}

int mm_handle_fault(MainMemory *mm, Process *proc, uint8_t vpn)
{
    PTE *pte;
    int  f;

    if (!mm || !mm->storage || !proc || !proc->pt) return -1;
    /* vpn is uint8_t: 0..255 is the entire page-number space, always valid */

    pte = &proc->pt->entries[vpn];

    /* 1. Spurious fault: another path already brought the page in.  Real
     *    systems hit this after a TLB shootdown or a racing fault; treat it
     *    as success rather than double-allocating a frame. */
    if (pte->present)
        return 0;

    mm->page_faults++;

    /* 2. At the cap: the process may not grow, so it evicts its own page.
     *    This is local replacement forced by the upper limit, inside an
     *    otherwise global policy. */
    if (proc->upper_limit && proc->frames_held >= proc->upper_limit) {
        uint32_t v, victim = NO_FRAME;
        uint8_t  best_age = 0;
        uint32_t best_freq = 0;

        for (v = 0; v < PAGES_PER_PROC; v++) {
            PTE *cand = &proc->pt->entries[v];
            FrameDesc *fd;

            if (!cand->present || v == vpn) continue;
            fd = &mm->frames[cand->frame];
            if (fd->is_page_table) continue;

            if (victim == NO_FRAME ||
                fd->aging < best_age ||
                (fd->aging == best_age && fd->freq < best_freq)) {
                victim    = cand->frame;
                best_age  = fd->aging;
                best_freq = fd->freq;
            }
        }
        if (victim == NO_FRAME)
            return -1;                  /* cap set below 1 resident page */

        /* This victim is provably the faulting process's own page, so the
         * single-process form is correct here. */
        evict_frame(mm, victim, proc, 1);
    }

    /* 3. Take a free frame if there is one. */
    f = mm_alloc_frame(mm, proc->pid, vpn, 0);

    /* 4. Otherwise evict globally and retry exactly once.  A second failure
     *    means every frame is either a page table or protected by a
     *    lower_limit -- genuine exhaustion, not bad luck. */
    if (f < 0) {
        uint16_t  n;
        Process  *tbl    = os_table(proc, &n);
        int       victim = mm_select_victim(mm, tbl, n);

        if (victim < 0)
            return -1;                  /* every frame is a page table or is
                                           protected by a lower_limit */

        evict_frame(mm, (uint32_t)victim, tbl, n);
        f = mm_alloc_frame(mm, proc->pid, vpn, 0);
        if (f < 0)
            return -1;
    }

    /* 5. Fetch the page and publish the mapping. */
    disk_read_page(proc->pid, vpn, frame_bytes(mm, (uint32_t)f));
    mm->disk_reads++;

    pte->present    = 1;
    pte->frame      = (uint32_t)f & (uint32_t)MASK(FRAME_BITS);
    pte->dirty      = 0;
    pte->referenced = 1;
    pte->aging      = 0x80;             /* just touched: MSB set */

    mm->frames[f].aging = 0x80;
    mm->frames[f].freq  = 1;

    proc->frames_held++;
    return 0;
}

/* ------------------------------------------------------------------------
 *  Data access
 * --------------------------------------------------------------------- */

void mm_read_block(MainMemory *mm, uint32_t pa, uint8_t *out_block)
{
    uint32_t base;

    if (!mm || !mm->storage || !out_block) return;

    base = BLOCK_BASE(pa) & (uint32_t)MASK(PA_BITS);
    if ((size_t)base + BLOCK_SIZE > MM_SIZE) return;    /* out of range */

    memcpy(out_block, mm->storage + base, BLOCK_SIZE);
    mm->block_fetches++;
}

void mm_write(MainMemory *mm, uint32_t pa, const uint8_t *bytes, uint32_t len)
{
    uint32_t addr;

    if (!mm || !mm->storage || !bytes || len == 0) return;

    addr = pa & (uint32_t)MASK(PA_BITS);
    if ((size_t)addr + len > MM_SIZE) return;

    memcpy(mm->storage + addr, bytes, len);
    mm->writes++;                       /* one write-through arrival from L2 */
}

/* Set the PTE dirty bit for the page containing pa.  The frame table gives
 * the reverse mapping frame -> vpn, so no page-table scan is needed. */
void mm_mark_dirty(MainMemory *mm, Process *proc, uint32_t pa)
{
    uint32_t f;
    uint8_t  vpn;

    if (!mm || !proc || !proc->pt) return;

    f = (uint32_t)PA_FRAME(pa);
    if (f >= NUM_FRAMES || !mm->frames[f].allocated) return;
    if (mm->frames[f].pid != proc->pid)              return;  /* not ours */

    vpn = mm->frames[f].vpn;

    if (proc->pt->entries[vpn].present && proc->pt->entries[vpn].frame == f) {
        proc->pt->entries[vpn].dirty      = 1;
        proc->pt->entries[vpn].referenced = 1;
        if (mm->frames[f].freq != 0xFFFFFFFFu) mm->frames[f].freq++;
    }
}
