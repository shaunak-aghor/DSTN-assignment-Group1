/* ============================================================================
 *  src/main.c -- simulator driver
 *
 *  Reads a trace file (or stdin), drives the MMU for every reference, and
 *  reports what the memory subsystem did.
 *
 *  Currently exercises TLB + page table + main memory.  The cache hierarchy
 *  is wired in at the single marked hook below, as soon as l1.c / l2.c
 *  provide l1_init(), l2_init() and the rest -- nothing else here changes.
 *
 *      ./sim_q3 [options] [tracefile]
 *
 *  TRACE FORMAT -- one directive per line, blank lines and '#' comments
 *  ignored, keywords case-insensitive:
 *
 *      PROC <pid> [<lower> <upper>]   create a process and pre-page it.
 *                                     lower defaults to MIN_FRAMES_PER_PROC
 *                                     (3 = 2 pre-paged pages + page table),
 *                                     upper to 256 (the whole address space).
 *      R <pid> <addr>                 read
 *      W <pid> <addr>                 write
 *      X <pid> <addr>                 instruction fetch
 *      PROT <pid> <page> <rwx>        set a page's permissions, e.g. "r--"
 *      TICK                           run the aging timer once
 *      EXIT <pid>                     terminate a process
 *      DUMP                           print the TLB right now
 *      STATS                          print a statistics snapshot
 *
 *  <addr> is an 18-bit virtual address, hex (0x...) or decimal.
 *  <pid> is 1..16383 (PID_BITS = 14).
 * ==========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "translate.h"

#define MAX_PROCS   64
#define LINE_MAX_   256

/* MainMemory embeds FrameDesc[32768] -- ~512 KB. Static, never a local. */
static MainMemory mm;
static Process    procs[MAX_PROCS];
static MMU        mmu;

static int      verbose      = 0;
static uint64_t line_no      = 0;
static uint64_t trace_errors = 0;

/* ------------------------------------------------------------------------
 *  Cache-hierarchy hook
 *
 *  L1 and L2 are physically tagged, so when main memory reclaims a frame the
 *  lines caching it MUST be invalidated or they will serve the previous
 *  page's bytes.  For frame f (64 blocks of 16 B in a 1 KB page):
 *
 *      L1: tag == f            -- 64 sets x 4 ways, invalidate where tag == f
 *      L2: tag == f >> 2, sets ((f & 3) << 6) | 0..63
 *
 *  Replace the body with l1_invalidate_frame()/l2_invalidate_frame() calls
 *  once those exist.  Counting here keeps the omission visible in the report
 *  rather than silent.
 * --------------------------------------------------------------------- */
static uint64_t pending_cache_invalidations = 0;

static void cache_invalidate_frame(void *ctx, uint32_t frame)
{
    (void)ctx; (void)frame;
    pending_cache_invalidations++;
}

/* ------------------------------------------------------------------------
 *  Helpers
 * --------------------------------------------------------------------- */

static void usage(const char *argv0)
{
    printf(
"Usage: %s [options] [tracefile]\n"
"\n"
"  Reads the trace from <tracefile>, or from stdin if none is given.\n"
"\n"
"Options:\n"
"  -v, --verbose      one line of output per memory reference\n"
"  -q, --quiet        suppress the per-directive echo\n"
"  -t, --tick N       run the aging timer every N accesses (default 1000,\n"
"                     0 disables it)\n"
"  -h, --help         this text\n"
"\n"
"Trace directives (case-insensitive, '#' starts a comment):\n"
"  PROC <pid> [<lower> <upper>]  create and pre-page a process\n"
"  R|W|X <pid> <addr>            read / write / instruction fetch\n"
"  PROT <pid> <page> <rwx>       set page permissions, e.g. r-- or rw-\n"
"  TICK                          run the aging timer once\n"
"  EXIT <pid>                    terminate a process\n"
"  DUMP                          dump the TLB\n"
"  STATS                         print a statistics snapshot\n"
"\n"
"Addresses are 18-bit virtual addresses in hex (0x...) or decimal.\n",
        argv0);
}

static int parse_u32(const char *s, uint32_t *out)
{
    char *end;
    unsigned long v;

    if (!s || !*s) return 0;
    v = strtoul(s, &end, (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10);
    if (*end != '\0') return 0;
    *out = (uint32_t)v;
    return 1;
}

static void upcase(char *s)
{
    for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

static const char *acc_name(AccessType a)
{
    return a == ACC_READ ? "R" : a == ACC_WRITE ? "W" : "X";
}

static void trace_error(const char *msg, const char *tok)
{
    fprintf(stderr, "trace line %llu: %s%s%s\n",
            (unsigned long long)line_no, msg,
            tok ? ": " : "", tok ? tok : "");
    trace_errors++;
}

/* ------------------------------------------------------------------------
 *  One memory reference
 * --------------------------------------------------------------------- */
static void do_access(uint16_t pid, uint32_t va, AccessType acc)
{
    uint32_t  pa = 0;
    MMUStatus st = mmu_translate(&mmu, pid, va, acc, &pa);

    if (verbose) {
        printf("%6llu  pid %-4u %s  VA 0x%05X  vpn %3u off %4u  %-28s",
               (unsigned long long)mmu.accesses, pid, acc_name(acc),
               va, (unsigned)VA_VPN(va), (unsigned)VA_OFFSET(va),
               mmu_status_name(st));

        if (mmu_ok(st))
            printf("  PA 0x%06X  L1[t=%5u s=%2u o=%2u]  L2[t=%5u s=%3u o=%2u]\n",
                   pa,
                   (unsigned)L1_TAG(pa), (unsigned)L1_INDEX(pa),
                   (unsigned)L1_BLK_OFFSET(pa),
                   (unsigned)L2_TAG(pa), (unsigned)L2_INDEX(pa),
                   (unsigned)L2_BLK_OFFSET(pa));
        else
            putchar('\n');
    }

    if (!mmu_ok(st))
        return;

    /* ==================================================================
     *  CACHE HIERARCHY GOES HERE
     *
     *  The physical address is resolved; from this point the reference is
     *  the caches' problem:
     *
     *      int where = cache_search(&l1, &l2, &wb, pa);
     *      switch (where) {
     *      case 1: break;                      L1 hit
     *      case 2: wb.forwards++;    break;    write-buffer forward
     *      case 3: promote from L2 to L1, invalidate in L2,
     *              demote L1's LRU victim into L2   (exclusive hierarchy)
     *      case 0: mm_read_block(&mm, pa, blk); install in L1;
     *              demote L1's victim into L2
     *      }
     *      on a write: wb_enqueue_store(...) -> drains through L2 to
     *      mm_write(), since both levels are write-through.
     *
     *  Blocked on l1_init/l1_install/l1_evict and l2_init/l2_promote/
     *  l2_allocate, which are still unimplemented.
     * ================================================================== */
}

/* ------------------------------------------------------------------------
 *  Trace interpreter
 * --------------------------------------------------------------------- */
static void run_trace(FILE *in, int echo)
{
    char line[LINE_MAX_];

    while (fgets(line, sizeof(line), in)) {
        char *hash, *tok, *save = NULL;
        char  op[LINE_MAX_];
        uint32_t a = 0, b = 0, c = 0;

        line_no++;

        hash = strchr(line, '#');
        if (hash) *hash = '\0';

        tok = strtok_r(line, " \t\r\n", &save);
        if (!tok) continue;                     /* blank or comment-only */

        strncpy(op, tok, sizeof(op) - 1);
        op[sizeof(op) - 1] = '\0';
        upcase(op);

        /* ---- PROC <pid> [<lower> <upper>] ---- */
        if (strcmp(op, "PROC") == 0) {
            Process *p = NULL;
            uint16_t i;

            tok = strtok_r(NULL, " \t\r\n", &save);
            if (!parse_u32(tok, &a) || a == 0 || a > MASK(PID_BITS)) {
                trace_error("PROC needs a pid in 1..16383", tok); continue;
            }
            if (mmu_find_process(&mmu, (uint16_t)a)) {
                trace_error("PROC: pid already active", tok); continue;
            }
            for (i = 0; i < MAX_PROCS; i++)
                if (!procs[i].active) { p = &procs[i]; break; }
            if (!p) { trace_error("PROC: process table full", NULL); continue; }

            memset(p, 0, sizeof(*p));
            p->pid = (uint16_t)a;

            tok = strtok_r(NULL, " \t\r\n", &save);
            if (tok && parse_u32(tok, &b)) p->lower_limit = (uint16_t)b;
            tok = strtok_r(NULL, " \t\r\n", &save);
            if (tok && parse_u32(tok, &c)) p->upper_limit = (uint16_t)c;

            if (mm_prepage(&mm, p) != 0) {
                trace_error("PROC: pre-paging failed (out of memory)", NULL);
                continue;
            }
            if (echo)
                printf("  [PROC %u created: page table in frame %u, "
                       "pages 0-1 pre-paged, limits %u..%u]\n",
                       p->pid, p->pt_frame, p->lower_limit, p->upper_limit);
            continue;
        }

        /* ---- R | W | X <pid> <addr> ---- */
        if (strcmp(op, "R") == 0 || strcmp(op, "W") == 0 || strcmp(op, "X") == 0) {
            AccessType acc = (op[0] == 'R') ? ACC_READ
                           : (op[0] == 'W') ? ACC_WRITE : ACC_EXEC;

            tok = strtok_r(NULL, " \t\r\n", &save);
            if (!parse_u32(tok, &a)) { trace_error("bad pid", tok); continue; }
            tok = strtok_r(NULL, " \t\r\n", &save);
            if (!parse_u32(tok, &b)) { trace_error("bad address", tok); continue; }

            do_access((uint16_t)a, b, acc);
            continue;
        }

        /* ---- PROT <pid> <page> <rwx> ---- */
        if (strcmp(op, "PROT") == 0) {
            Process *p;
            uint32_t prot = 0;

            tok = strtok_r(NULL, " \t\r\n", &save);
            if (!parse_u32(tok, &a)) { trace_error("bad pid", tok); continue; }
            tok = strtok_r(NULL, " \t\r\n", &save);
            if (!parse_u32(tok, &b) || b >= PAGES_PER_PROC) {
                trace_error("page must be 0..255", tok); continue;
            }
            tok = strtok_r(NULL, " \t\r\n", &save);
            if (!tok) { trace_error("PROT needs permissions like rw-", NULL); continue; }
            if (strchr(tok, 'r')) prot |= PROT_READ;
            if (strchr(tok, 'w')) prot |= PROT_WRITE;
            if (strchr(tok, 'x')) prot |= PROT_EXEC;

            p = mmu_find_process(&mmu, (uint16_t)a);
            if (!p || !p->pt) { trace_error("PROT: no such process", NULL); continue; }

            p->pt->entries[b].prot = prot;
            /* A permission change is only observed on a TLB miss -- see the
             * limitation note in translate.h -- so drop any cached entry. */
            tlb_impl_invalidate_entry(&mmu.tlb, (uint16_t)a, b);

            if (echo)
                printf("  [PROT pid %u page %u = %s (TLB entry invalidated)]\n",
                       a, b, tok);
            continue;
        }

        if (strcmp(op, "TICK") == 0) {
            mm_age_tick(&mm, procs, MAX_PROCS);
            if (echo) printf("  [TICK: aging registers shifted]\n");
            continue;
        }

        if (strcmp(op, "EXIT") == 0) {
            tok = strtok_r(NULL, " \t\r\n", &save);
            if (!parse_u32(tok, &a)) { trace_error("bad pid", tok); continue; }
            if (!mmu_find_process(&mmu, (uint16_t)a)) {
                trace_error("EXIT: no such process", tok); continue;
            }
            mmu_process_exit(&mmu, (uint16_t)a);
            if (echo)
                printf("  [EXIT %u: frames released, TLB entries invalidated]\n", a);
            continue;
        }

        if (strcmp(op, "DUMP") == 0)  { tlb_impl_dump(&mmu.tlb);        continue; }
        if (strcmp(op, "STATS") == 0) { mmu_print_stats(&mmu, stdout);  continue; }

        trace_error("unknown directive", op);
    }
}

/* ------------------------------------------------------------------------
 *  main
 * --------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    const char *path = NULL;
    FILE       *in   = stdin;
    uint64_t    tick = 1000;
    int         echo = 1, i;
    uint16_t    p;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]); return 0;
        } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
            verbose = 1;
        } else if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) {
            echo = 0;
        } else if (!strcmp(argv[i], "-t") || !strcmp(argv[i], "--tick")) {
            if (i + 1 >= argc) { fprintf(stderr, "--tick needs a value\n"); return 2; }
            tick = strtoull(argv[++i], NULL, 10);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            usage(argv[0]); return 2;
        } else {
            path = argv[i];
        }
    }

    if (path) {
        in = fopen(path, "r");
        if (!in) { perror(path); return 1; }
    }

    if (mm_init(&mm) != 0) {
        fprintf(stderr, "could not allocate %lu MB of main memory\n",
                MM_SIZE / (1024UL * 1024UL));
        return 1;
    }

    memset(procs, 0, sizeof(procs));
    mmu_init(&mmu, &mm, procs, MAX_PROCS);
    mmu_set_cache_invalidator(&mmu, cache_invalidate_frame, NULL);
    mmu_set_tick_interval(&mmu, tick);

    printf("=================================================================\n");
    printf("  Memory subsystem simulator\n");
    printf("  VA %u bits (%u pages x %u B) | PA %u bits (%u frames, %lu MB)\n",
           VA_BITS, PAGES_PER_PROC, PAGE_SIZE,
           PA_BITS, NUM_FRAMES, MM_SIZE / (1024UL * 1024UL));
    printf("  TLB %u entries, PID-tagged, LRU | aging tick every %llu accesses\n",
           TLB_ENTRIES, (unsigned long long)tick);
    printf("  trace: %s\n", path ? path : "<stdin>");
    printf("=================================================================\n");

    if (verbose) {
        printf("\n%6s  %-8s %s  %-10s  %-16s  %-28s  %s\n",
               "#", "process", "op", "virtual", "page / offset", "outcome",
               "physical address + cache set decomposition");
        for (i = 0; i < 126; i++) putchar('-');
        putchar('\n');
    }

    run_trace(in, echo);

    if (in != stdin) fclose(in);

    mmu_print_stats(&mmu, stdout);

    printf("--- Cache hierarchy ---\n"
           "  frame invalidations requested .. %llu\n"
           "  (L1/L2 invalidation is stubbed: l1.c and l2.c do not yet export\n"
           "   an invalidate-by-frame entry point. See the hook in src/main.c.)\n",
           (unsigned long long)pending_cache_invalidations);

    printf("--- Processes ---\n");
    for (p = 0; p < MAX_PROCS; p++)
        if (procs[p].active)
            printf("  pid %-5u resident %3u frames  (limits %u..%u)  "
                   "page table in frame %u\n",
                   procs[p].pid, procs[p].frames_held,
                   procs[p].lower_limit, procs[p].upper_limit,
                   procs[p].pt_frame);

    if (trace_errors)
        printf("\n  %llu malformed trace line(s) were skipped\n",
               (unsigned long long)trace_errors);

    tlb_impl_dump(&mmu.tlb);
    mm_destroy(&mm);

    return trace_errors ? 1 : 0;
}
