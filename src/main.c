/* 
 * To run this file, do the following:
 * unzip BenchmarkTraces-20260914.zip -d traces/
 * run: make
 * run: ./sim_q3 <num_processes> <accesses_per_context_switch> <trace file>
 * by default, num_processes=5, accesses_per_context_switch=10
 * 
 *  Each trace line is a bare hex virtual address. The traces carry no
 *  read/write information, so the access type is determined by WRITE_PERCENT, set at 30%.
 */

#include <stdio.h>
#include <stdlib.h>

#include "MM.h"
#include "mmu.h"
#include "cache.h"
#include "stats.h"

#define WRITE_PERCENT      30      /* share of accesses treated as stores */
#define AGE_TICK_INTERVAL  1000    /* accesses between OS aging passes */
#define WB_DRAIN_INTERVAL  10      /* accesses between background WB drains */
#define PROC_UPPER_LIMIT   0       /* max frames per process, 0 = PAGES_PER_PROC */
#define PROC_LOWER_LIMIT   0       /* min frames per process, 0 = MIN_FRAMES_PER_PROC */
#define MAX_PROCS          10000   /* upper bound on the num_processes argument */

static MM          mm;
static Process     procs[MAX_PROCS];
static TLB         tlb;
static L1Cache     l1;
static L2Cache     l2;
static WriteBuffer wb;

Stats stats;

/* The traces have no read/write flag, so a seeded random generator makes
 * WRITE_PERCENT% of accesses stores. Same seed = same run every time. */
static uint32_t rng = 12345u;
static int is_write(void)
{
    rng = rng * 1103515245u + 12345u;
    return (int)((rng >> 16) % 100u) < WRITE_PERCENT;
}

/* Runs one memory access. Returns 0, or -1 if main memory is full. */
static int do_access(Process *proc, uint32_t va, AccessType acc)
{
    uint32_t pa, evicted;

    stats.accesses++;
    if (acc == ACC_WRITE) stats.writes++; else stats.reads++;

    /* 1. Virtual -> physical. */
    if (va_to_pa(&tlb, &mm, proc, va, acc, &wb, &pa, &evicted) != 0)
        return -1;

    /* 2. Caches are physically tagged: blocks of a reused frame are now stale. */
    if (evicted != MM_NO_FRAME) {
        l1_invalidate_frame(&l1, evicted);
        l2_invalidate_frame(&l2, evicted);
    }

    /* 3. The cache access itself. */
    if (acc == ACC_WRITE)
        cache_write(&l1, &l2, &wb, pa);
    else
        cache_read(&l1, &l2, &wb, pa);

    /* 4. Background work. */
    if (stats.accesses % WB_DRAIN_INTERVAL == 0)
        wb_drain_head(&wb);

    if (stats.accesses % AGE_TICK_INTERVAL == 0) {
        mm_age_tick(&mm, &tlb);
        stats.ticks++;
    }
    return 0;
}

/* Helper function to calculate percentages for statistics */
static double pct(uint64_t part, uint64_t whole)
{
    return whole ? (100.0 * (double)part / (double)whole) : 0.0;
}

/* Reporting the current statistics for debugging and checking purposes */
static void report(uint16_t nproc, uint64_t quantum, const char *trace)
{
    uint64_t l1_acc  = stats.l1_read_hits + stats.l1_read_misses
                     + stats.l1_write_hits + stats.l1_write_misses;
    uint64_t l1_hit  = stats.l1_read_hits + stats.l1_write_hits;
    uint64_t l2_acc  = stats.l2_hits + stats.l2_misses;
    uint64_t tlb_acc = stats.tlb_hits + stats.tlb_misses;

    printf("\n================ RUN ================\n");
    printf("  trace %s\n", trace);
    printf("  processes %u   switch every %llu accesses   %d%% writes\n",
           nproc, (unsigned long long)quantum, WRITE_PERCENT);
    printf("  accesses %llu   reads %llu (%.1f%%)   writes %llu (%.1f%%)\n",
           (unsigned long long)stats.accesses,
           (unsigned long long)stats.reads,  pct(stats.reads,  stats.accesses),
           (unsigned long long)stats.writes, pct(stats.writes, stats.accesses));
    printf("  context switches %llu   aging ticks %llu\n",
           (unsigned long long)stats.switches, (unsigned long long)stats.ticks);

    printf("\n\nTLB (%d entries, PID tagged):\n", TLB_ENTRIES);
    printf("  hits %llu   misses %llu   hit rate %.2f%%   evictions %llu\n",
           (unsigned long long)stats.tlb_hits, (unsigned long long)stats.tlb_misses,
           pct(stats.tlb_hits, tlb_acc), (unsigned long long)stats.tlb_evictions);
    printf("  page-table walks %llu  (one main-memory access each)\n",
           (unsigned long long)stats.pt_walks);

    printf("\n\nL1 (4 KB, 16 B, 4-way, LRU):\n");
    printf("  read  hits %llu   misses %llu\n",
           (unsigned long long)stats.l1_read_hits, (unsigned long long)stats.l1_read_misses);
    printf("  write hits %llu   misses %llu   (no-write-allocate)\n",
           (unsigned long long)stats.l1_write_hits, (unsigned long long)stats.l1_write_misses);
    printf("  hit rate %.2f%%   evictions %llu\n",
           pct(l1_hit, l1_acc), (unsigned long long)stats.l1_evictions);

    printf("\n\nL2 (32 KB, 16 B, 8-way, FIFO):\n");
    printf("  hits %llu   misses %llu   hit rate %.2f%%\n",
           (unsigned long long)stats.l2_hits, (unsigned long long)stats.l2_misses,
           pct(stats.l2_hits, l2_acc));
    printf("  promotions to L1 %llu   evictions %llu   (exclusive)\n",
           (unsigned long long)stats.l2_promotions, (unsigned long long)stats.l2_evictions);
    printf("  writes updating a line %llu   passing through %llu\n",
           (unsigned long long)stats.l2_updated_writes,
           (unsigned long long)stats.l2_passthrough_writes);

    printf("\n\nWrite buffer (%d blocks, FIFO):\n", WB_ENTRIES);
    printf("  stores queued %llu   drains %llu   full stalls %llu   forwards %llu\n",
           (unsigned long long)stats.wb_enqueued, (unsigned long long)stats.wb_drains,
           (unsigned long long)stats.wb_stalls,   (unsigned long long)stats.wb_forwards);

    printf("\n\nMain memory (32 MB, LFU+aging):\n");
    printf("  page faults (disk reads) %llu   writebacks %llu\n",
           (unsigned long long)stats.page_faults, (unsigned long long)stats.disk_writebacks);
    printf("  frame evictions %llu   free frames %u / %d\n",
           (unsigned long long)stats.mm_evictions, mm.free_count, NUM_FRAMES);
    printf("  block fetches %llu   write arrivals %llu\n",
           (unsigned long long)stats.mm_block_fetches, (unsigned long long)stats.mm_writes);

    printf("\n\nper process:  pid  frames held  limits:\n");
    for (uint16_t i = 0; i < nproc; i++)
        printf("                %3u  %11u  %u..%u\n", procs[i].pid,
               procs[i].frames_held, procs[i].lower_limit, procs[i].upper_limit);

    if (stats.mm_evictions == 0)
        printf("\n\nNOTE: no frame was ever evicted -- %d frames is far more than these\n"
               "  traces touch, so page replacement was never done.\n"
               "  you can lower PROC_UPPER_LIMIT in main.c to force it.\n", NUM_FRAMES);
    printf("=====================================\n");
}


int main(int argc, char **argv)
{
    const char *trace;
    FILE       *fp;
    uint16_t    nproc, cur;
    uint64_t    quantum, in_quantum;
    int         oom = 0;
    char        line[64];

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

    /* Sets up the memory and cache systems */
    mm_init(&mm, procs, nproc);         /* also zeroes the process table */
    tlb_init(&tlb);
    l1_init(&l1);
    l2_init(&l2);
    wb_init(&wb);

    for (uint16_t i = 0; i < nproc; i++) {
        /* A page table plus the first two pages are bought once the process starts
         * into MAIN MEMORY only, not the caches. */
        if (mm_create_process(&mm, &procs[i], (uint16_t)(i + 1),
                              PROC_LOWER_LIMIT, PROC_UPPER_LIMIT) != 0) {
            fprintf(stderr,
                    "OUT OF MEMORY: could not seat process %u.  Too many "
                    "processes for %d frames.\n", i + 1, NUM_FRAMES);
            fclose(fp);
            return 1;
        }
        printf("  process %u created  (page table pinned in frame %u, "
               "pages 0-1 pre-paged)\n", procs[i].pid, procs[i].pt_frame);
    }
    printf("  trace %s, switching PID every %llu accesses\n\n",
           trace, (unsigned long long)quantum);

    /* walk the stream once, rotating the owning process */
    cur = 0;
    in_quantum = 0;

    while (fgets(line, sizeof line, fp)) {
        uint32_t   va  = (uint32_t)strtoul(line, NULL, 16);
        AccessType acc = is_write() ? ACC_WRITE : ACC_READ;

        if (do_access(&procs[cur], va, acc) != 0) {
            fprintf(stderr,
                    "\nOUT OF MEMORY at access %llu: every frame is a page "
                    "table or is protected by a process's lower limit.\n",
                    (unsigned long long)stats.accesses);
            oom = 1;
            break;
        }

        /* Context Switch flow:
         * The TLB is PID tagged, so a switch costs nothing: entries from
         * several processes coexist and none have to be flushed. 
         */
        if (++in_quantum >= quantum) {
            in_quantum = 0;
            if (nproc > 1) {
                cur = (uint16_t)((cur + 1) % nproc);
                stats.switches++;
            }
        }
    }

    /* 
     * Drain what the write buffer still holds.
     * Without this the last stores never reach memory. 
     */
    while (wb_drain_head(&wb))
        ;

    report(nproc, quantum, trace); // Outputs

    fclose(fp);
    return oom ? 1 : 0;
}
