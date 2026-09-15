/* ============================================================================
 *  src/new_main.c -- driver.
 *
 *      ./sim_q3 <num_processes> <accesses_per_context_switch> <trace file>
 *
 *  ONE address stream, time sliced across N processes.  The trace is walked
 *  once from start to finish; every <accesses_per_context_switch> accesses the
 *  running PID advances, so each process sees a different slice of it through
 *  its own address space and page table.
 *
 *  Each trace line is a bare hex virtual address.  The traces carry no
 *  read/write information, so the access type is synthesised deterministically
 *  at WRITE_PERCENT.
 *
 *  The driver owns the ORDER of operations.  Each module does one thing and
 *  hands back what the next one needs:
 *
 *      va_to_pa()          translate; faults the page in; scrubs the TLB of a
 *                          reclaimed frame and REPORTS that frame
 *        -> driver         scrubs L1 and L2 of it (physically tagged)
 *      cache_read/write()  the access; reports a displaced write-buffer entry
 *        -> driver         sends that entry, and any unbuffered store, to MM
 *      mm_age_tick()       the OS sampling pass: reads Accessed bits, clears
 *                          them, and shoots down the matching TLB entries
 * ==========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "MM.h"
#include "mmu.h"
#include "cache.h"

/* ---- tuning constants -- edit here and rebuild -------------------------- */
#define WRITE_PERCENT      30                   /* 70% reads / 30% writes    */
#define AGE_TICK_INTERVAL  1000                 /* accesses per aging pass   */
#define WB_DRAIN_INTERVAL  10                   /* accesses per buffer drain */
#define MAX_PROCS          20                   /* cap on the CLI argument   */

/* Frames a process may hold, its page table included.  Lower the cap to put
 * main memory under pressure: at the full 256 these traces never fill the
 * 32768 frames, so no page is ever evicted and replacement never runs. */
#define PROC_LOWER_LIMIT   MIN_FRAMES_PER_PROC  /* 3   */
#define PROC_UPPER_LIMIT   PAGES_PER_PROC       /* 256 */

/* ---- the machine ------------------------------------------------------- */
static MM          mm;                  /* ~320 KB -- never a stack local */
static Process     procs[MAX_PROCS];
static TLB         tlb;
static L1Cache     l1;
static L2Cache     l2;
static WriteBuffer wb;

/* ---- statistics ---------------------------------------------------------
 * Every counter lives here.  No module keeps statistics of its own: they
 * report what happened through return values, and the driver decides what is
 * worth counting. */
static struct {
    uint64_t accesses, reads, writes, switches, ticks;

    uint64_t tlb_hits, tlb_misses, tlb_evictions;

    uint64_t l1_read_hits, l1_read_misses;
    uint64_t l1_write_hits, l1_write_misses, l1_evictions;

    uint64_t l2_hits, l2_misses, l2_promotions, l2_evictions;
    uint64_t l2_updated_writes, l2_passthrough_writes;

    uint64_t wb_enqueued, wb_drains, wb_stalls, wb_forwards;

    uint64_t page_faults, disk_reads, disk_writebacks, mm_evictions;
    uint64_t mm_writes, mm_block_fetches;
} st;

/* Deterministic read/write split: the same trace always produces the same
 * sequence, which is what makes a run reproducible. */
static uint32_t rng = 12345u;
static int is_write(void)
{
    rng = rng * 1103515245u + 12345u;
    return (int)((rng >> 16) % 100u) < WRITE_PERCENT;
}

/* --------------------------------------------------------------------------
 *  One access.  Returns 0, or -1 when main memory is exhausted.
 * ----------------------------------------------------------------------- */
static int do_access(Process *proc, uint32_t va, AccessType acc)
{
    uint32_t    pa, evicted;
    WBEntry     drained;
    XlateResult x;

    st.accesses++;
    if (acc == ACC_WRITE) st.writes++; else st.reads++;

    //virtual address to physical address
    x = va_to_pa(&tlb, &mm, proc, va, acc, &wb, &pa, &evicted);
    if (x & XLATE_OOM)
        return -1;

    if (x & XLATE_TLB_HIT) st.tlb_hits++; else st.tlb_misses++;
    if (x & XLATE_TLB_EVICT)  st.tlb_evictions++;
    if (x & XLATE_FAULT)      st.page_faults++;
    if (x & XLATE_DISK_READ)  st.disk_reads++;
    if (x & XLATE_WROTE_BACK) st.disk_writebacks++;
    if (x & XLATE_EVICTED)    st.mm_evictions++;

    /* --- 2. finish the eviction ------------------------------------------
     * L1 and L2 are PHYSICALLY tagged: lines of a reclaimed frame would serve
     * the previous page's data once the frame is refilled. */
    if (evicted != MM_NO_FRAME) {
        l1_invalidate_frame(&l1, evicted);
        l2_invalidate_frame(&l2, evicted);
    }

    /* --- 3. the access itself -------------------------------------------- */
    if (acc == ACC_WRITE) {
        CacheSearchResult r = cache_write(&l1, &l2, &wb, pa, &drained);

        if (r == CACHE_HIT_L1) st.l1_write_hits++; else st.l1_write_misses++;

        if (r == CACHE_HIT_L1 || r == CACHE_HIT_WB) {
            st.wb_enqueued++;                 /* the store was buffered */
            if (drained.valid) {              /* it arrived at a full buffer */
                st.wb_stalls++;
                st.wb_drains++;
                mm_write(&mm, WB_TO_PA(drained.block_addr));
                st.mm_writes++;
            }
        } else {
            /* L2 has no write buffer of its own, so the store goes straight
             * to memory and the CPU stalls. */
            if (r == CACHE_HIT_L2) { st.l2_hits++;   st.l2_updated_writes++; }
            else                   { st.l2_misses++; st.l2_passthrough_writes++; }
            mm_write(&mm, pa);
            st.mm_writes++;
        }
    } else {
        CacheSearchResult r = cache_read(&l1, &l2, &wb, pa);

        if (r == CACHE_HIT_L1) st.l1_read_hits++; else st.l1_read_misses++;
        if (r == CACHE_HIT_WB) st.wb_forwards++;
        if (r == CACHE_HIT_L2) { st.l2_hits++; st.l2_promotions++; }

        if (r == CACHE_MISS) {
            /* L1 and L2 are EXCLUSIVE: the block goes to L1 only, and L1's
             * victim must be demoted into L2 or it is lost.  Evict first so
             * the victim is known. */
            uint32_t idx = (uint32_t)L1_INDEX(pa);
            int      way = l1_select_victim(&l1, idx);
            uint32_t victim_pa = 0;
            int      displaced = l1_evict(&l1, idx, way, &victim_pa);

            st.l2_misses++;
            mm_read_block(&mm, pa);
            st.mm_block_fetches++;

            l1_install(&l1, pa);
            if (displaced) {
                st.l1_evictions++;
                if (l2_allocate(&l2, victim_pa))
                    st.l2_evictions++;
            }
        }
    }

    /* --- 4. background write-buffer drain -------------------------------- */
    if (st.accesses % WB_DRAIN_INTERVAL == 0) {
        WBEntry bg;
        if (wb_drain_head(&wb, &bg)) {
            st.wb_drains++;
            mm_write(&mm, WB_TO_PA(bg.block_addr));
            st.mm_writes++;
        }
    }

    /* --- 5. the OS sampling pass ----------------------------------------- */
    if (st.accesses % AGE_TICK_INTERVAL == 0) {
        mm_age_tick(&mm, &tlb);
        st.ticks++;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 *  Reporting
 * ----------------------------------------------------------------------- */
static double pct(uint64_t part, uint64_t whole)
{
    return whole ? (100.0 * (double)part / (double)whole) : 0.0;
}

static void report(uint16_t nproc, uint64_t quantum, const char *trace)
{
    uint64_t l1_acc  = st.l1_read_hits + st.l1_read_misses
                     + st.l1_write_hits + st.l1_write_misses;
    uint64_t l1_hit  = st.l1_read_hits + st.l1_write_hits;
    uint64_t l2_acc  = st.l2_hits + st.l2_misses;
    uint64_t tlb_acc = st.tlb_hits + st.tlb_misses;

    printf("\n================ RUN ================\n");
    printf("  trace %s\n", trace);
    printf("  processes %u   switch every %llu accesses   %d%% writes\n",
           nproc, (unsigned long long)quantum, WRITE_PERCENT);
    printf("  accesses %llu   reads %llu (%.1f%%)   writes %llu (%.1f%%)\n",
           (unsigned long long)st.accesses,
           (unsigned long long)st.reads,  pct(st.reads,  st.accesses),
           (unsigned long long)st.writes, pct(st.writes, st.accesses));
    printf("  context switches %llu   aging ticks %llu\n",
           (unsigned long long)st.switches, (unsigned long long)st.ticks);

    printf("\n---------------- TLB (%d entries, PID tagged) ----------------\n", TLB_ENTRIES);
    printf("  hits %llu   misses %llu   hit rate %.2f%%   evictions %llu\n",
           (unsigned long long)st.tlb_hits, (unsigned long long)st.tlb_misses,
           pct(st.tlb_hits, tlb_acc), (unsigned long long)st.tlb_evictions);
    printf("  page-table walks %llu  (one main-memory access each)\n",
           (unsigned long long)st.tlb_misses);

    printf("\n---------------- L1 (4 KB, 16 B, 4-way, LRU) ----------------\n");
    printf("  read  hits %llu   misses %llu\n",
           (unsigned long long)st.l1_read_hits, (unsigned long long)st.l1_read_misses);
    printf("  write hits %llu   misses %llu   (no-write-allocate)\n",
           (unsigned long long)st.l1_write_hits, (unsigned long long)st.l1_write_misses);
    printf("  hit rate %.2f%%   evictions %llu\n",
           pct(l1_hit, l1_acc), (unsigned long long)st.l1_evictions);

    printf("\n---------------- L2 (32 KB, 16 B, 8-way, FIFO) ----------------\n");
    printf("  hits %llu   misses %llu   hit rate %.2f%%\n",
           (unsigned long long)st.l2_hits, (unsigned long long)st.l2_misses,
           pct(st.l2_hits, l2_acc));
    printf("  promotions to L1 %llu   evictions %llu   (exclusive)\n",
           (unsigned long long)st.l2_promotions, (unsigned long long)st.l2_evictions);
    printf("  writes updating a line %llu   passing through %llu\n",
           (unsigned long long)st.l2_updated_writes,
           (unsigned long long)st.l2_passthrough_writes);

    printf("\n---------------- Write buffer (%d blocks, FIFO) ----------------\n", WB_ENTRIES);
    printf("  stores queued %llu   drains %llu   full stalls %llu   forwards %llu\n",
           (unsigned long long)st.wb_enqueued, (unsigned long long)st.wb_drains,
           (unsigned long long)st.wb_stalls,   (unsigned long long)st.wb_forwards);

    printf("\n---------------- Main memory (32 MB, LFU+aging) ----------------\n");
    printf("  page faults %llu   disk reads %llu   writebacks %llu\n",
           (unsigned long long)st.page_faults, (unsigned long long)st.disk_reads,
           (unsigned long long)st.disk_writebacks);
    printf("  frame evictions %llu   free frames %u / %d\n",
           (unsigned long long)st.mm_evictions, mm.free_count, NUM_FRAMES);
    printf("  block fetches %llu   write arrivals %llu\n",
           (unsigned long long)st.mm_block_fetches, (unsigned long long)st.mm_writes);

    printf("\n  per process:  pid  frames held  limits\n");
    for (uint16_t i = 0; i < nproc; i++)
        printf("                %3u  %11u  %u..%u\n", procs[i].pid,
               procs[i].frames_held, procs[i].lower_limit, procs[i].upper_limit);

    if (st.mm_evictions == 0)
        printf("\n  NOTE: no frame was ever evicted -- %d frames is far more than these\n"
               "        traces touch, so page replacement never came under pressure.\n"
               "        Lower PROC_UPPER_LIMIT in new_main.c to force it.\n", NUM_FRAMES);
    printf("=====================================\n");
}

/* ------------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    const char *trace;
    FILE       *fp;
    uint16_t    nproc, cur;
    uint64_t    quantum, in_quantum;
    int         oom = 0;
    char        line[64];
    WBEntry     e;

    if (argc != 4) {
        fprintf(stderr,
                "usage: %s <num_processes> <accesses_per_context_switch> <trace file>\n"
                "   e.g. %s 5 10 traces/2026_27_ISEM_CC1.txt\n",
                argv[0], argv[0]);
        return 2;
    }

    nproc   = (uint16_t)strtoul(argv[1], NULL, 10);
    quantum = strtoull(argv[2], NULL, 10);
    trace   = argv[3];

    if (nproc < 1 || nproc > MAX_PROCS) {
        fprintf(stderr, "num_processes must be 1..%d\n", MAX_PROCS);
        return 2;
    }
    if (quantum < 1) {
        fprintf(stderr, "accesses_per_context_switch must be >= 1\n");
        return 2;
    }

    fp = fopen(trace, "r");
    if (!fp) {
        fprintf(stderr, "cannot open %s\n", trace);
        return 1;
    }

    /* --- bring the machine up ------------------------------------------ */
    mm_init(&mm, procs, nproc);         /* also zeroes the process table */
    tlb_init(&tlb);
    l1_init(&l1);
    l2_init(&l2);
    wb_init(&wb);

    for (uint16_t i = 0; i < nproc; i++) {
        /* A page table plus the first two pages, per the pre-paging rule --
         * into MAIN MEMORY only, never the caches. */
        if (mm_create_process(&mm, &procs[i], (uint16_t)(i + 1), PROC_LOWER_LIMIT, PROC_UPPER_LIMIT) != 0) 
        {
            printf("OUT OF MEMORY: could not seat process %u.  Too many processes for %d frames.\n", i + 1, NUM_FRAMES);
            fclose(fp);
            return 1;
        }
        printf("  process %u created  (page table pinned in frame %u, pages 0-1 pre-paged)\n", procs[i].pid, procs[i].pt_frame);
    }
    printf("trace %s, switching PID every %llu accesses\n\n", trace, (unsigned long long)quantum);

    /* --- walk the stream once, rotating the owning process -------------- */
    cur = 0;
    in_quantum = 0;

    while (fgets(line, sizeof line, fp)) {
        uint32_t   va  = (uint32_t)strtoul(line, NULL, 16);
        AccessType acc = is_write() ? ACC_WRITE : ACC_READ;

        if (do_access(&procs[cur], va, acc) != 0) 
        {
            printf("\nOUT OF MEMORY at access %llu: every frame is a page table or is protected by a process's lower limit.\n", (unsigned long long)st.accesses);
            oom = 1;
            break;
        }

        /* --- context switch ---------------------------------------------
         * The TLB is PID tagged, so a switch costs nothing: entries from
         * several processes coexist and none has to be flushed. */
        if (++in_quantum >= quantum) {
            in_quantum = 0;
            if (nproc > 1) 
            {
                cur = (uint16_t)((cur + 1) % nproc);
                st.switches++;
            }
        }
    }

    /* --- drain what the write buffer still holds ------------------------
     * Without this the last stores never reach memory. */
    while (wb_drain_head(&wb, &e)) 
    {
        st.wb_drains++;
        mm_write(&mm, WB_TO_PA(e.block_addr));
        st.mm_writes++;
    }

    report(nproc, quantum, trace);

    mm_destroy(&mm);            /* frees the page tables */
    fclose(fp);
    return oom ? 1 : 0;
}
