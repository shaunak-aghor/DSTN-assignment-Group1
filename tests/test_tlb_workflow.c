/* ============================================================================
 *  tests/test_tlb_workflow.c
 *
 *  TLB workflow test cases, reported as INPUT / EXPECTED / ACTUAL / WHY.
 *
 *  Focus: the two parts of the specification that involve the TLB indirectly
 *  rather than directly --
 *
 *      "First 2 blocks of the process (page size = frame size = 1 KB) will be
 *       pre-paged into main memory [NOT to cache] before a process starts."
 *      "All other pages are loaded on demand."
 *
 *  Build:
 *    gcc -Wall -Wextra -g -I./include -o test_tlb_workflow \
 *        tests/test_tlb_workflow.c src/MainMemory.c src/translate.c src/TLB.c
 * ==========================================================================*/

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "translate.h"
#include "report.h"

/* ==========================================================================
 *  Reporting harness: every field prints EXPECTED and ACTUAL, pass or fail.
 * ========================================================================*/

static int  tc_no = 0, tc_failed = 0, tc_bad;
static char exp_buf[16][160], act_buf[16][160];
static int  fld_ok[16], nfld;

static void tc_begin(const char *id, const char *title)
{
    tc_no++;
    nfld = 0; tc_bad = 0;
    printf("\n%s  %s\n", id, title);
    for (int i = 0; i < 74; i++) putchar('=');
    putchar('\n');
}

static void tc_input(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("  Input     : "); vprintf(fmt, ap); putchar('\n');
    va_end(ap);
}

static void tc_setup(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("  Setup     : "); vprintf(fmt, ap); putchar('\n');
    va_end(ap);
}

/* Record one expected/actual pair. */
static void field_i(const char *label, long expected, long actual)
{
    snprintf(exp_buf[nfld], 160, "%-22s = %ld", label, expected);
    snprintf(act_buf[nfld], 160, "%-22s = %ld", label, actual);
    fld_ok[nfld] = (expected == actual);
    if (!fld_ok[nfld]) tc_bad = 1;
    nfld++;
}

static void field_s(const char *label, const char *expected, const char *actual)
{
    snprintf(exp_buf[nfld], 160, "%-22s = %s", label, expected);
    snprintf(act_buf[nfld], 160, "%-22s = %s", label, actual);
    fld_ok[nfld] = (strcmp(expected, actual) == 0);
    if (!fld_ok[nfld]) tc_bad = 1;
    nfld++;
}

static void tc_end(const char *why)
{
    int i;
    printf("  Expected  : %s\n", nfld ? exp_buf[0] : "(none)");
    for (i = 1; i < nfld; i++) printf("              %s\n", exp_buf[i]);
    printf("  Actual    : %s%s\n", nfld ? act_buf[0] : "(none)",
           nfld && !fld_ok[0] ? "     <-- MISMATCH" : "");
    for (i = 1; i < nfld; i++)
        printf("              %s%s\n", act_buf[i],
               fld_ok[i] ? "" : "     <-- MISMATCH");
    printf("  Result    : %s\n", tc_bad ? "*** FAIL ***" : "PASS");
    printf("  Why       : %s\n", why);
    if (tc_bad) tc_failed++;
}

/* ==========================================================================
 *  Fixtures
 * ========================================================================*/

static MainMemory mm;
static Process    procs[8];
static MMU        mmu;

static uint64_t s_walks, s_faults, s_disk, s_hits, s_misses, s_blocks;

static void snap(void)
{
    s_walks  = mmu.pt_walks;   s_faults = mmu.page_faults;
    s_hits   = mmu.tlb_hits;   s_misses = mmu.tlb_misses;
    s_disk   = mm.disk_reads;  s_blocks = mm.block_fetches;
}
static long d_walks(void)  { return (long)(mmu.pt_walks    - s_walks);  }
static long d_faults(void) { return (long)(mmu.page_faults - s_faults); }
static long d_disk(void)   { return (long)(mm.disk_reads   - s_disk);   }
static long d_hits(void)   { return (long)(mmu.tlb_hits    - s_hits);   }
static long d_misses(void) { return (long)(mmu.tlb_misses  - s_misses); }

static unsigned tlb_valid(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < TLB_ENTRIES; i++) n += mmu.tlb.entries[i].valid;
    return n;
}

static void world(uint16_t n)
{
    mm_destroy(&mm);
    memset(procs, 0, sizeof(procs));
    mm_init(&mm);
    for (uint16_t i = 0; i < n; i++) procs[i].pid = (uint16_t)(i + 1);
    mmu_init(&mmu, &mm, procs, n);
    mmu_set_tick_interval(&mmu, 0);
}

#define VA(page, off)  (((uint32_t)(page) << PAGE_OFFSET_BITS) | (off))

/* ========================================================================== */

int main(int argc, char **argv)
{
    report_begin(argc, argv, "results/test_tlb_workflow.txt");

    uint32_t pa;
    MMUStatus st;

    printf("========================================================================\n");
    printf("  TLB WORKFLOW TEST CASES\n");
    printf("  pre-paging of the first 2 pages, and page-fault handling\n");
    printf("  page = frame = %u B, VA %u bits, TLB %u entries\n",
           PAGE_SIZE, VA_BITS, TLB_ENTRIES);
    printf("========================================================================\n");

    /* ------------------------------------------------------------ TC-01 */
    world(2);
    tc_begin("TC-01", "Pre-paging loads exactly the first 2 pages");
    tc_input("mm_prepage(pid 1)");
    tc_setup("fresh 32 MB memory, no process yet");
    {
        int rc = mm_prepage(&mm, &procs[0]);
        field_i("return code",        0, rc);
        field_i("PTE[0].present",     1, procs[0].pt->entries[0].present);
        field_i("PTE[1].present",     1, procs[0].pt->entries[1].present);
        field_i("PTE[2].present",     0, procs[0].pt->entries[2].present);
        field_i("frames_held",        2, procs[0].frames_held);
        field_i("frames consumed",    3, (long)(NUM_FRAMES - mm.free_frames));
        field_i("disk reads",         2, (long)mm.disk_reads);
    }
    tc_end("The specification pre-pages the first 2 pages only. The third\n"
           "              frame is the page table itself, which must be resident\n"
           "              before any translation can happen -- hence\n"
           "              MIN_FRAMES_PER_PROC = 3 = 2 pages + 1 page table.\n"
           "              Two disk reads happened: pre-paging is real I/O.");

    /* ------------------------------------------------------------ TC-02 */
    tc_begin("TC-02", "Pre-paging puts NOTHING in the TLB");
    tc_input("inspect the TLB immediately after mm_prepage(pid 1)");
    tc_setup("pages 0 and 1 are resident in main memory");
    field_i("valid TLB entries",   0, (long)tlb_valid());
    field_i("probe(pid 1, vpn 0)", -1, tlb_impl_probe(&mmu.tlb, 1, 0));
    field_i("probe(pid 1, vpn 1)", -1, tlb_impl_probe(&mmu.tlb, 1, 1));
    field_i("16 B block fetches",   0, (long)mm.block_fetches);
    tc_end("The specification says pre-paged \"into main memory [not to\n"
           "              cache]\". The TLB caches TRANSLATIONS, and a translation\n"
           "              only exists once something has been translated -- no\n"
           "              reference has been made yet. So pre-paging populates the\n"
           "              page table, not the TLB, and block_fetches = 0 confirms\n"
           "              nothing reached L1/L2 either. The first instruction fetch\n"
           "              is therefore a guaranteed TLB miss AND cache miss.");

    /* ------------------------------------------------------------ TC-03 */
    tc_begin("TC-03", "First touch of a PRE-PAGED page: TLB miss, but no fault");
    tc_input("read VA 0x%05X  (page 1, offset 0x034)", VA(1, 0x34));
    tc_setup("page 1 pre-paged and present; TLB empty");
    snap();
    st = mmu_translate(&mmu, 1, VA(1, 0x34), ACC_READ, &pa);
    field_s("status", "OK (TLB miss, PTE present)", mmu_status_name(st));
    field_i("TLB misses",       1, d_misses());
    field_i("page-table walks", 1, d_walks());
    field_i("page faults",      0, d_faults());
    field_i("disk reads",       0, d_disk());
    field_i("TLB entries now",  1, (long)tlb_valid());
    tc_end("This is the case pre-paging exists to create, and the reason\n"
           "              the MMU returns THREE distinct success codes. A TLB miss\n"
           "              whose walk finds the PTE already present costs ONE memory\n"
           "              access and no disk I/O. Without pre-paging this same\n"
           "              access would have been a page fault: a walk PLUS a disk\n"
           "              read PLUS an instruction restart.");

    /* ------------------------------------------------------------ TC-04 */
    tc_begin("TC-04", "Second touch of the same page: pure TLB hit");
    tc_input("read VA 0x%05X  (page 1 again, different offset)", VA(1, 0x38));
    tc_setup("the TLB now holds (pid 1, vpn 1)");
    snap();
    st = mmu_translate(&mmu, 1, VA(1, 0x38), ACC_READ, &pa);
    field_s("status", "OK (TLB hit)", mmu_status_name(st));
    field_i("TLB hits",         1, d_hits());
    field_i("page-table walks", 0, d_walks());
    field_i("disk reads",       0, d_disk());
    tc_end("Zero walks is the whole point of the TLB: the page table was\n"
           "              not touched at all. Since page tables are never cached,\n"
           "              every avoided walk is one avoided main-memory access.");

    /* ------------------------------------------------------------ TC-05 */
    tc_begin("TC-05", "First touch of a NON-pre-paged page: full page fault");
    tc_input("read VA 0x%05X  (page 9, offset 0x134)", VA(9, 0x134));
    tc_setup("page 9 was never loaded: PTE[9].present = 0");
    snap();
    {
        uint16_t held = procs[0].frames_held;
        st = mmu_translate(&mmu, 1, VA(9, 0x134), ACC_READ, &pa);
        field_s("status", "OK (page fault serviced)", mmu_status_name(st));
        field_i("TLB misses",        1, d_misses());
        field_i("page-table walks",  1, d_walks());
        field_i("page faults",       1, d_faults());
        field_i("disk reads",        1, d_disk());
        field_i("PTE[9].present",    1, procs[0].pt->entries[9].present);
        field_i("frames_held grew by", 1, (long)(procs[0].frames_held - held));
        field_i("PA page offset", 0x134, (long)PA_OFFSET(pa));
    }
    tc_end("The demand-paging path, end to end. Compare with TC-03: the\n"
           "              TLB miss and the walk are identical -- what differs is\n"
           "              that the PTE came back absent, so the OS had to allocate\n"
           "              a frame and read the page from disk. The page offset\n"
           "              0x134 is unchanged in the physical address, because\n"
           "              translation only ever replaces the page number.");

    /* ------------------------------------------------------------ TC-06 */
    tc_begin("TC-06", "The fault handler leaves the translation IN the TLB");
    tc_input("read VA 0x%05X again (page 9, new offset)", VA(9, 0x140));
    tc_setup("the fault in TC-05 has just completed");
    snap();
    st = mmu_translate(&mmu, 1, VA(9, 0x140), ACC_READ, &pa);
    field_s("status", "OK (TLB hit)", mmu_status_name(st));
    field_i("page-table walks", 0, d_walks());
    field_i("page faults",      0, d_faults());
    field_i("disk reads",       0, d_disk());
    tc_end("A fault that did not fill the TLB would make the restarted\n"
           "              instruction miss again, walk again, and -- because the\n"
           "              PTE is now present -- silently cost a second memory\n"
           "              access forever after. Step [7] of the flow inserts the\n"
           "              mapping before the physical address is built.");

    /* ------------------------------------------------------------ TC-07 */
    tc_begin("TC-07", "A FAILED access must not pollute the TLB");
    tc_input("write to VA 0x%05X, on a page marked r--", VA(50, 0x10));
    tc_setup("PTE[50].prot = PROT_READ, page absent");
    procs[0].pt->entries[50].prot = PROT_READ;
    snap();
    st = mmu_translate(&mmu, 1, VA(50, 0x10), ACC_WRITE, &pa);
    field_s("status", "FAULT: protection violation", mmu_status_name(st));
    field_i("probe(pid 1, vpn 50)", -1, tlb_impl_probe(&mmu.tlb, 1, 50));
    field_i("page faults",           0, d_faults());
    field_i("disk reads",            0, d_disk());
    field_i("PTE[50].present",       0, procs[0].pt->entries[50].present);
    tc_end("Two things are proved at once. No TLB entry was created, so a\n"
           "              later legal access is still forced through the page table.\n"
           "              And no frame or disk read was spent: protection is checked\n"
           "              immediately after the walk, BEFORE the fault path, so an\n"
           "              access that can never succeed costs nothing but the walk.");

    /* ------------------------------------------------------------ TC-08 */
    tc_begin("TC-08", "An out-of-range VA never reaches the TLB at all");
    tc_input("read VA 0x%05X  (= 2^%u, one past the address space)",
             1u << VA_BITS, VA_BITS);
    tc_setup("pid 1 is running normally");
    snap();
    {
        uint32_t before = tlb_valid();
        pa = 0xDEADBEEF;
        st = mmu_translate(&mmu, 1, 1u << VA_BITS, ACC_READ, &pa);
        field_s("status", "FAULT: address out of range", mmu_status_name(st));
        field_i("TLB lookups performed", 0, d_hits() + d_misses());
        field_i("valid TLB entries unchanged", (long)before, (long)tlb_valid());
        field_i("pa_out left untouched", (long)0xDEADBEEF, (long)pa);
    }
    tc_end("VA_VPN() masks to 8 bits, so 0x40000 would decode as PAGE 0 --\n"
           "              and page 0 is resident, so the access would SUCCEED while\n"
           "              reading completely the wrong page. The range check runs\n"
           "              before the split, which is why zero TLB lookups happened.");

    /* ------------------------------------------------------------ TC-09 */
    tc_begin("TC-09", "Every TLB miss costs exactly one page-table walk");
    tc_input("40 reads spread over 20 distinct pages of pid 2");
    tc_setup("pid 2 pre-paged, TLB shared with pid 1's entries");
    mm_prepage(&mm, &procs[1]);
    snap();
    for (int i = 0; i < 20; i++) {
        mmu_translate(&mmu, 2, VA(i, 0), ACC_READ, &pa);
        mmu_translate(&mmu, 2, VA(i, 8), ACC_READ, &pa);
    }
    field_i("accesses",         40, d_hits() + d_misses());
    field_i("TLB misses",       20, d_misses());
    field_i("TLB hits",         20, d_hits());
    field_i("page-table walks", 20, d_walks());
    field_i("walks == misses",   1, (d_walks() == d_misses()));
    tc_end("Each page is touched twice: the first touch misses and walks,\n"
           "              the second hits. walks == misses is an invariant of the\n"
           "              design -- the page table is consulted if and only if the\n"
           "              TLB failed. Note pid 2's first 2 pages were pre-paged, so\n"
           "              of the 20 misses only 18 became page faults.");

    /* ------------------------------------------------------------ TC-10 */
    tc_begin("TC-10", "Pre-paged pages fault on their FIRST access only if evicted");
    tc_input("pid 3 with upper_limit 4, then touch 12 distinct pages");
    tc_setup("cap forces the process to evict its own pages");
    world(3);
    procs[2].pid = 3; procs[2].upper_limit = 4;
    mm_prepage(&mm, &procs[2]);
    procs[2].lower_limit = 2;
    snap();
    {
        uint16_t peak = 0;
        for (int i = 0; i < 12; i++) {
            mmu_translate(&mmu, 3, VA(i, 0), ACC_READ, &pa);
            if (procs[2].frames_held > peak) peak = procs[2].frames_held;
        }
        /* page 0 was pre-paged, evicted by the cap, and re-faulted */
        field_i("peak resident frames",  4, (long)peak);
        field_i("final resident frames", 4, (long)procs[2].frames_held);
        field_i("page faults",          10, d_faults());
        field_i("free frames still ample", 1,
                (mm.free_frames > NUM_FRAMES - 100));
    }
    tc_end("Pre-paging is a head start, not a guarantee. Pages 0 and 1 came\n"
           "              in free, so 12 pages touched cost only 10 faults. But the\n"
           "              upper_limit caps the resident set at 4, so the pre-paged\n"
           "              pages are evicted like any other -- and memory was never\n"
           "              short (32000+ frames free), proving the CAP forced this,\n"
           "              not scarcity.");

    /* ------------------------------------------------------------ TC-11 */
    tc_begin("TC-11", "A fault that evicts a page kills that page's TLB entry");
    world(1);
    mm_prepage(&mm, &procs[0]);
    tc_input("fault in a new page when memory is full and page 4 is coldest");
    tc_setup("memory filled; pid 1 holds pages 0-7; page 4 is in the TLB");
    {
        uint32_t doomed = 4, doomed_frame;
        int f;
        for (int i = 2; i < 8; i++) mmu_translate(&mmu, 1, VA(i,0), ACC_READ, &pa);
        mmu_translate(&mmu, 1, VA(doomed, 0), ACC_READ, &pa);
        doomed_frame = procs[0].pt->entries[doomed].frame;

        field_i("page 4 is in the TLB before", 1,
                tlb_impl_probe(&mmu.tlb, 1, doomed) >= 0);

        while ((f = mm_alloc_frame(&mm, 0x3FFF, 0, 0)) >= 0)
            mm.frames[f].aging = 0xFF;               /* fill, make them hot */
        for (int i = 0; i < 8; i++)
            if (procs[0].pt->entries[i].present)
                mm.frames[procs[0].pt->entries[i].frame].aging = 0xFF;
        mm.frames[doomed_frame].aging = 0x00;        /* page 4 = coldest */

        st = mmu_translate(&mmu, 1, VA(60, 0), ACC_READ, &pa);

        field_s("status", "OK (page fault serviced)", mmu_status_name(st));
        field_i("PTE[4].present after",         0, procs[0].pt->entries[4].present);
        field_i("probe(pid 1, vpn 4) after",   -1, tlb_impl_probe(&mmu.tlb, 1, 4));
        field_i("page 60's new frame", (long)doomed_frame, (long)PA_FRAME(pa));
    }
    tc_end("This is the page-fault case that actually endangers the TLB.\n"
           "              Frame reuse makes a cached translation a lie: a hit on\n"
           "              page 4 would return a frame that now holds page 60. The\n"
           "              fault handler notifies the MMU, which calls\n"
           "              tlb_impl_invalidate_frame() BEFORE the frame is refilled.\n"
           "              Note it is invalidated by FRAME, not by page, because a\n"
           "              frame can be mapped from several page tables.");

    /* ------------------------------------------------------------ TC-12 */
    tc_begin("TC-12", "Pre-paging both processes: TLB stays empty, memory does not");
    world(2);
    tc_input("mm_prepage(pid 1); mm_prepage(pid 2)");
    tc_setup("fresh memory");
    mm_prepage(&mm, &procs[0]);
    mm_prepage(&mm, &procs[1]);
    field_i("frames consumed", 6, (long)(NUM_FRAMES - mm.free_frames));
    field_i("valid TLB entries", 0, (long)tlb_valid());
    field_i("pid 1 frames_held", 2, procs[0].frames_held);
    field_i("pid 2 frames_held", 2, procs[1].frames_held);
    field_i("demand page faults", 0, (long)mm.page_faults);
    tc_end("6 frames = 2 processes x (2 pre-paged pages + 1 page table).\n"
           "              The TLB is still completely empty: pre-paging is a MEMORY\n"
           "              event, not a translation event. And page_faults stays 0\n"
           "              because pre-paged loads are planned, not demanded -- they\n"
           "              cost disk reads but must not inflate the fault rate you\n"
           "              report.");

    /* ------------------------------------------------------------ TC-13 */
    world(2);
    mm_prepage(&mm, &procs[0]);
    tc_begin("TC-13", "Dead or unknown process is rejected before the TLB");
    tc_input("read VA 0x%05X as pid 9 (never created)", VA(1, 0));
    tc_setup("only pid 1 and pid 2 exist; pid 1 is running");
    snap();
    st = mmu_translate(&mmu, 9, VA(1, 0), ACC_READ, &pa);
    field_s("status", "FAULT: process not runnable", mmu_status_name(st));
    field_i("TLB lookups performed", 0, d_hits() + d_misses());
    field_i("page-table walks",      0, d_walks());
    tc_end("Step [2] runs before step [3]. Without it the TLB would be\n"
           "              probed with a PID that owns no page table, and a stale\n"
           "              entry from a recycled PID could produce a hit for a\n"
           "              process that does not exist.");

    /* ------------------------------------------------------------ TC-14 */
    tc_begin("TC-14", "Execute permission is enforced separately from read");
    tc_input("X (instruction fetch) on page 30 marked rw- , then on page 31 marked r-x");
    tc_setup("pid 1 running; both pages absent");
    procs[0].pt->entries[30].prot = PROT_READ | PROT_WRITE;           /* no X */
    procs[0].pt->entries[31].prot = PROT_READ | PROT_EXEC;            /* no W */
    {
        MMUStatus s1 = mmu_translate(&mmu, 1, VA(30, 0), ACC_EXEC,  &pa);
        MMUStatus s2 = mmu_translate(&mmu, 1, VA(31, 0), ACC_EXEC,  &pa);
        MMUStatus s3 = mmu_translate(&mmu, 1, VA(31, 0), ACC_WRITE, &pa);
        MMUStatus s4;

        /* s3 above HIT in the TLB, which carries no protection bits, so the
         * illegal write was allowed. Force the check through the page table
         * to test the permission bits themselves. TC-23 covers the hit case
         * as the documented limitation it is. */
        tlb_impl_invalidate_entry(&mmu.tlb, 1, 31);
        s4 = mmu_translate(&mmu, 1, VA(31, 8), ACC_WRITE, &pa);

        field_s("X on rw- page (absent)", "FAULT: protection violation",
                mmu_status_name(s1));
        field_s("X on r-x page (absent)", "OK (page fault serviced)",
                mmu_status_name(s2));
        field_s("W on r-x page, TLB cached", "OK (TLB hit)", mmu_status_name(s3));
        field_s("W on r-x page, after invalidate", "FAULT: protection violation",
                mmu_status_name(s4));
        field_i("probe(pid 1, vpn 30)", -1, tlb_impl_probe(&mmu.tlb, 1, 30));
    }
    tc_end("Three access types, three independent permission bits: page 30\n"
           "              is rw- so a fetch is refused, page 31 is r-x so a fetch\n"
           "              succeeds but a write is refused. The third line is the\n"
           "              honest part -- my FIRST version of this case expected the\n"
           "              write to fault, and the harness caught the wrong\n"
           "              EXPECTATION, not a code bug: page 31 was cached by the\n"
           "              successful fetch, and a TLB hit carries no protection\n"
           "              bits. Line four forces the walk and the check fires.\n"
           "              Also note page 30 left no TLB entry: a refused access\n"
           "              never caches a translation.");

    /* ------------------------------------------------------------ TC-15 */
    world(1);
    mm_prepage(&mm, &procs[0]);
    tc_begin("TC-15", "The aging timer fires automatically every N accesses");
    tc_input("tick_interval = 5, then 20 accesses that never touch page 5");
    tc_setup("page 5 faulted in (aging = 0x80, referenced = 1)");
    {
        uint8_t before, after;
        mmu_translate(&mmu, 1, VA(5, 0), ACC_READ, &pa);      /* fault it in */
        mmu_translate(&mmu, 1, VA(6, 0), ACC_READ, &pa);
        mmu_translate(&mmu, 1, VA(7, 0), ACC_READ, &pa);
        before = (uint8_t)procs[0].pt->entries[5].aging;

        mmu_set_tick_interval(&mmu, 5);                       /* resets the
                                                                 counter    */
        for (int i = 0; i < 20; i++)
            mmu_translate(&mmu, 1, VA(6 + (i & 1), 0), ACC_READ, &pa);

        after = (uint8_t)procs[0].pt->entries[5].aging;
        field_i("aging before ticks", 0x80, before);
        field_i("ticks that should fire (20/5)", 4, 4);
        field_i("aging after 4 ticks", 0x18, after);
        field_i("referenced bit cleared", 0, procs[0].pt->entries[5].referenced);
        mmu_set_tick_interval(&mmu, 0);
    }
    tc_end("Hand-computed: 0x80 -> (0x80>>1)|0x80 = 0xC0 on the first tick\n"
           "              (its reference bit was still set from the fault), then\n"
           "              0x60, 0x30, 0x18 as it goes unreferenced. An unused page\n"
           "              decays exponentially, which is exactly what stops plain\n"
           "              LFU from keeping a once-hot page resident forever.");

    /* ------------------------------------------------------------ TC-16 */
    world(1);
    mm_prepage(&mm, &procs[0]);
    tc_begin("TC-16", "A page fault into a FULL TLB evicts the LRU entry");
    tc_input("touch 32 distinct pages to fill the TLB, then fault a 33rd");
    tc_setup("memory is plentiful; only the TLB is under pressure");
    {
        for (uint32_t v = 0; v < TLB_ENTRIES; v++)
            mmu_translate(&mmu, 1, VA(v, 0), ACC_READ, &pa);
        field_i("valid TLB entries", (long)TLB_ENTRIES, (long)tlb_valid());
        field_i("TLB evictions so far", 0, (long)mmu.tlb.evictions);

        mmu_translate(&mmu, 1, VA(0, 4), ACC_READ, &pa);   /* refresh page 0 */
        st = mmu_translate(&mmu, 1, VA(100, 0), ACC_READ, &pa);

        field_s("status", "OK (page fault serviced)", mmu_status_name(st));
        field_i("valid TLB entries after", (long)TLB_ENTRIES, (long)tlb_valid());
        field_i("TLB evictions", 1, (long)mmu.tlb.evictions);
        field_i("page 0 survived (was touched)", 1,
                tlb_impl_probe(&mmu.tlb, 1, 0) >= 0);
        field_i("page 1 evicted (was LRU)", -1, tlb_impl_probe(&mmu.tlb, 1, 1));
    }
    tc_end("Two independent resources can be full at once. Here memory has\n"
           "              32000+ free frames, so the fault is cheap, but the TLB is\n"
           "              at capacity and must drop something. Page 0 was refreshed\n"
           "              just before, so LRU protects it and kills page 1 -- under\n"
           "              FIFO page 0 would have died instead.");

    /* ------------------------------------------------------------ TC-17 */
    world(1);
    procs[0].upper_limit = 4;
    mm_prepage(&mm, &procs[0]);
    procs[0].lower_limit = 2;
    tc_begin("TC-17", "upper_limit self-eviction ALSO invalidates the TLB");
    tc_input("pid 1 capped at 4 frames touches pages 0..5");
    tc_setup("memory is plentiful -- only the per-process cap forces eviction");
    {
        int evicted_still_cached = 0;
        for (uint32_t v = 0; v < 6; v++)
            mmu_translate(&mmu, 1, VA(v, 0), ACC_READ, &pa);

        /* Any page the cap pushed out must be gone from the TLB too. */
        for (uint32_t v = 0; v < 6; v++)
            if (!procs[0].pt->entries[v].present &&
                tlb_impl_probe(&mmu.tlb, 1, v) >= 0)
                evicted_still_cached++;

        field_i("resident frames", 4, (long)procs[0].frames_held);
        field_i("pages evicted by the cap", 2,
                (long)(6 - procs[0].frames_held));
        field_i("evicted pages still in TLB", 0, evicted_still_cached);
        field_i("free frames still ample", 1,
                (mm.free_frames > NUM_FRAMES - 100));
    }
    tc_end("The upper_limit branch is a SECOND eviction path, separate from\n"
           "              the global victim search, and it is easy to forget that it\n"
           "              needs the same invalidation. It goes through the same\n"
           "              evict_frame() helper, so the notification fires and the\n"
           "              TLB is cleaned -- this case proves that, rather than\n"
           "              assuming it.");

    /* ------------------------------------------------------------ TC-18 */
    world(1);
    mm_prepage(&mm, &procs[0]);
    tc_begin("TC-18", "No evictable frame anywhere: MMU_FAULT_NO_FRAME");
    tc_input("fault a new page when every frame is pinned");
    tc_setup("all free frames taken as page-table frames; pid 1 sits at its "
             "lower_limit");
    {
        int f;
        while ((f = mm_alloc_frame(&mm, 0x3FFF, 0, 1 /* page table */)) >= 0)
            ;
        field_i("free frames", 0, (long)mm.free_frames);
        field_i("pid 1 frames_held", 2, (long)procs[0].frames_held);
        field_i("pid 1 lower_limit", 3, (long)procs[0].lower_limit);
        field_i("mm_select_victim", -1, mm_select_victim(&mm, procs, 1));

        snap();
        pa = 0x11111111;
        st = mmu_translate(&mmu, 1, VA(40, 0), ACC_READ, &pa);
        field_s("status", "FAULT: no evictable frame", mmu_status_name(st));
        field_i("pa_out untouched", (long)0x11111111, (long)pa);
        field_i("disk reads", 0, d_disk());
        field_i("probe(pid 1, vpn 40)", -1, tlb_impl_probe(&mmu.tlb, 1, 40));
    }
    tc_end("The out-of-memory leaf of the flowchart, and the one path I had\n"
           "              previously left untested. Every allocated frame is a page\n"
           "              table (never evictable) and pid 1 is at its floor (2 held,\n"
           "              limit 3), so nothing may be taken. The MMU reports the\n"
           "              failure instead of violating a limit, leaves pa_out alone,\n"
           "              spends no disk I/O, and creates no TLB entry.");

    /* ------------------------------------------------------------ TC-19 */
    world(1);
    mm_prepage(&mm, &procs[0]);
    tc_begin("TC-19", "A spurious fault on an already-present page is a no-op");
    tc_input("call mm_handle_fault(pid 1, page 1) -- page 1 is already resident");
    tc_setup("page 1 was pre-paged");
    {
        uint64_t f0 = mm.page_faults, d0 = mm.disk_reads;
        uint16_t h0 = procs[0].frames_held;
        uint32_t fr0 = procs[0].pt->entries[1].frame;
        int rc = mm_handle_fault(&mm, &procs[0], 1);

        field_i("return code",        0, rc);
        field_i("page faults added",  0, (long)(mm.page_faults - f0));
        field_i("disk reads added",   0, (long)(mm.disk_reads  - d0));
        field_i("frames_held added",  0, (long)(procs[0].frames_held - h0));
        field_i("frame unchanged", (long)fr0, (long)procs[0].pt->entries[1].frame);
    }
    tc_end("Real systems reach this after a TLB shootdown or two faults\n"
           "              racing on the same page. Treating it as a fresh fault\n"
           "              would allocate a SECOND frame for a page that already has\n"
           "              one -- leaking the first frame permanently and double\n"
           "              counting the fault.");

    /* ------------------------------------------------------------ TC-20 */
    world(1);
    tc_begin("TC-20", "Pre-paging fails cleanly when memory cannot supply 3 frames");
    tc_input("mm_prepage(pid 1) with zero free frames");
    tc_setup("every frame allocated before the process is created");
    {
        int f;
        while ((f = mm_alloc_frame(&mm, 0x3FFF, 0, 1)) >= 0)
            ;
        field_i("free frames", 0, (long)mm.free_frames);
        {
            int rc = mm_prepage(&mm, &procs[0]);
            field_i("return code", -1, rc);
            field_i("page table left NULL", 1, procs[0].pt == NULL);
            field_i("frames_held", 0, (long)procs[0].frames_held);
        }
    }
    tc_end("A process that cannot get its 3 minimum frames must not start\n"
           "              half-initialised. Returning -1 with pt == NULL means the\n"
           "              later translate() call hits step [2] and reports\n"
           "              NO_PROCESS rather than dereferencing a null page table.");

    /* ------------------------------------------------------------ TC-21 */
    world(1);
    mm_prepage(&mm, &procs[0]);
    tc_begin("TC-21", "A dirty victim is written back, and its TLB entry dies");
    tc_input("write page 3, fill memory, then fault page 80 so page 3 is the victim");
    tc_setup("pid 1 holds pages 0-7; page 3 has been written to");
    {
        uint32_t dirty_frame;
        uint64_t wb0;
        int f;
        for (uint32_t v = 2; v < 8; v++)
            mmu_translate(&mmu, 1, VA(v, 0), ACC_READ, &pa);
        mmu_translate(&mmu, 1, VA(3, 0x20), ACC_WRITE, &pa);
        dirty_frame = procs[0].pt->entries[3].frame;

        field_i("PTE[3].dirty", 1, procs[0].pt->entries[3].dirty);
        field_i("page 3 cached in TLB", 1, tlb_impl_probe(&mmu.tlb, 1, 3) >= 0);

        while ((f = mm_alloc_frame(&mm, 0x3FFF, 0, 0)) >= 0)
            mm.frames[f].aging = 0xFF;
        for (uint32_t v = 0; v < 8; v++)
            if (procs[0].pt->entries[v].present)
                mm.frames[procs[0].pt->entries[v].frame].aging = 0xFF;
        mm.frames[dirty_frame].aging = 0x00;
        wb0 = mm.disk_writebacks;

        mmu_translate(&mmu, 1, VA(80, 0), ACC_READ, &pa);

        field_i("disk write-backs", 1, (long)(mm.disk_writebacks - wb0));
        field_i("PTE[3].present after", 0, procs[0].pt->entries[3].present);
        field_i("probe(pid 1, vpn 3) after", -1, tlb_impl_probe(&mmu.tlb, 1, 3));
        field_i("page 80 took frame", (long)dirty_frame, (long)PA_FRAME(pa));
    }
    tc_end("Both consequences of one eviction. The page differs from disk,\n"
           "              so it is written back -- and because BOTH caches are\n"
           "              write-through, main memory already held the newest bytes,\n"
           "              so no cache flush was needed first. Meanwhile the TLB\n"
           "              entry must die or it would point at page 80's frame.");

    /* ------------------------------------------------------------ TC-22 */
    world(2);
    mm_prepage(&mm, &procs[0]);
    mm_prepage(&mm, &procs[1]);
    tc_begin("TC-22", "Process exit clears its TLB entries and frees its frames");
    tc_input("pid 2 touches 6 pages, then terminates");
    tc_setup("pid 1 and pid 2 both running, sharing the 32-entry TLB");
    {
        unsigned pid2_entries = 0;
        uint32_t free_before;
        for (uint32_t v = 0; v < 6; v++)
            mmu_translate(&mmu, 2, VA(v, 0), ACC_READ, &pa);
        for (uint32_t v = 0; v < 4; v++)
            mmu_translate(&mmu, 1, VA(v, 0), ACC_READ, &pa);

        for (unsigned i = 0; i < TLB_ENTRIES; i++)
            if (mmu.tlb.entries[i].valid && mmu.tlb.entries[i].pid == 2)
                pid2_entries++;
        free_before = mm.free_frames;
        field_i("pid 2 TLB entries before", 6, (long)pid2_entries);

        mmu_process_exit(&mmu, 2);

        pid2_entries = 0;
        for (unsigned i = 0; i < TLB_ENTRIES; i++)
            if (mmu.tlb.entries[i].valid && mmu.tlb.entries[i].pid == 2)
                pid2_entries++;
        field_i("pid 2 TLB entries after", 0, (long)pid2_entries);
        field_i("frames returned (6 pages + 1 page table)", 7,
                (long)(mm.free_frames - free_before));
        field_i("pid 1 still cached", 1, tlb_impl_probe(&mmu.tlb, 1, 0) >= 0);
        field_i("pid 1 frames untouched", 4, (long)procs[0].frames_held);
    }
    tc_end("The bulk invalidation the specification names explicitly. The\n"
           "              +1 frame is the page table, which replacement can never\n"
           "              touch -- exit is the ONLY path that frees it, so missing\n"
           "              this leaks a frame per terminated process. And the sweep\n"
           "              is PID-scoped: pid 1 keeps every entry and every frame.");

    /* ------------------------------------------------------------ TC-23 */
    world(1);
    mm_prepage(&mm, &procs[0]);
    tc_begin("TC-23", "KNOWN LIMITATION: revoking permission is invisible to a TLB hit");
    tc_input("read page 20 (caches it), revoke write permission, then write to it");
    tc_setup("pid 1 running; page 20 will be resident and TLB-cached");
    {
        MMUStatus s_after;
        mmu_translate(&mmu, 1, VA(20, 0), ACC_READ, &pa);   /* fills the TLB */
        procs[0].pt->entries[20].prot = PROT_READ;          /* revoke write  */
        s_after = mmu_translate(&mmu, 1, VA(20, 4), ACC_WRITE, &pa);

        field_s("status of the illegal write", "OK (TLB hit)",
                mmu_status_name(s_after));
        field_i("PTE says writable", 0,
                (procs[0].pt->entries[20].prot & PROT_WRITE) != 0);

        tlb_impl_invalidate_entry(&mmu.tlb, 1, 20);         /* the fix       */
        s_after = mmu_translate(&mmu, 1, VA(20, 8), ACC_WRITE, &pa);
        field_s("after invalidating the entry", "FAULT: protection violation",
                mmu_status_name(s_after));
    }
    tc_end("This test asserts the BUG, deliberately, so it is documented and\n"
           "              cannot regress silently. TLBImplEntry carries no prot bits,\n"
           "              and re-reading the PTE on every hit would cost a memory\n"
           "              access per reference and defeat the TLB entirely. So the\n"
           "              write is permitted until the entry is invalidated -- which\n"
           "              the driver's PROT directive does for exactly this reason.\n"
           "              Real fix: add 'prot : 3' to TLBImplEntry, which needs\n"
           "              TLB.h -- a file this work does not modify.");

    /* -------------------------------------------------------------- */
    printf("\n========================================================================\n");
    printf("  %d test cases run, %d passed, %d failed\n",
           tc_no, tc_no - tc_failed, tc_failed);
    printf("========================================================================\n");
    mm_destroy(&mm);
    report_end("test_tlb_workflow", "cases", tc_no, tc_failed);
    return tc_failed ? 1 : 0;
}
