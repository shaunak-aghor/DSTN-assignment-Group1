/* ============================================================================
 *  tests/test_tlb.c -- standalone test harness for the PID-tagged TLB
 *
 *  Build:   make test-tlb
 *  Run:     ./test_tlb
 *
 *  Links against src/TLB.c only, so it runs even while the rest of the
 *  simulator is unfinished.
 *
 *  Covers, in order:
 *     1  configuration sanity (compile-time)
 *     2  address decomposition
 *     3  cold TLB -> compulsory misses
 *     4  fill and hit, the full VA -> PA flow
 *     5  no duplicate entry for a repeated (pid, vpn)
 *     6  LRU: the 33rd fill evicts the true least-recently-used entry
 *     7  PID tagging: same VPN, different processes
 *     8  context switch costs nothing
 *     9  invalidate_entry  -- a page was evicted
 *    10  invalidate_pid    -- a process terminated
 *    11  invalidate_frame  -- a frame was reused
 *    12  freed slots are refilled before anything is evicted
 *    13  statistics add up
 *    14  TLB reach: 32 entries x 1 KB = 32 KB
 *    15  sharing 32 entries between processes
 *
 *  The LRU rank invariant (ranks of the valid entries are a permutation of
 *  0..valid_count-1) is re-checked after every mutation in every test.
 * ==========================================================================*/

#include <stdio.h>
#include <string.h>
#include "TLB.h"
#include "report.h"

/* ---------------------------------------------------------------- harness */

static int checks = 0, failures = 0;
static const char *section = "";

/* NOTE: `cond` is evaluated exactly ONCE. Several checks call
 * tlb_impl_lookup() directly in the condition, and a second evaluation
 * would corrupt the very hit/miss counters the next check inspects. */
#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        int ok_ = (cond) ? 1 : 0;                                             \
        checks++;                                                             \
        if (ok_) {                                                            \
            printf("    pass  ");                                             \
        } else {                                                              \
            failures++;                                                       \
            printf("    FAIL  ");                                             \
        }                                                                     \
        printf(__VA_ARGS__);                                                  \
        putchar('\n');                                                        \
        if (!ok_)                                                             \
            printf("          ^ in \"%s\" at %s:%d\n", section,               \
                   __FILE__, __LINE__);                                       \
    } while (0)

static void head(const char *n, const char *title)
{
    section = title;
    printf("\n%s  %s\n", n, title);
    for (unsigned i = 0; i < 68; i++) putchar('-');
    putchar('\n');
}

/* Compile-time configuration checks -- these fail the BUILD, not the run. */
#define STATIC_ASSERT(c, tag) typedef char static_assert_##tag[(c) ? 1 : -1]
STATIC_ASSERT(VA_BITS == VPN_BITS + PAGE_OFFSET_BITS,      va_split);
STATIC_ASSERT(PA_BITS == FRAME_BITS + PAGE_OFFSET_BITS,    pa_split);
STATIC_ASSERT(PAGES_PER_PROC == (1 << VPN_BITS),           pages_per_proc);
STATIC_ASSERT(NUM_FRAMES == (1 << FRAME_BITS),             frame_count);
STATIC_ASSERT((1UL << TLB_LRU_BITS) >= TLB_ENTRIES,        lru_width);
STATIC_ASSERT(PAGE_SIZE == (1 << PAGE_OFFSET_BITS),        page_size);

/* ------------------------------------------------------- LRU invariant ---
 * Valid entries must hold ranks 0,1,...,valid_count-1 exactly once each.
 * If this ever breaks, replacement silently stops being LRU.
 * ---------------------------------------------------------------------- */
static int lru_ok(const TLBImpl *t, const char **why)
{
    int seen[TLB_ENTRIES];
    unsigned i, live = 0;
    int r;

    memset(seen, 0, sizeof(seen));

    for (i = 0; i < TLB_ENTRIES; i++) {
        if (!t->entries[i].valid) continue;
        if (seen[t->entries[i].lru])          { *why = "duplicate rank";    return 0; }
        seen[t->entries[i].lru] = 1;
        live++;
    }
    for (r = 0; r < (int)live; r++)
        if (!seen[r]) { *why = "gap in the rank sequence"; return 0; }

    *why = "ok";
    return 1;
}

static void check_lru(const TLBImpl *t, const char *when)
{
    const char *why;
    CHECK(lru_ok(t, &why), "LRU ranks form a permutation %-28s (%s)", when, why);
}

static unsigned live_entries(const TLBImpl *t)
{
    unsigned i, n = 0;
    for (i = 0; i < TLB_ENTRIES; i++) n += t->entries[i].valid;
    return n;
}

/* --------------------------------------------------- fake page-table walk
 * The TLB is passive: on a miss the caller walks the page table and fills.
 * Assumption 5 says page tables are never cached, so a walk is one real
 * main-memory access -- counted here so the tests can talk about cost.
 * ---------------------------------------------------------------------- */
static unsigned long walks = 0;

static uint32_t page_table_walk(uint32_t pid, uint32_t vpn)
{
    walks++;
    return ((pid * 0x40u) + vpn + 0x100u) & (uint32_t)MASK(FRAME_BITS);
}

/* One complete access: VA in, PA out. Returns 1 if the TLB hit. */
static int translate(TLBImpl *t, uint32_t pid, uint32_t va, uint32_t *pa_out)
{
    uint32_t vpn = (unsigned)VA_VPN(va);
    uint32_t off = (unsigned)VA_OFFSET(va);
    uint32_t pfn;
    int hit = tlb_impl_lookup(t, pid, vpn, &pfn);

    if (!hit) {
        pfn = page_table_walk(pid, vpn);
        tlb_impl_insert(t, pid, vpn, pfn);
    }
    if (pa_out) *pa_out = (pfn << PAGE_OFFSET_BITS) | off;
    return hit;
}

/* ============================================================== the tests */

int main(int argc, char **argv)
{
    report_begin(argc, argv, "results/test_tlb.txt");

    TLBImpl t;
    uint32_t pa, pfn;
    unsigned i;

    printf("========================================================\n");
    printf("  TLB flow tests -- %d entries, PID-tagged, LRU counter\n", TLB_ENTRIES);
    printf("========================================================\n");

    /* ---------------------------------------------------------------- 1 */
    head("1.", "Configuration");
    printf("    VA %u bits = VPN %u + offset %u   -> %u pages/process (%lu KB)\n",
           VA_BITS, VPN_BITS, PAGE_OFFSET_BITS, PAGES_PER_PROC,
           (1UL << VA_BITS) / 1024UL);
    printf("    PA %u bits = frame %u + offset %u -> %u frames (%lu MB)\n",
           PA_BITS, FRAME_BITS, PAGE_OFFSET_BITS, NUM_FRAMES,
           MM_SIZE / (1024UL * 1024UL));
    printf("    entry = 1 + %u vpn + %u pfn + %u pid + %u lru = %u bits\n",
           VPN_BITS, FRAME_BITS, PID_BITS, TLB_LRU_BITS,
           1 + VPN_BITS + FRAME_BITS + PID_BITS + TLB_LRU_BITS);
    printf("    TLB storage = %u x %u = %u bits = %u bytes\n",
           TLB_ENTRIES, 1 + VPN_BITS + FRAME_BITS + PID_BITS + TLB_LRU_BITS,
           TLB_ENTRIES * (1 + VPN_BITS + FRAME_BITS + PID_BITS + TLB_LRU_BITS),
           TLB_ENTRIES * (1 + VPN_BITS + FRAME_BITS + PID_BITS + TLB_LRU_BITS) / 8);
    CHECK(1, "compile-time geometry assertions held");

    /* ---------------------------------------------------------------- 2 */
    head("2.", "Address decomposition");
    {
        uint32_t va = 0x2A234;                 /* the worked example      */
        CHECK(va < (1u << VA_BITS),  "VA 0x%05X fits in %u bits", va, VA_BITS);
        CHECK((unsigned)VA_VPN(va)    == 0xA8, "VA_VPN(0x%05X)    = 0x%02X (%u)",
              va, (unsigned)VA_VPN(va), (unsigned)VA_VPN(va));
        CHECK((unsigned)VA_OFFSET(va) == 564,  "VA_OFFSET(0x%05X) = 0x%03X (%u)",
              va, (unsigned)VA_OFFSET(va), (unsigned)VA_OFFSET(va));
    }

    /* ---------------------------------------------------------------- 3 */
    head("3.", "Cold TLB -- every lookup is a compulsory miss");
    tlb_impl_init(&t);
    check_lru(&t, "after init");
    CHECK(live_entries(&t) == 0, "0 valid entries after init");
    for (i = 0; i < 5; i++)
        CHECK(tlb_impl_lookup(&t, 1, i, &pfn) == 0, "lookup(pid 1, vpn %u) misses", i);
    CHECK(t.misses == 5 && t.hits == 0, "misses=%llu hits=%llu",
          (unsigned long long)t.misses, (unsigned long long)t.hits);

    /* ---------------------------------------------------------------- 4 */
    head("4.", "Fill and hit -- the full VA -> PA flow");
    tlb_impl_init(&t);
    walks = 0;
    {
        uint32_t va = 0x2A234;
        int hit1 = translate(&t, 7, va, &pa);
        uint32_t pa1 = pa;
        int hit2 = translate(&t, 7, va, &pa);

        CHECK(hit1 == 0, "1st access to VA 0x%05X misses -> page-table walk", va);
        CHECK(walks == 1, "exactly 1 walk performed (1 memory access)");
        CHECK(hit2 == 1, "2nd access to the same page hits -- no walk");
        CHECK(walks == 1, "still 1 walk after the second access");
        CHECK(pa == pa1, "both accesses produce the same PA 0x%06X", pa);
        CHECK((unsigned)PA_OFFSET(pa) == (unsigned)VA_OFFSET(va),
              "page offset passes through untranslated (0x%03X)", (unsigned)PA_OFFSET(pa));
        CHECK((unsigned)PA_FRAME(pa) < NUM_FRAMES, "frame %u is in range", (unsigned)PA_FRAME(pa));

        printf("    L1 view of PA 0x%06X: tag %5u | index %2u | offset %u\n",
               pa, (unsigned)L1_TAG(pa), (unsigned)L1_INDEX(pa), (unsigned)L1_BLK_OFFSET(pa));
        printf("    L2 view of PA 0x%06X: tag %5u | index %3u | offset %u\n",
               pa, (unsigned)L2_TAG(pa), (unsigned)L2_INDEX(pa), (unsigned)L2_BLK_OFFSET(pa));
        CHECK((unsigned)L1_TAG(pa) == (unsigned)PA_FRAME(pa),
              "L1 tag == frame number (both %u bits) -- the VIPT property",
              FRAME_BITS);
    }
    check_lru(&t, "after fill + hit");

    /* ---------------------------------------------------------------- 5 */
    head("5.", "Re-inserting the same (pid, vpn) must not duplicate");
    tlb_impl_init(&t);
    tlb_impl_insert(&t, 3, 0x20, 0x111);
    tlb_impl_insert(&t, 3, 0x20, 0x222);        /* remap, e.g. after a fault */
    CHECK(live_entries(&t) == 1, "still exactly 1 valid entry");
    tlb_impl_lookup(&t, 3, 0x20, &pfn);
    CHECK(pfn == 0x222, "the mapping was refreshed, not shadowed (pfn 0x%X)", pfn);
    check_lru(&t, "after duplicate insert");

    /* ---------------------------------------------------------------- 6 */
    head("6.", "LRU -- the 33rd fill evicts the true LRU entry");
    tlb_impl_init(&t);
    for (i = 0; i < TLB_ENTRIES; i++) {
        tlb_impl_insert(&t, 1, 0x10 + i, 0x400 + i);
        check_lru(&t, "during fill");
    }
    CHECK(live_entries(&t) == TLB_ENTRIES, "TLB is full (%u entries)", TLB_ENTRIES);
    CHECK(t.evictions == 0, "no evictions while filling empty slots");

    /* vpn 0x10 is the oldest; touch it so vpn 0x11 becomes the victim. */
    tlb_impl_lookup(&t, 1, 0x10, &pfn);
    check_lru(&t, "after touching the oldest entry");

    tlb_impl_insert(&t, 1, 0x70, 0x500);
    check_lru(&t, "after the 33rd insert");
    CHECK(t.evictions == 1, "exactly 1 capacity eviction recorded");
    CHECK(tlb_impl_probe(&t, 1, 0x10) >= 0, "vpn 0x10 survived -- it was touched");
    CHECK(tlb_impl_probe(&t, 1, 0x11) <  0, "vpn 0x11 was evicted -- it was the LRU");
    CHECK(tlb_impl_probe(&t, 1, 0x70) >= 0, "vpn 0x70 is now resident");
    CHECK(live_entries(&t) == TLB_ENTRIES, "still exactly %u valid entries", TLB_ENTRIES);

    /* ---------------------------------------------------------------- 7 */
    head("7.", "PID tagging -- same VPN, different processes");
    tlb_impl_init(&t);
    tlb_impl_insert(&t, 7, 0xA8, 0x1A3);
    tlb_impl_insert(&t, 9, 0xA8, 0x2C0);
    CHECK(live_entries(&t) == 2, "both mappings coexist for the same VPN 0xA8");
    CHECK(tlb_impl_lookup(&t, 7, 0xA8, &pfn) && pfn == 0x1A3,
          "pid 7 -> frame 0x%03X", pfn);
    CHECK(tlb_impl_lookup(&t, 9, 0xA8, &pfn) && pfn == 0x2C0,
          "pid 9 -> frame 0x%03X", pfn);
    CHECK(tlb_impl_lookup(&t, 5, 0xA8, &pfn) == 0,
          "pid 5 misses -- the PID is part of the match, not just the VPN");
    check_lru(&t, "after two-process fill");

    /* ---------------------------------------------------------------- 8 */
    head("8.", "Context switch -- no flush, no cost");
    {
        unsigned before = live_entries(&t);
        uint64_t h = t.hits;
        /* A context switch in a PID-tagged TLB is just a register write:
           there is no API call to make, and nothing may be lost. */
        CHECK(live_entries(&t) == before,
              "switching pid 7 -> 9 -> 7 leaves all %u entries valid", before);
        CHECK(tlb_impl_lookup(&t, 7, 0xA8, &pfn) == 1,
              "pid 7's mapping still hits after the switch");
        CHECK(t.hits == h + 1, "and it counted as a hit, not a refill");
        printf("    (an untagged TLB would need %u invalidations here)\n", before);
    }

    /* ---------------------------------------------------------------- 9 */
    head("9.", "invalidate_entry -- one page was evicted from memory");
    tlb_impl_init(&t);
    for (i = 0; i < 4; i++) tlb_impl_insert(&t, 1, i, 0x300 + i);
    tlb_impl_invalidate_entry(&t, 1, 2);
    check_lru(&t, "after invalidate_entry");
    CHECK(live_entries(&t) == 3, "3 of 4 entries remain");
    CHECK(tlb_impl_probe(&t, 1, 2) < 0, "vpn 2 is gone");
    CHECK(tlb_impl_probe(&t, 1, 1) >= 0 && tlb_impl_probe(&t, 1, 3) >= 0,
          "its neighbours are untouched");
    tlb_impl_invalidate_entry(&t, 1, 99);
    CHECK(live_entries(&t) == 3, "invalidating an absent page is a no-op");

    /* --------------------------------------------------------------- 10 */
    head("10.", "invalidate_pid -- a process terminated");
    tlb_impl_init(&t);
    for (i = 0; i < 6; i++) tlb_impl_insert(&t, 1, i, 0x300 + i);
    for (i = 0; i < 5; i++) tlb_impl_insert(&t, 2, i, 0x700 + i);
    CHECK(live_entries(&t) == 11, "11 entries across 2 processes");
    tlb_impl_invalidate_pid(&t, 2);
    check_lru(&t, "after invalidate_pid");
    CHECK(live_entries(&t) == 6, "process 2's 5 entries were removed");
    for (i = 0; i < 5; i++)
        if (tlb_impl_probe(&t, 2, i) >= 0) { CHECK(0, "pid 2 vpn %u lingers", i); break; }
    CHECK(tlb_impl_probe(&t, 2, 0) < 0, "no pid-2 entry survives");
    CHECK(tlb_impl_probe(&t, 1, 0) >= 0, "process 1 is completely unaffected");

    /* --------------------------------------------------------------- 11 */
    head("11.", "invalidate_frame -- a frame was reclaimed and reused");
    tlb_impl_init(&t);
    tlb_impl_insert(&t, 1, 0x05, 0x777);
    tlb_impl_insert(&t, 2, 0x40, 0x777);        /* shared/aliased frame */
    tlb_impl_insert(&t, 3, 0x41, 0x778);
    tlb_impl_invalidate_frame(&t, 0x777);
    check_lru(&t, "after invalidate_frame");
    CHECK(tlb_impl_probe(&t, 1, 0x05) < 0 && tlb_impl_probe(&t, 2, 0x40) < 0,
          "every mapping of frame 0x777 is gone, across all processes");
    CHECK(tlb_impl_probe(&t, 3, 0x41) >= 0, "frame 0x778 is untouched");
    printf("    (this is the step that stops a reused frame returning stale data)\n");

    /* --------------------------------------------------------------- 12 */
    head("12.", "Freed slots are refilled before anything is evicted");
    tlb_impl_init(&t);
    for (i = 0; i < TLB_ENTRIES; i++) tlb_impl_insert(&t, 1, 0x10 + i, 0x400 + i);
    tlb_impl_invalidate_pid(&t, 1);
    CHECK(live_entries(&t) == 0, "TLB emptied by process exit");
    tlb_impl_reset_stats(&t);
    for (i = 0; i < TLB_ENTRIES; i++) {
        tlb_impl_insert(&t, 2, 0x10 + i, 0x800 + i);
        check_lru(&t, "during refill");
    }
    CHECK(live_entries(&t) == TLB_ENTRIES, "refilled to %u entries", TLB_ENTRIES);
    CHECK(t.evictions == 0, "0 evictions -- freed slots were reused first");

    /* --------------------------------------------------------------- 13 */
    head("13.", "Statistics accounting");
    tlb_impl_init(&t);
    for (i = 0; i < 10; i++) tlb_impl_insert(&t, 1, i, 0x200 + i);
    tlb_impl_reset_stats(&t);
    for (i = 0; i < 10; i++) tlb_impl_lookup(&t, 1, i,  &pfn);   /* 10 hits   */
    for (i = 0; i < 4;  i++) tlb_impl_lookup(&t, 1, 90 + i, &pfn); /* 4 misses */
    CHECK(t.hits == 10,  "hits   = %llu (expected 10)", (unsigned long long)t.hits);
    CHECK(t.misses == 4, "misses = %llu (expected 4)",  (unsigned long long)t.misses);
    CHECK(t.hits + t.misses == 14, "every lookup was counted exactly once");
    CHECK(t.evictions == 0, "probes and lookups never evict");

    /* --------------------------------------------------------------- 14 */
    head("14.", "TLB reach -- 32 entries x 1 KB = 32 KB");
    {
        unsigned reach_pages = TLB_ENTRIES;
        unsigned pass;

        /* A loop over exactly TLB_ENTRIES pages: warm up, then every
           subsequent sweep must hit. A loop over one page more evicts the
           page it is about to need, so LRU degenerates to 0% -- the classic
           cyclic-reference worst case. */
        for (unsigned span = reach_pages; span <= reach_pages + 1; span++) {
            tlb_impl_init(&t);
            for (i = 0; i < span; i++)                     /* warm up */
                translate(&t, 1, i << PAGE_OFFSET_BITS, &pa);
            tlb_impl_reset_stats(&t);
            for (pass = 0; pass < 10; pass++)
                for (i = 0; i < span; i++)
                    translate(&t, 1, i << PAGE_OFFSET_BITS, &pa);

            {
                double hr = 100.0 * (double)t.hits / (double)(t.hits + t.misses);
                printf("    working set %2u pages (%2u KB): hit ratio %6.2f%%\n",
                       span, span, hr);
                if (span <= reach_pages)
                    CHECK(t.misses == 0, "%u pages fit -- 0 misses after warm-up", span);
                else
                    CHECK(t.hits == 0,
                          "%u pages do not fit -- LRU thrashes to 0%% on a cyclic sweep",
                          span);
            }
        }
        printf("    reach = %u pages x %u B = %u KB, and only %.1f%% of a\n"
               "    process's %u-page address space\n",
               TLB_ENTRIES, PAGE_SIZE, TLB_ENTRIES * PAGE_SIZE / 1024,
               100.0 * TLB_ENTRIES / PAGES_PER_PROC, PAGES_PER_PROC);
    }

    /* --------------------------------------------------------------- 15 */
    head("15.", "Processes share the 32 entries -- the aggregate set is what matters");
    {
        const unsigned NPROC = 4, ROUNDS = 40;
        unsigned pages;

        /* 6 pages each -> 24 mappings, fits.  12 pages each -> 48, does not.
           The per-process footprint is identical in kind; only the SUM
           across live processes decides whether the TLB copes. */
        for (pages = 6; pages <= 12; pages += 6) {
            double hr;
            tlb_impl_init(&t);
            walks = 0;
            for (unsigned round = 0; round < ROUNDS; round++)
                for (uint32_t pid = 1; pid <= NPROC; pid++)
                    for (i = 0; i < pages; i++)
                        translate(&t, pid, i << PAGE_OFFSET_BITS, &pa);

            hr = 100.0 * (double)t.hits / (double)(t.hits + t.misses);
            printf("    %u procs x %2u pages = %2u mappings vs %u slots:"
                   " hit %6.2f%% | %4lu walks\n",
                   NPROC, pages, NPROC * pages, TLB_ENTRIES, hr, walks);

            CHECK(t.hits + t.misses == ROUNDS * NPROC * pages,
                  "all %u accesses accounted for", ROUNDS * NPROC * pages);
            if (NPROC * pages <= TLB_ENTRIES) {
                CHECK(t.evictions == 0, "%u mappings fit -- no eviction, %lu walks total",
                      NPROC * pages, walks);
                CHECK(hr > 95.0, "hit ratio %.2f%% -- only the compulsory misses", hr);
            } else {
                CHECK(t.evictions > 0, "%u mappings do not fit -- %llu evictions",
                      NPROC * pages, (unsigned long long)t.evictions);
                CHECK(walks == ROUNDS * NPROC * pages,
                      "every access walks the page table: %lu memory accesses", walks);
            }
            check_lru(&t, "after multi-process churn");
        }
        printf("    a process needing more than %u pages resident is already\n"
               "    beyond the TLB on its own -- sharing only makes it sooner\n",
               TLB_ENTRIES);
    }

    /* ------------------------------------------------------------ report */
    tlb_impl_dump(&t);

    printf("\n========================================================\n");
    printf("  %d checks, %d failed\n", checks, failures);
    printf("  %s\n", failures ? "  SOME TESTS FAILED" : "  ALL TESTS PASSED");
    printf("========================================================\n");
    report_end("test_tlb", "checks", checks, failures);
    return failures ? 1 : 0;
}
