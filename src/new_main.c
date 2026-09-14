/* ============================================================================
 *  src/new_main.c -- driver.
 *
 *      ./sim_q3 <num_processes> <accesses_per_context_switch>
 *
 *  One benchmark trace per process, run round robin.  Each trace line is a
 *  bare hex virtual address; the traces carry no read/write information, so
 *  the access type is synthesised deterministically at WRITE_PERCENT.
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

/* ---- knobs -- override at compile time, e.g. -DPROC_UPPER_LIMIT=8 ------- */
#ifndef WRITE_PERCENT
#define WRITE_PERCENT      30      /* 70% reads / 30% writes                 */
#endif
#ifndef AGE_TICK_INTERVAL
#define AGE_TICK_INTERVAL  1000    /* accesses between OS sampling passes    */
#endif
#ifndef WB_DRAIN_INTERVAL
#define WB_DRAIN_INTERVAL  10      /* accesses between background WB drains  */
#endif
#ifndef PROC_UPPER_LIMIT
#define PROC_UPPER_LIMIT   0       /* 0 = no cap (PAGES_PER_PROC frames)     */
#endif
#ifndef PROC_LOWER_LIMIT
#define PROC_LOWER_LIMIT   0       /* 0 = MIN_FRAMES_PER_PROC                */
#endif
#define MAX_PROCS          5       /* one per benchmark trace                */

static const char *TRACE_FILES[MAX_PROCS] = {
    "traces/2026_27_ISEM_CC1.txt",
    "traces/2026_27_ISEM_LI.txt",
    "traces/2026_27_ISEM_APSI.txt",
    "traces/2026_27_ISEM_M88KSIM.txt",
    "traces/2026_27_ISEM_VORTEX.txt",
};

/* ---- the machine ------------------------------------------------------- */
static MM          mm;                  /* ~320 KB -- never a stack local */
static Process     procs[MAX_PROCS];
static TLB         tlb;
static L1Cache     l1;
static L2Cache     l2;
static WriteBuffer wb;

/* ---- driver-side statistics -------------------------------------------- */
static uint64_t n_access, n_read, n_write, n_switch, n_ticks;
static uint64_t n_l1_fill;              /* total misses that fetched a block */

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
    uint32_t pa, evicted;
    WBEntry  drained;

    n_access++;
    if (acc == ACC_WRITE) n_write++; else n_read++;

    /* --- 1. translate ---------------------------------------------------
     * Faults the page in if needed.  va_to_pa invalidates the TLB itself,
     * because it holds it; it cannot reach the caches. */
    if (va_to_pa(&tlb, &mm, proc, va, acc, &wb, &pa, &evicted) != 0)
        return -1;

    /* --- 2. finish the eviction ----------------------------------------
     * L1 and L2 are PHYSICALLY tagged: lines of a reclaimed frame would
     * serve the previous page's data once the frame is refilled. */
    if (evicted != MM_NO_FRAME) {
        l1_invalidate_frame(&l1, evicted);
        l2_invalidate_frame(&l2, evicted);
    }

    /* --- 3. the access itself ------------------------------------------ */
    if (acc == ACC_WRITE) {
        CacheSearchResult r = cache_write(&l1, &l2, &wb, pa, &drained);

        /* The buffer was full and its head was displaced to make room --
         * that store reaches memory now. */
        if (drained.valid)
            mm_write(&mm, WB_TO_PA(drained.block_addr));

        /* L2 has no write buffer of its own (Q3), so a store that missed L1
         * goes straight to memory and the CPU stalls. */
        if (r != CACHE_HIT_L1 && r != CACHE_HIT_WB)
            mm_write(&mm, pa);
    } else {
        CacheSearchResult r = cache_read(&l1, &l2, &wb, pa);

        if (r == CACHE_MISS) {
            /* Fetch the block and install it in L1.  L1 and L2 are EXCLUSIVE,
             * so the block goes to L1 only -- and L1's victim must be demoted
             * into L2, or it is lost.  Evict first so the victim is known. */
            uint32_t idx = (uint32_t)L1_INDEX(pa);
            int      way = l1_select_victim(&l1, idx);
            uint32_t victim_pa = 0;
            int      displaced = l1_evict(&l1, idx, way, &victim_pa);

            mm_read_block(&mm, pa);
            n_l1_fill++;

            l1_install(&l1, pa);
            if (displaced)
                l2_allocate(&l2, victim_pa);
        }
    }

    /* --- 4. background write-buffer drain ------------------------------
     * A real write buffer empties itself whenever the memory bus is idle; it
     * is a shock absorber, not a parking spot.  Draining only when a store
     * finds it full makes it permanently full, so nearly every store stalls.
     * One entry every WB_DRAIN_INTERVAL accesses stands in for that idle-cycle
     * drain, and it also stops an entry lingering long enough for its frame to
     * be reclaimed underneath it. */
    if (n_access % WB_DRAIN_INTERVAL == 0) {
        WBEntry bg;
        if (wb_drain_head(&wb, &bg)) {
            wb.drains++;
            mm_write(&mm, WB_TO_PA(bg.block_addr));
        }
    }

    /* --- 5. the OS sampling pass ---------------------------------------
     * Not per access -- that is the whole point of aging.  Clearing an
     * Accessed bit also shoots down its TLB entry, or the translation would
     * stay cached and the bit could never be set again. */
    if (n_access % AGE_TICK_INTERVAL == 0) {
        mm_age_tick(&mm, &tlb);
        n_ticks++;
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

static void report(uint16_t nproc, uint64_t quantum)
{
    uint64_t l1_acc = l1.read_hits + l1.read_misses + l1.write_hits + l1.write_misses;
    uint64_t l1_hit = l1.read_hits + l1.write_hits;
    uint64_t l2_acc = l2.hits + l2.misses;
    uint64_t tlb_acc = tlb.hits + tlb.misses;

    printf("\n================ RUN ================\n");
    printf("  processes %u   switch every %llu accesses   %d%% writes\n",
           nproc, (unsigned long long)quantum, WRITE_PERCENT);
    printf("  accesses %llu   reads %llu (%.1f%%)   writes %llu (%.1f%%)\n",
           (unsigned long long)n_access,
           (unsigned long long)n_read,  pct(n_read,  n_access),
           (unsigned long long)n_write, pct(n_write, n_access));
    printf("  context switches %llu   aging ticks %llu\n",
           (unsigned long long)n_switch, (unsigned long long)n_ticks);

    printf("\n---------------- TLB (%d entries, PID tagged) ----------------\n", TLB_ENTRIES);
    printf("  hits %llu   misses %llu   hit rate %.2f%%   evictions %llu\n",
           (unsigned long long)tlb.hits, (unsigned long long)tlb.misses,
           pct(tlb.hits, tlb_acc), (unsigned long long)tlb.evictions);
    printf("  page-table walks %llu  (one main-memory access each)\n",
           (unsigned long long)tlb.misses);

    printf("\n---------------- L1 (4 KB, 16 B, 4-way, LRU) ----------------\n");
    printf("  read  hits %llu   misses %llu\n",
           (unsigned long long)l1.read_hits, (unsigned long long)l1.read_misses);
    printf("  write hits %llu   misses %llu   (no-write-allocate)\n",
           (unsigned long long)l1.write_hits, (unsigned long long)l1.write_misses);
    printf("  hit rate %.2f%%   block fills %llu\n", pct(l1_hit, l1_acc),
           (unsigned long long)n_l1_fill);

    printf("\n---------------- L2 (32 KB, 16 B, 8-way, FIFO) ----------------\n");
    printf("  hits %llu   misses %llu   hit rate %.2f%%\n",
           (unsigned long long)l2.hits, (unsigned long long)l2.misses, pct(l2.hits, l2_acc));
    printf("  promotions to L1 %llu   evictions %llu   (exclusive)\n",
           (unsigned long long)l2.promotions, (unsigned long long)l2.evictions);
    printf("  writes updating a line %llu   passing through %llu\n",
           (unsigned long long)l2.updated_writes,
           (unsigned long long)l2.passthrough_writes);

    printf("\n---------------- Write buffer (%d blocks, FIFO) ----------------\n", WB_ENTRIES);
    printf("  stores queued %llu   drains %llu   full stalls %llu   forwards %llu\n",
           (unsigned long long)wb.enqueued_stores, (unsigned long long)wb.drains,
           (unsigned long long)wb.full_stalls, (unsigned long long)wb.forwards);

    printf("\n---------------- Main memory (32 MB, LFU+aging) ----------------\n");
    printf("  page faults %llu   disk reads %llu   writebacks %llu\n",
           (unsigned long long)mm.page_faults, (unsigned long long)mm.disk_reads,
           (unsigned long long)mm.disk_writebacks);
    printf("  frame evictions %llu   free frames %u / %d\n",
           (unsigned long long)mm.evictions, mm.free_count, NUM_FRAMES);
    printf("  block fetches %llu   write arrivals %llu\n",
           (unsigned long long)mm.block_fetches, (unsigned long long)mm.writes);

    printf("\n  per process:  pid  frames held  limits\n");
    for (uint16_t i = 0; i < nproc; i++)
        printf("                %3u  %11u  %u..%u\n", procs[i].pid,
               procs[i].frames_held, procs[i].lower_limit, procs[i].upper_limit);

    if (mm.evictions == 0)
        printf("\n  NOTE: no frame was ever evicted -- %d frames is far more than these\n"
               "        traces touch, so page replacement never came under pressure.\n"
               "        Lower PROC_UPPER_LIMIT in new_main.c to force it.\n", NUM_FRAMES);
    printf("=====================================\n");
}

/* ------------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    FILE    *fp[MAX_PROCS];
    int      done[MAX_PROCS];
    uint16_t nproc, cur;
    uint64_t quantum, in_quantum;
    int      alive, oom = 0;
    WBEntry  e;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <num_processes> <accesses_per_context_switch>\n",
                argv[0]);
        return 2;
    }

    nproc   = (uint16_t)strtoul(argv[1], NULL, 10);
    quantum = strtoull(argv[2], NULL, 10);

    if (nproc < 1 || nproc > MAX_PROCS) {
        fprintf(stderr, "num_processes must be 1..%d (one per trace)\n", MAX_PROCS);
        return 2;
    }
    if (quantum < 1) {
        fprintf(stderr, "accesses_per_context_switch must be >= 1\n");
        return 2;
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
        if (mm_create_process(&mm, &procs[i], (uint16_t)(i + 1),
                              PROC_LOWER_LIMIT, PROC_UPPER_LIMIT) != 0) {
            fprintf(stderr,
                    "OUT OF MEMORY: could not seat process %u.  Too many "
                    "processes for %d frames.\n", i + 1, NUM_FRAMES);
            return 1;
        }

        fp[i] = fopen(TRACE_FILES[i], "r");
        if (!fp[i]) {
            fprintf(stderr, "cannot open %s\n", TRACE_FILES[i]);
            return 1;
        }
        done[i] = 0;
        printf("  process %u  <- %-34s  page table in frame %u\n",
               procs[i].pid, TRACE_FILES[i], procs[i].pt_frame);
    }

    /* --- run, round robin ---------------------------------------------- */
    cur = 0;
    in_quantum = 0;
    alive = nproc;

    while (alive > 0 && !oom) {
        char line[64];

        if (done[cur]) {                        /* this trace is finished */
            cur = (uint16_t)((cur + 1) % nproc);
            in_quantum = 0;
            continue;
        }

        if (!fgets(line, sizeof line, fp[cur])) {
            done[cur] = 1;
            alive--;
            cur = (uint16_t)((cur + 1) % nproc);
            in_quantum = 0;
            continue;
        }

        {
            uint32_t   va  = (uint32_t)strtoul(line, NULL, 16);
            AccessType acc = is_write() ? ACC_WRITE : ACC_READ;

            if (do_access(&procs[cur], va, acc) != 0) {
                fprintf(stderr,
                        "\nOUT OF MEMORY at access %llu: every frame is a page "
                        "table or is protected by a process's lower limit.\n",
                        (unsigned long long)n_access);
                oom = 1;
                break;
            }
        }

        /* --- context switch ---------------------------------------------
         * The TLB is PID tagged, so a switch costs nothing: entries from
         * several processes coexist and none has to be flushed. */
        if (++in_quantum >= quantum) {
            in_quantum = 0;
            if (nproc > 1) {
                cur = (uint16_t)((cur + 1) % nproc);
                n_switch++;
            }
        }
    }

    /* --- drain what the write buffer still holds ------------------------
     * Without this the last stores never reach memory. */
    while (wb_drain_head(&wb, &e)) {
        wb.drains++;
        mm_write(&mm, WB_TO_PA(e.block_addr));
    }

    report(nproc, quantum);

    for (uint16_t i = 0; i < nproc; i++)
        fclose(fp[i]);
    return oom ? 1 : 0;
}
