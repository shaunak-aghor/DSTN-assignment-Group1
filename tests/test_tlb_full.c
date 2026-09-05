/* ============================================================================
 *  tests/test_tlb_full.c -- exhaustive scenario coverage for the TLB
 *
 *  tests/test_tlb.c checks hand-picked cases. This file attacks the same code
 *  two other ways:
 *
 *    1. BOUNDARY SCENARIOS -- the extreme values of every bit field, and what
 *       happens when a caller passes something too wide for the field.
 *
 *    2. DIFFERENTIAL TESTING -- an independent reference model of a PID-tagged
 *       fully-associative LRU TLB, written in the most obvious way possible
 *       (an ordered MRU list, no cleverness). Both are driven with the same
 *       randomized operation stream and compared after EVERY operation on
 *       every observable: hit/miss, the frame returned, the resident set, and
 *       the exact recency ordering. A divergence is reported with the full
 *       state of both.
 *
 *  The random stream is a self-contained xorshift with a fixed seed, so runs
 *  are reproducible on any platform -- unlike rand(), which differs by libc.
 *
 *  Build:  gcc -Wall -Wextra -g -I./include -o test_tlb_full \
 *              tests/test_tlb_full.c src/TLB.c
 * ==========================================================================*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "TLB.h"
#include "report.h"

/* ---------------------------------------------------------------- harness */

static int scenarios = 0, scen_failed = 0;
static const char *cur = "";

static void scenario(const char *name)
{
    cur = name;
    scenarios++;
    printf("\nS%-2d  %s\n", scenarios, name);
    for (int i = 0; i < 72; i++) putchar('-');
    putchar('\n');
}

static int  local_fail;
#define REQUIRE(cond, ...)                                                    \
    do {                                                                      \
        int ok_ = (cond) ? 1 : 0;                                             \
        printf(ok_ ? "     ok   " : "     BAD  ");                            \
        printf(__VA_ARGS__);                                                  \
        putchar('\n');                                                        \
        if (!ok_) { local_fail = 1;                                           \
                    printf("          ^ %s:%d\n", __FILE__, __LINE__); }      \
    } while (0)

static void scenario_end(void)
{
    if (local_fail) { scen_failed++; printf("     => SCENARIO FAILED\n"); }
    else            { printf("     => scenario passed\n"); }
    local_fail = 0;
    (void)cur;
}

/* ------------------------------------------------- reproducible randomness */

static uint32_t rng_state = 0x2A234u;

static uint32_t rnd(void)
{
    uint32_t x = rng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return (rng_state = x);
}
static uint32_t rnd_below(uint32_t n) { return rnd() % n; }

/* ==========================================================================
 *  THE REFERENCE MODEL
 *
 *  A PID-tagged fully-associative LRU TLB written the naive way: an array
 *  kept in strict most-recently-used-first order. No counters, no ranks, no
 *  in-place updates -- every operation is a list shuffle. It is obviously
 *  correct at a glance, which is the entire point: it has no bugs to share
 *  with the implementation under test.
 * ========================================================================*/

typedef struct { uint32_t pid, vpn, pfn; } Ref;

typedef struct {
    Ref      e[TLB_ENTRIES];       /* e[0] = most recently used              */
    unsigned n;
    uint64_t hits, misses, evictions;
} RefTLB;

static void ref_init(RefTLB *r) { memset(r, 0, sizeof(*r)); }

static int ref_find(const RefTLB *r, uint32_t pid, uint32_t vpn)
{
    for (unsigned i = 0; i < r->n; i++)
        if (r->e[i].pid == pid && r->e[i].vpn == vpn) return (int)i;
    return -1;
}

static void ref_move_front(RefTLB *r, unsigned i)
{
    Ref tmp = r->e[i];
    for (unsigned j = i; j > 0; j--) r->e[j] = r->e[j - 1];
    r->e[0] = tmp;
}

static void ref_remove(RefTLB *r, unsigned i)
{
    for (unsigned j = i; j + 1 < r->n; j++) r->e[j] = r->e[j + 1];
    r->n--;
}

static int ref_lookup(RefTLB *r, uint32_t pid, uint32_t vpn, uint32_t *pfn)
{
    int i = ref_find(r, pid, vpn);
    if (i < 0) { r->misses++; return 0; }
    if (pfn) *pfn = r->e[i].pfn;
    ref_move_front(r, (unsigned)i);
    r->hits++;
    return 1;
}

static void ref_insert(RefTLB *r, uint32_t pid, uint32_t vpn, uint32_t pfn)
{
    int i = ref_find(r, pid, vpn);
    if (i >= 0) {                              /* refresh, never duplicate */
        r->e[i].pfn = pfn;
        ref_move_front(r, (unsigned)i);
        return;
    }
    /* A full TLB stays FULL: the shift below pushes the least recently used
     * entry off the end, and the new one takes position 0. Decrementing n
     * here would LOSE an entry instead of replacing one -- that was a real
     * bug in this model, and the differential test caught it on op 184. */
    if (r->n == TLB_ENTRIES) r->evictions++;
    else                     r->n++;
    for (unsigned j = r->n - 1; j > 0; j--) r->e[j] = r->e[j - 1];
    r->e[0].pid = pid; r->e[0].vpn = vpn; r->e[0].pfn = pfn;
}

static void ref_inval_entry(RefTLB *r, uint32_t pid, uint32_t vpn)
{
    int i = ref_find(r, pid, vpn);
    if (i >= 0) ref_remove(r, (unsigned)i);
}

static void ref_inval_pid(RefTLB *r, uint32_t pid)
{
    for (unsigned i = 0; i < r->n; )
        if (r->e[i].pid == pid) ref_remove(r, i); else i++;
}

static void ref_inval_frame(RefTLB *r, uint32_t pfn)
{
    for (unsigned i = 0; i < r->n; )
        if (r->e[i].pfn == pfn) ref_remove(r, i); else i++;
}

/* ==========================================================================
 *  COMPARISON
 *
 *  The implementation stores recency as a rank per entry; the model stores it
 *  as list position. Sorting the valid entries by ascending rank must
 *  reproduce the model's list EXACTLY -- same entries, same order, same
 *  frames. That is a far stronger claim than "the same pages are resident".
 * ========================================================================*/

static unsigned impl_order(const TLBImpl *t, Ref *out)
{
    unsigned n = 0;
    for (unsigned rank = 0; rank < TLB_ENTRIES; rank++)
        for (unsigned i = 0; i < TLB_ENTRIES; i++)
            if (t->entries[i].valid && t->entries[i].lru == rank) {
                out[n].pid = t->entries[i].pid;
                out[n].vpn = t->entries[i].vpn;
                out[n].pfn = t->entries[i].pfn;
                n++;
            }
    return n;
}

static unsigned impl_valid(const TLBImpl *t)
{
    unsigned n = 0;
    for (unsigned i = 0; i < TLB_ENTRIES; i++) n += t->entries[i].valid;
    return n;
}

/* Returns 0 on agreement, or a description of the first divergence. */
static const char *compare(const TLBImpl *t, const RefTLB *r)
{
    static char msg[256];
    Ref got[TLB_ENTRIES];
    unsigned n = impl_order(t, got);

    if (impl_valid(t) != n)
        return "two valid entries share an LRU rank (ordering is ambiguous)";

    if (n != r->n) {
        snprintf(msg, sizeof(msg),
                 "resident count: implementation %u, model %u", n, r->n);
        return msg;
    }
    for (unsigned i = 0; i < n; i++) {
        if (got[i].pid != r->e[i].pid || got[i].vpn != r->e[i].vpn) {
            snprintf(msg, sizeof(msg),
                     "recency position %u: implementation (pid %u, vpn %u), "
                     "model (pid %u, vpn %u)",
                     i, got[i].pid, got[i].vpn, r->e[i].pid, r->e[i].vpn);
            return msg;
        }
        if (got[i].pfn != r->e[i].pfn) {
            snprintf(msg, sizeof(msg),
                     "frame at position %u: implementation %u, model %u",
                     i, got[i].pfn, r->e[i].pfn);
            return msg;
        }
    }
    return NULL;
}

/* ==========================================================================
 *  Differential driver
 * ========================================================================*/

typedef struct {
    unsigned pid_space, vpn_space;   /* how much the workload can address   */
    unsigned w_lookup, w_insert, w_inval_e, w_inval_pid, w_inval_frame;
    const char *name;
} Mix;

static int run_differential(const Mix *mix, unsigned long ops, uint32_t seed)
{
    TLBImpl t;
    RefTLB  r;
    unsigned long i;
    unsigned total = mix->w_lookup + mix->w_insert + mix->w_inval_e +
                     mix->w_inval_pid + mix->w_inval_frame;
    unsigned long n_lookup = 0, n_insert = 0, n_inval = 0, n_hit = 0;

    rng_state = seed;
    tlb_impl_init(&t);
    ref_init(&r);

    for (i = 0; i < ops; i++) {
        uint32_t pid = 1 + rnd_below(mix->pid_space);
        uint32_t vpn = rnd_below(mix->vpn_space);
        uint32_t pfn = rnd_below(64) + 0x100;
        uint32_t pick = rnd_below(total);
        const char *bad;
        const char *what;

        if (pick < mix->w_lookup) {
            uint32_t a = 0xAAAA, b = 0xBBBB;
            int ha = tlb_impl_lookup(&t, pid, vpn, &a);
            int hb = ref_lookup(&r, pid, vpn, &b);
            what = "lookup";
            n_lookup++; n_hit += (unsigned)hb;
            if (ha != hb) {
                printf("     BAD  op %lu %s(pid %u, vpn %u): implementation "
                       "said %s, model said %s\n",
                       i, what, pid, vpn, ha ? "HIT" : "miss",
                       hb ? "HIT" : "miss");
                return 0;
            }
            if (ha && a != b) {
                printf("     BAD  op %lu %s(pid %u, vpn %u): frame %u vs %u\n",
                       i, what, pid, vpn, a, b);
                return 0;
            }
        } else if (pick < mix->w_lookup + mix->w_insert) {
            tlb_impl_insert(&t, pid, vpn, pfn);
            ref_insert(&r, pid, vpn, pfn);
            what = "insert"; n_insert++;
        } else if (pick < mix->w_lookup + mix->w_insert + mix->w_inval_e) {
            tlb_impl_invalidate_entry(&t, pid, vpn);
            ref_inval_entry(&r, pid, vpn);
            what = "invalidate_entry"; n_inval++;
        } else if (pick < mix->w_lookup + mix->w_insert + mix->w_inval_e +
                          mix->w_inval_pid) {
            tlb_impl_invalidate_pid(&t, pid);
            ref_inval_pid(&r, pid);
            what = "invalidate_pid"; n_inval++;
        } else {
            tlb_impl_invalidate_frame(&t, pfn);
            ref_inval_frame(&r, pfn);
            what = "invalidate_frame"; n_inval++;
        }

        bad = compare(&t, &r);
        if (bad) {
            printf("     BAD  divergence after op %lu (%s pid %u vpn %u): %s\n",
                   i, what, pid, vpn, bad);
            return 0;
        }
    }

    printf("     ok   %-26s %7lu ops  (%lu lookups, %.1f%% hit | "
           "%lu inserts | %lu invalidations)\n",
           mix->name, ops, n_lookup,
           n_lookup ? 100.0 * (double)n_hit / (double)n_lookup : 0.0,
           n_insert, n_inval);
    printf("          every operation agreed with the reference model on\n"
           "          hit/miss, frame, resident set AND recency order\n");
    return 1;
}

/* ========================================================================== */

int main(int argc, char **argv)
{
    report_begin(argc, argv, "results/test_tlb_full.txt");

    TLBImpl t;
    uint32_t pfn;

    printf("=========================================================================\n");
    printf("  TLB -- exhaustive scenario coverage\n");
    printf("  boundary values, field truncation, PID reuse, differential testing\n");
    printf("=========================================================================\n");

    /* ------------------------------------------------------------------ */
    scenario("Boundary: the first and last page of an address space");
    tlb_impl_init(&t);
    tlb_impl_insert(&t, 1, 0, 0x111);
    tlb_impl_insert(&t, 1, PAGES_PER_PROC - 1, 0x222);
    REQUIRE(tlb_impl_lookup(&t, 1, 0, &pfn) && pfn == 0x111,
            "vpn 0 (first page) maps to frame 0x%X", pfn);
    REQUIRE(tlb_impl_lookup(&t, 1, PAGES_PER_PROC - 1, &pfn) && pfn == 0x222,
            "vpn %u (last page) maps to frame 0x%X", PAGES_PER_PROC - 1, pfn);
    REQUIRE(tlb_impl_probe(&t, 1, 0) != tlb_impl_probe(&t, 1, 255),
            "they occupy different entries -- vpn 0 is not confused with 255");
    scenario_end();

    /* ------------------------------------------------------------------ */
    scenario("Boundary: lowest and highest PID the 14-bit field can hold");
    tlb_impl_init(&t);
    {
        uint32_t maxpid = (uint32_t)MASK(PID_BITS);          /* 16383 */
        tlb_impl_insert(&t, 1,      0x40, 0x300);
        tlb_impl_insert(&t, maxpid, 0x40, 0x400);
        REQUIRE(tlb_impl_lookup(&t, 1, 0x40, &pfn) && pfn == 0x300,
                "pid 1 -> frame 0x%X", pfn);
        REQUIRE(tlb_impl_lookup(&t, maxpid, 0x40, &pfn) && pfn == 0x400,
                "pid %u (2^%u - 1) -> frame 0x%X", maxpid, PID_BITS, pfn);
        REQUIRE(tlb_impl_probe(&t, 1, 0x40) >= 0 &&
                tlb_impl_probe(&t, maxpid, 0x40) >= 0,
                "both survive: the PID field holds its full range");
    }
    scenario_end();

    /* ------------------------------------------------------------------ */
    scenario("Boundary: frame 0 and the last frame in 32 MB");
    tlb_impl_init(&t);
    {
        uint32_t maxfrm = NUM_FRAMES - 1;                    /* 32767 */
        tlb_impl_insert(&t, 2, 0x01, 0);
        tlb_impl_insert(&t, 2, 0x02, maxfrm);
        REQUIRE(tlb_impl_lookup(&t, 2, 0x01, &pfn) && pfn == 0,
                "frame 0 survives the round trip (a zero pfn is legal)");
        REQUIRE(tlb_impl_lookup(&t, 2, 0x02, &pfn) && pfn == maxfrm,
                "frame %u survives -- the %u-bit field is not one bit short",
                pfn, FRAME_BITS);
    }
    scenario_end();

    /* ------------------------------------------------------------------ */
    scenario("Field truncation is CONSISTENT between insert and probe");
    tlb_impl_init(&t);
    {
        /* The bit fields are narrower than the uint32_t parameters. What
         * matters is not that oversized values are rejected -- they are
         * masked -- but that insert and probe mask IDENTICALLY, so a caller
         * can never store under one key and fail to find it under the same
         * key. An asymmetric mask would be a silent lookup failure. */
        tlb_impl_insert(&t, 1, 300, 0x555);          /* vpn 300 -> 44 */
        REQUIRE(tlb_impl_probe(&t, 1, 300) >= 0,
                "vpn 300 is found again under vpn 300");
        REQUIRE(tlb_impl_probe(&t, 1, 300) == tlb_impl_probe(&t, 1, 300 & 0xFF),
                "and under its masked form %u -- same entry, no split", 300 & 0xFF);

        tlb_impl_init(&t);
        tlb_impl_insert(&t, 1, 0x10, NUM_FRAMES + 5);   /* pfn wraps to 5 */
        tlb_impl_lookup(&t, 1, 0x10, &pfn);
        REQUIRE(pfn == 5, "pfn %u + %u masks to %u -- and stays masked on read",
                NUM_FRAMES, 5, pfn);
        REQUIRE(pfn < NUM_FRAMES,
                "a stored frame is ALWAYS a legal frame number");
    }
    scenario_end();

    /* ------------------------------------------------------------------ */
    scenario("PID recycling: a new process must not inherit a dead one's TLB");
    tlb_impl_init(&t);
    {
        int inherited;
        /* pid 5 runs and caches four translations */
        for (uint32_t v = 0; v < 4; v++) tlb_impl_insert(&t, 5, v, 0x600 + v);
        REQUIRE(tlb_impl_probe(&t, 5, 2) >= 0, "pid 5 has its mappings cached");

        /* pid 5 terminates -- the specification's invalidation event */
        tlb_impl_invalidate_pid(&t, 5);

        /* the PID is recycled: a brand-new, unrelated process gets pid 5 */
        inherited = 0;
        for (uint32_t v = 0; v < 4; v++)
            if (tlb_impl_lookup(&t, 5, v, &pfn)) inherited = 1;

        REQUIRE(!inherited,
                "the new pid 5 inherits NOTHING -- all 4 lookups miss");
        printf("          PID_BITS = %u, so only %lu identifiers exist and\n"
               "          recycling is inevitable in a long run. Without the\n"
               "          invalidation on exit, the new process would read the\n"
               "          dead process's frames -- a total protection failure.\n",
               PID_BITS, (unsigned long)MASK(PID_BITS) + 1);

        /* and it can build its own mappings on the same VPNs */
        tlb_impl_insert(&t, 5, 2, 0x999);
        REQUIRE(tlb_impl_lookup(&t, 5, 2, &pfn) && pfn == 0x999,
                "and establishes its own mapping for vpn 2 -> 0x%X", pfn);
    }
    scenario_end();

    /* ------------------------------------------------------------------ */
    scenario("Negative control: the same sequence WITHOUT the invalidation");
    tlb_impl_init(&t);
    {
        int inherited = 0;
        for (uint32_t v = 0; v < 4; v++) tlb_impl_insert(&t, 5, v, 0x600 + v);
        /* pid 5 "exits" but nobody calls tlb_impl_invalidate_pid */
        for (uint32_t v = 0; v < 4; v++)
            if (tlb_impl_lookup(&t, 5, v, &pfn)) inherited = 1;
        REQUIRE(inherited,
                "the stale entries ARE still reachable -- confirming the "
                "previous scenario tested something real, not a tautology");
    }
    scenario_end();

    /* ------------------------------------------------------------------ */
    scenario("Churn on a single mapping never grows or evicts");
    tlb_impl_init(&t);
    {
        for (int i = 0; i < 1000; i++) {
            tlb_impl_insert(&t, 3, 0x77, 0x800 + (uint32_t)(i & 7));
            tlb_impl_lookup(&t, 3, 0x77, &pfn);
        }
        REQUIRE(impl_valid(&t) == 1, "exactly 1 valid entry after 1000 inserts");
        REQUIRE(t.evictions == 0, "0 evictions (%llu)",
                (unsigned long long)t.evictions);
        REQUIRE(pfn == 0x800 + 7, "and it holds the newest frame 0x%X", pfn);
    }
    scenario_end();

    /* ------------------------------------------------------------------ */
    scenario("Scattered invalidation then refill reuses holes, never evicts");
    tlb_impl_init(&t);
    {
        for (uint32_t v = 0; v < TLB_ENTRIES; v++)
            tlb_impl_insert(&t, 4, v, 0x900 + v);
        for (uint32_t v = 1; v < TLB_ENTRIES; v += 3)      /* punch holes */
            tlb_impl_invalidate_entry(&t, 4, v);
        {
            unsigned holes = TLB_ENTRIES - impl_valid(&t);
            tlb_impl_reset_stats(&t);
            for (unsigned k = 0; k < holes; k++)
                tlb_impl_insert(&t, 6, 0xF0 + k, 0xA00 + k);
            REQUIRE(t.evictions == 0,
                    "%u holes refilled with 0 evictions (%llu)", holes,
                    (unsigned long long)t.evictions);
            REQUIRE(impl_valid(&t) == TLB_ENTRIES,
                    "TLB is full again (%u entries)", impl_valid(&t));
        }
    }
    scenario_end();

    /* ------------------------------------------------------------------ */
    scenario("Statistics stay self-consistent under a mixed workload");
    tlb_impl_init(&t);
    {
        uint64_t lookups = 0;
        for (int i = 0; i < 5000; i++) {
            uint32_t pid = 1 + rnd_below(4), vpn = rnd_below(40);
            if (rnd_below(3) == 0) tlb_impl_insert(&t, pid, vpn, 0xB00 + vpn);
            else { tlb_impl_lookup(&t, pid, vpn, &pfn); lookups++; }
        }
        REQUIRE(t.hits + t.misses == lookups,
                "hits (%llu) + misses (%llu) == lookups (%llu)",
                (unsigned long long)t.hits, (unsigned long long)t.misses,
                (unsigned long long)lookups);
        REQUIRE(t.evictions <= t.misses + 5000,
                "evictions (%llu) never exceed the inserts that caused them",
                (unsigned long long)t.evictions);
    }
    scenario_end();

    /* ==================================================================
     *  Differential testing
     * ================================================================== */
    scenario("Differential: pressure -- working set 4x the TLB");
    {
        Mix m = { 4, 32, 60, 30, 4, 3, 3, "4 pids x 32 pages" };
        REQUIRE(run_differential(&m, 200000, 0x2A234u),
                "200,000 operations, no divergence");
    }
    scenario_end();

    scenario("Differential: fits -- working set smaller than the TLB");
    {
        Mix m = { 2, 8, 70, 25, 2, 2, 1, "2 pids x 8 pages" };
        REQUIRE(run_differential(&m, 100000, 0x13579u),
                "100,000 operations, no divergence");
    }
    scenario_end();

    scenario("Differential: invalidation-heavy -- constant paging churn");
    {
        Mix m = { 8, 64, 30, 30, 15, 15, 10, "8 pids, 40% invalidations" };
        REQUIRE(run_differential(&m, 200000, 0xBEEF1u),
                "200,000 operations, no divergence");
    }
    scenario_end();

    scenario("Differential: single hot page -- degenerate locality");
    {
        Mix m = { 1, 1, 50, 40, 4, 3, 3, "1 pid x 1 page" };
        REQUIRE(run_differential(&m, 50000, 0x0F0F0u),
                "50,000 operations, no divergence");
    }
    scenario_end();

    scenario("Differential: exactly TLB-sized working set (the boundary)");
    {
        Mix m = { 1, TLB_ENTRIES, 65, 30, 2, 1, 2, "1 pid x 32 pages" };
        REQUIRE(run_differential(&m, 200000, 0xC0FFEu),
                "200,000 operations, no divergence");
    }
    scenario_end();

    scenario("Differential: one page more than the TLB holds");
    {
        Mix m = { 1, TLB_ENTRIES + 1, 65, 30, 2, 1, 2, "1 pid x 33 pages" };
        REQUIRE(run_differential(&m, 200000, 0xD00Du),
                "200,000 operations, no divergence");
    }
    scenario_end();

    /* --------------------------------------------------------- report */
    printf("\n=========================================================================\n");
    printf("  %d scenarios run, %d passed, %d failed\n",
           scenarios, scenarios - scen_failed, scen_failed);
    printf("  differential coverage: 950,000 operations against a reference model\n");
    printf("=========================================================================\n");
    report_end("test_tlb_full", "scenarios", scenarios, scen_failed);
    return scen_failed ? 1 : 0;
}
