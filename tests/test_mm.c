/* ============================================================================
 *  tests/test_mm.c -- main memory + the complete VA -> PA translation flow
 *
 *  Build:  make test-mm      Run:  ./test_mm
 *  Links src/MainMemory.c, src/translate.c and src/TLB.c only.
 *
 *  Sections
 *    1  geometry: the page table really is one frame
 *    2  mm_init / mm_alloc_frame / free-frame accounting
 *    3  pre-paging: page table + first 2 pages
 *    4  the happy path: TLB miss -> walk -> hit
 *    5  demand paging, and the page that arrives is the one requested
 *    6  edge case: VA outside the 18-bit space
 *    7  edge case: protection violation, checked before any disk work
 *    8  edge case: unknown / inactive process
 *    9  writes set the PTE dirty bit
 *   10  LFU with aging: shift register behaviour
 *   11  victim policy: lowest aging, skip page tables, honour lower_limit
 *   12  upper_limit forces a process to evict its own page
 *   13  frame reuse invalidates the TLB   <-- the correctness landmine
 *   14  cross-process eviction clears the VICTIM OWNER's PTE
 *   15  dirty victims are written back to disk
 *   16  process exit releases frames and TLB entries
 *   17  mm_read_block / mm_write
 * ==========================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "translate.h"
#include "report.h"

/* ---------------------------------------------------------------- harness */

static int checks = 0, failures = 0;
static const char *section = "";

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        int ok_ = (cond) ? 1 : 0;                                             \
        checks++;                                                             \
        printf(ok_ ? "    pass  " : "    FAIL  ");                            \
        printf(__VA_ARGS__);                                                  \
        putchar('\n');                                                        \
        if (!ok_) { failures++;                                               \
            printf("          ^ in \"%s\" at %s:%d\n", section,               \
                   __FILE__, __LINE__); }                                     \
    } while (0)

static void head(const char *n, const char *title)
{
    section = title;
    printf("\n%s  %s\n", n, title);
    for (unsigned i = 0; i < 70; i++) putchar('-');
    putchar('\n');
}

/* MainMemory embeds FrameDesc[32768] -- about 512 KB. Never a local. */
static MainMemory mm;
static Process    procs[8];
static MMU        mmu;

/* Records what the cache layer would have been told to invalidate. */
static uint32_t inval_frames[64];
static unsigned inval_count;

static void fake_cache_invalidate(void *ctx, uint32_t frame)
{
    (void)ctx;
    if (inval_count < 64) inval_frames[inval_count] = frame;
    inval_count++;
}

static int saw_invalidation_of(uint32_t frame)
{
    unsigned i;
    for (i = 0; i < inval_count && i < 64; i++)
        if (inval_frames[i] == frame) return 1;
    return 0;
}

static void reset_world(uint16_t nproc)
{
    uint16_t i;
    mm_destroy(&mm);
    memset(procs, 0, sizeof(procs));
    mm_init(&mm);
    for (i = 0; i < nproc; i++) procs[i].pid = (uint16_t)(i + 1);
    mmu_init(&mmu, &mm, procs, nproc);
    mmu_set_cache_invalidator(&mmu, fake_cache_invalidate, NULL);
    mmu_set_tick_interval(&mmu, 0);          /* aging only when asked */
    inval_count = 0;
}

/* Fill every remaining free frame so the next fault must evict. Filler
 * frames have no live owner, so they are freely evictable -- tests that need
 * a specific victim raise the filler aging to 0xFF afterwards. */
static uint32_t fill_memory(uint8_t filler_aging)
{
    uint32_t n = 0;
    int f;
    while ((f = mm_alloc_frame(&mm, 0x3FFF, 0, 0)) >= 0) {
        mm.frames[f].aging = filler_aging;
        mm.frames[f].freq  = 0xFFFF;
        n++;
    }
    return n;
}

/* ============================================================== the tests */

int main(int argc, char **argv)
{
    report_begin(argc, argv, "results/test_mm.txt");

    uint32_t pa, pa2;
    MMUStatus st;
    int i;

    printf("=================================================================\n");
    printf("  Main memory + address translation -- 32 MB, 1 KB pages\n");
    printf("=================================================================\n");

    /* ---------------------------------------------------------------- 1 */
    head("1.", "Geometry -- a page table is exactly one frame");
    printf("    sizeof(PTE)       = %2zu B   (29 bits packed into a uint32_t)\n",
           sizeof(PTE));
    printf("    sizeof(PageTable) = %4zu B  = %u PTEs x %zu B\n",
           sizeof(PageTable), PAGES_PER_PROC, sizeof(PTE));
    printf("    sizeof(MainMemory)= %4zu KB (FrameDesc x %u -- heap or static!)\n",
           sizeof(MainMemory) / 1024, NUM_FRAMES);
    CHECK(sizeof(PageTable) == PAGE_SIZE,
          "page table (%zu B) fits one %u B frame -- no multi-level walk needed",
          sizeof(PageTable), PAGE_SIZE);

    /* ---------------------------------------------------------------- 2 */
    head("2.", "mm_init and frame accounting");
    reset_world(2);
    CHECK(mm.storage != NULL, "32 MB of storage allocated");
    CHECK(mm.free_frames == NUM_FRAMES, "all %u frames free", NUM_FRAMES);
    {
        int f = mm_alloc_frame(&mm, 1, 7, 0);
        CHECK(f >= 0, "allocated frame %d", f);
        CHECK(mm.free_frames == NUM_FRAMES - 1, "free_frames dropped to %u",
              mm.free_frames);
        CHECK(mm.frames[f].allocated && mm.frames[f].pid == 1 &&
              mm.frames[f].vpn == 7, "frame descriptor records pid 1, vpn 7");
    }

    /* ---------------------------------------------------------------- 3 */
    head("3.", "Pre-paging -- page table plus the first 2 pages");
    reset_world(2);
    CHECK(mm_prepage(&mm, &procs[0]) == 0, "mm_prepage(pid 1) succeeded");
    CHECK(procs[0].pt != NULL, "page table is resident, in frame %u",
          procs[0].pt_frame);
    CHECK(mm.frames[procs[0].pt_frame].is_page_table,
          "its frame is marked is_page_table (never evictable)");
    CHECK((uint8_t *)procs[0].pt ==
              mm.storage + (size_t)procs[0].pt_frame * PAGE_SIZE,
          "the page table lives IN main memory, not beside it");
    CHECK(procs[0].pt->entries[0].present && procs[0].pt->entries[1].present,
          "pages 0 and 1 are present");
    CHECK(!procs[0].pt->entries[2].present, "page 2 is NOT -- demand paged");
    CHECK(procs[0].frames_held == 2, "frames_held = %u (data pages only)",
          procs[0].frames_held);
    CHECK(mm.free_frames == NUM_FRAMES - 3,
          "3 frames consumed = 2 pages + 1 page table = MIN_FRAMES_PER_PROC");
    CHECK(mm.page_faults == 0,
          "pre-paging did not inflate the demand-fault count (%llu)",
          (unsigned long long)mm.page_faults);

    /* ---------------------------------------------------------------- 4 */
    head("4.", "Happy path -- TLB miss, walk, then hit");
    {
        uint32_t va = (1u << PAGE_OFFSET_BITS) | 0x234;   /* page 1, off 564 */

        st = mmu_translate(&mmu, 1, va, ACC_READ, &pa);
        CHECK(st == MMU_OK_TLB_MISS, "1st access: %s", mmu_status_name(st));
        CHECK(mmu.pt_walks == 1, "exactly 1 page-table walk = 1 memory access");
        CHECK(mmu.page_faults == 0, "no page fault -- the page was pre-paged");

        st = mmu_translate(&mmu, 1, va, ACC_READ, &pa2);
        CHECK(st == MMU_OK, "2nd access: %s", mmu_status_name(st));
        CHECK(mmu.pt_walks == 1, "still 1 walk -- the TLB absorbed it");
        CHECK(pa == pa2, "same PA both times: 0x%06X", pa);
        CHECK((unsigned)PA_OFFSET(pa) == 0x234,
              "page offset 0x234 passed through untranslated");
        CHECK((unsigned)L1_TAG(pa) == (unsigned)PA_FRAME(pa),
              "L1 tag == frame %u -- L1 can index before translation finishes",
              (unsigned)PA_FRAME(pa));
    }

    /* ---------------------------------------------------------------- 5 */
    head("5.", "Demand paging -- and the right page arrives");
    {
        uint32_t va = (37u << PAGE_OFFSET_BITS) | 0x10;
        uint64_t reads = mm.disk_reads;
        uint32_t held  = procs[0].frames_held;
        const uint8_t *bytes;

        st = mmu_translate(&mmu, 1, va, ACC_READ, &pa);
        CHECK(st == MMU_OK_PAGE_FAULT, "page 37: %s", mmu_status_name(st));
        CHECK(mm.disk_reads == reads + 1, "exactly 1 disk read");
        CHECK(procs[0].frames_held == held + 1, "resident set grew to %u",
              procs[0].frames_held);
        CHECK(procs[0].pt->entries[37].present, "PTE 37 is now present");

        bytes = mm.storage + (size_t)PA_FRAME(pa) * PAGE_SIZE;
        CHECK(bytes[0] == 1 && bytes[2] == 37,
              "the frame holds page 37 of pid 1 (marker %u/%u)",
              bytes[0], bytes[2]);

        st = mmu_translate(&mmu, 1, va, ACC_READ, &pa2);
        CHECK(st == MMU_OK && pa == pa2, "second access hits the TLB");
    }

    /* ---------------------------------------------------------------- 6 */
    head("6.", "Edge case -- virtual address outside the 18-bit space");
    {
        uint32_t bad = 1u << VA_BITS;                    /* 0x40000 */
        uint64_t walks = mmu.pt_walks;
        pa2 = 0xDEADBEEF;
        st = mmu_translate(&mmu, 1, bad, ACC_READ, &pa2);
        CHECK(st == MMU_FAULT_ADDRESS, "VA 0x%05X: %s", bad, mmu_status_name(st));
        CHECK(pa2 == 0xDEADBEEF, "*pa_out untouched on a fault");
        CHECK(mmu.pt_walks == walks, "rejected before any page-table access");
        CHECK(mmu.addr_faults == 1, "counted as an address fault");
        printf("    (without this check VA_VPN() would truncate 0x40000 to page 0)\n");
    }

    /* ---------------------------------------------------------------- 7 */
    head("7.", "Edge case -- protection violation, before any disk work");
    {
        uint64_t reads = mm.disk_reads, faults = mm.page_faults;

        procs[0].pt->entries[50].prot = PROT_READ;        /* read-only, absent */
        st = mmu_translate(&mmu, 1, 50u << PAGE_OFFSET_BITS, ACC_WRITE, &pa2);
        CHECK(st == MMU_FAULT_PROTECTION, "write to a read-only page: %s",
              mmu_status_name(st));
        CHECK(mm.disk_reads == reads, "no disk read was wasted on it");
        CHECK(mm.page_faults == faults, "no frame was allocated for it");
        CHECK(!procs[0].pt->entries[50].present, "the page is still absent");

        st = mmu_translate(&mmu, 1, 50u << PAGE_OFFSET_BITS, ACC_READ, &pa2);
        CHECK(st == MMU_OK_PAGE_FAULT, "reading the same page is allowed: %s",
              mmu_status_name(st));
        printf("    note: after this fill, protection is no longer re-checked on\n");
        printf("    a TLB hit -- TLBImplEntry has no prot bits (see translate.h)\n");
    }

    /* ---------------------------------------------------------------- 8 */
    head("8.", "Edge case -- unknown or inactive process");
    st = mmu_translate(&mmu, 99, 0x400, ACC_READ, &pa2);
    CHECK(st == MMU_FAULT_NO_PROCESS, "pid 99 was never created: %s",
          mmu_status_name(st));
    CHECK(mmu_find_process(&mmu, 99) == NULL, "no Process record for pid 99");

    /* ---------------------------------------------------------------- 9 */
    head("9.", "Writes set the PTE dirty bit");
    {
        uint32_t va = (37u << PAGE_OFFSET_BITS) | 0x20;
        CHECK(!procs[0].pt->entries[37].dirty, "page 37 starts clean");
        st = mmu_translate(&mmu, 1, va, ACC_WRITE, &pa);
        CHECK(mmu_ok(st), "write translated: %s", mmu_status_name(st));
        CHECK(procs[0].pt->entries[37].dirty,
              "PTE 37 is dirty -- meaning dirty vs DISK; both caches are "
              "write-through so no cache holds newer bytes");
    }

    /* --------------------------------------------------------------- 10 */
    head("10.", "LFU with aging -- the shift register");
    reset_world(1);
    mm_prepage(&mm, &procs[0]);
    {
        PTE *hot  = &procs[0].pt->entries[0];
        PTE *cold = &procs[0].pt->entries[1];

        hot->aging = cold->aging = 0;
        for (i = 0; i < 8; i++) {
            hot->referenced = 1;                 /* touched every interval */
            cold->referenced = 0;                /* never touched again    */
            mm_age_tick(&mm, procs, 1);
        }
        printf("    after 8 ticks: hot aging = 0x%02X, cold aging = 0x%02X\n",
               hot->aging, cold->aging);
        CHECK(hot->aging == 0xFF, "a page used every interval saturates at 0xFF");
        CHECK(cold->aging == 0x00, "an unused page decays to 0x00 -- LFU alone "
              "would have kept it forever on its old count");
        CHECK(hot->referenced == 0, "the reference bit is cleared by the tick");
        CHECK(mm.frames[hot->frame].aging == hot->aging,
              "FrameDesc.aging mirrors PTE.aging, so victim search stays linear");

        cold->referenced = 1;
        mm_age_tick(&mm, procs, 1);
        CHECK(cold->aging == 0x80, "one touch puts a 1 in the MSB: 0x%02X",
              cold->aging);
        CHECK(hot->aging == 0x7F, "and an idle interval shifts the hot page to "
              "0x%02X -- recency outranks raw frequency", hot->aging);
    }

    /* --------------------------------------------------------------- 11 */
    head("11.", "Victim policy -- lowest aging, skip page tables, honour floors");
    reset_world(2);
    mm_prepage(&mm, &procs[0]);
    mm_prepage(&mm, &procs[1]);
    {
        int victim;
        uint32_t f_low;

        for (i = 2; i < 8; i++)                        /* grow pid 1 to 8 */
            mmu_translate(&mmu, 1, (uint32_t)i << PAGE_OFFSET_BITS,
                          ACC_READ, &pa);

        /* Make page 5 the coldest thing in memory. */
        for (i = 0; i < 8; i++)
            mm.frames[procs[0].pt->entries[i].frame].aging = 0xF0;
        f_low = procs[0].pt->entries[5].frame;
        mm.frames[f_low].aging = 0x01;

        victim = mm_select_victim(&mm, procs, 2);
        CHECK(victim == (int)f_low, "picked frame %d -- the lowest aging (0x01)",
              victim);
        CHECK(!mm.frames[victim].is_page_table, "and it is not a page table");

        /* Pin pid 1 at its floor: every one of its frames becomes ineligible. */
        procs[0].lower_limit = procs[0].frames_held;
        victim = mm_select_victim(&mm, procs, 2);
        CHECK(victim != (int)f_low,
              "at lower_limit=%u, pid 1's pages are skipped (victim now %d)",
              procs[0].lower_limit, victim);
        CHECK(victim < 0 || mm.frames[victim].pid != 1,
              "the victim no longer belongs to the pinned process");
        procs[0].lower_limit = MIN_FRAMES_PER_PROC;

        /* Page-table frames must never be selectable, at any aging value. */
        mm.frames[procs[0].pt_frame].aging = 0x00;
        mm.frames[procs[1].pt_frame].aging = 0x00;
        victim = mm_select_victim(&mm, procs, 2);
        CHECK(victim != (int)procs[0].pt_frame &&
              victim != (int)procs[1].pt_frame,
              "page-table frames are never chosen even at aging 0x00");
    }

    /* --------------------------------------------------------------- 12 */
    head("12.", "upper_limit forces a process to evict its own page");
    reset_world(1);
    procs[0].upper_limit = 4;
    mm_prepage(&mm, &procs[0]);
    procs[0].lower_limit = 2;
    {
        uint32_t peak = 0;
        for (i = 0; i < 20; i++) {
            mmu_translate(&mmu, 1, (uint32_t)i << PAGE_OFFSET_BITS,
                          ACC_READ, &pa);
            if (procs[0].frames_held > peak) peak = procs[0].frames_held;
        }
        CHECK(peak <= 4, "resident set never exceeded upper_limit (peak %u)", peak);
        CHECK(procs[0].frames_held == 4, "and sits at the cap: %u frames",
              procs[0].frames_held);
        CHECK(mm.free_frames > NUM_FRAMES - 20,
              "memory was never short -- the cap alone forced the evictions");
        printf("    20 pages touched, 4 frames allowed: local replacement\n");
        printf("    inside an otherwise global policy\n");
    }

    /* --------------------------------------------------------------- 13 */
    head("13.", "Frame reuse invalidates the TLB  <-- the correctness landmine");
    reset_world(2);
    mm_prepage(&mm, &procs[0]);
    {
        uint32_t doomed_vpn = 4, doomed_frame;
        uint32_t got;

        for (i = 2; i < 8; i++)
            mmu_translate(&mmu, 1, (uint32_t)i << PAGE_OFFSET_BITS, ACC_READ, &pa);

        doomed_frame = procs[0].pt->entries[doomed_vpn].frame;
        CHECK(tlb_impl_probe(&mmu.tlb, 1, doomed_vpn) >= 0,
              "page %u is cached in the TLB, mapping frame %u",
              doomed_vpn, doomed_frame);

        /* Exhaust memory, then make the doomed page the coldest thing. */
        fill_memory(0xFF);
        CHECK(mm.free_frames == 0, "main memory is full");
        for (i = 0; i < 8; i++)
            mm.frames[procs[0].pt->entries[i].frame].aging = 0xFF;
        mm.frames[doomed_frame].aging = 0x00;
        mm.frames[doomed_frame].freq  = 0;
        inval_count = 0;

        st = mmu_translate(&mmu, 1, 60u << PAGE_OFFSET_BITS, ACC_READ, &pa);
        CHECK(st == MMU_OK_PAGE_FAULT, "faulting page 60: %s",
              mmu_status_name(st));
        CHECK(!procs[0].pt->entries[doomed_vpn].present,
              "the victim's PTE was cleared");
        CHECK(tlb_impl_probe(&mmu.tlb, 1, doomed_vpn) < 0,
              "its TLB entry is GONE -- otherwise a hit would hand back "
              "frame %u, which now holds page 60", doomed_frame);
        CHECK(mmu.tlb_invalidations >= 1, "%llu TLB entries invalidated",
              (unsigned long long)mmu.tlb_invalidations);
        CHECK(saw_invalidation_of(doomed_frame),
              "the cache layer was told to invalidate frame %u too",
              doomed_frame);
        CHECK((uint32_t)PA_FRAME(pa) == doomed_frame,
              "and page 60 was given exactly that frame -- the reuse is real");

        /* Prove the TLB now resolves the frame to the NEW page. */
        mmu_translate(&mmu, 1, 60u << PAGE_OFFSET_BITS, ACC_READ, &pa2);
        tlb_impl_lookup(&mmu.tlb, 1, 60, &got);
        CHECK(got == doomed_frame, "TLB now maps page 60 -> frame %u", got);
    }

    /* --------------------------------------------------------------- 14 */
    head("14.", "Cross-process eviction clears the VICTIM OWNER's PTE");
    reset_world(2);
    mm_prepage(&mm, &procs[0]);          /* pid 1 -- will fault */
    mm_prepage(&mm, &procs[1]);          /* pid 2 -- will be robbed */
    {
        uint32_t victim_vpn = 5, victim_frame;
        uint16_t held_before;

        for (i = 2; i < 9; i++)
            mmu_translate(&mmu, 2, (uint32_t)i << PAGE_OFFSET_BITS, ACC_READ, &pa);

        victim_frame = procs[1].pt->entries[victim_vpn].frame;
        held_before  = procs[1].frames_held;

        fill_memory(0xFF);
        for (i = 0; i < 9; i++) {
            if (procs[0].pt->entries[i].present)
                mm.frames[procs[0].pt->entries[i].frame].aging = 0xFF;
            if (procs[1].pt->entries[i].present)
                mm.frames[procs[1].pt->entries[i].frame].aging = 0xFF;
        }
        mm.frames[victim_frame].aging = 0x00;
        mm.frames[victim_frame].freq  = 0;

        st = mmu_translate(&mmu, 1, 70u << PAGE_OFFSET_BITS, ACC_READ, &pa);
        CHECK(st == MMU_OK_PAGE_FAULT, "pid 1 faults: %s", mmu_status_name(st));
        CHECK((uint32_t)PA_FRAME(pa) == victim_frame,
              "pid 1 took frame %u from pid 2 (global replacement)", victim_frame);
        CHECK(!procs[1].pt->entries[victim_vpn].present,
              "pid 2's PTE for page %u was cleared -- not pid 1's", victim_vpn);
        CHECK(procs[1].frames_held == held_before - 1,
              "pid 2's resident set dropped %u -> %u",
              held_before, procs[1].frames_held);
        CHECK(tlb_impl_probe(&mmu.tlb, 2, victim_vpn) < 0,
              "and pid 2's TLB entry for it is gone");
        printf("    without mm_set_process_table() the fault would have cleared\n");
        printf("    the WRONG page table and left pid 2 pointing at pid 1's page\n");
    }

    /* --------------------------------------------------------------- 15 */
    head("15.", "Dirty victims are written back to disk");
    reset_world(1);
    mm_prepage(&mm, &procs[0]);
    {
        uint32_t dirty_vpn = 3, dirty_frame;
        uint64_t wb_before;

        for (i = 2; i < 8; i++)
            mmu_translate(&mmu, 1, (uint32_t)i << PAGE_OFFSET_BITS, ACC_READ, &pa);
        mmu_translate(&mmu, 1, dirty_vpn << PAGE_OFFSET_BITS, ACC_WRITE, &pa);

        dirty_frame = procs[0].pt->entries[dirty_vpn].frame;
        CHECK(procs[0].pt->entries[dirty_vpn].dirty, "page %u is dirty", dirty_vpn);

        fill_memory(0xFF);
        for (i = 0; i < 8; i++)
            if (procs[0].pt->entries[i].present)
                mm.frames[procs[0].pt->entries[i].frame].aging = 0xFF;
        mm.frames[dirty_frame].aging = 0x00;
        mm.frames[dirty_frame].freq  = 0;
        wb_before = mm.disk_writebacks;

        mmu_translate(&mmu, 1, 80u << PAGE_OFFSET_BITS, ACC_READ, &pa);
        CHECK(mm.disk_writebacks == wb_before + 1,
              "1 write-back performed (%llu total)",
              (unsigned long long)mm.disk_writebacks);
        printf("    both caches are write-through, so main memory already held\n");
        printf("    the newest bytes -- no cache flush was needed first\n");
    }

    /* --------------------------------------------------------------- 16 */
    head("16.", "Process exit releases frames and TLB entries");
    reset_world(2);
    mm_prepage(&mm, &procs[0]);
    mm_prepage(&mm, &procs[1]);
    {
        uint32_t free_before, pt_frame = procs[1].pt_frame;
        unsigned tlb_pid2 = 0, k;

        for (i = 2; i < 10; i++)
            mmu_translate(&mmu, 2, (uint32_t)i << PAGE_OFFSET_BITS, ACC_READ, &pa);
        for (i = 2; i < 6; i++)
            mmu_translate(&mmu, 1, (uint32_t)i << PAGE_OFFSET_BITS, ACC_READ, &pa);

        free_before = mm.free_frames;
        for (k = 0; k < TLB_ENTRIES; k++)
            if (mmu.tlb.entries[k].valid && mmu.tlb.entries[k].pid == 2) tlb_pid2++;
        CHECK(tlb_pid2 > 0, "pid 2 owns %u TLB entries before exit", tlb_pid2);

        mmu_process_exit(&mmu, 2);

        CHECK(mm.free_frames == free_before + 10 + 1,
              "10 data frames + 1 page-table frame returned (free %u -> %u)",
              free_before, mm.free_frames);
        CHECK(!mm.frames[pt_frame].allocated,
              "the page-table frame is released -- only exit can free it, "
              "since replacement never touches it");
        for (k = 0; k < TLB_ENTRIES; k++)
            if (mmu.tlb.entries[k].valid && mmu.tlb.entries[k].pid == 2)
                { CHECK(0, "a pid-2 TLB entry survived exit"); break; }
        CHECK(tlb_impl_probe(&mmu.tlb, 2, 3) < 0,
              "every pid-2 TLB entry is gone -- PIDs are %u bits and get "
              "recycled, so this is mandatory", PID_BITS);
        CHECK(procs[0].frames_held > 0 && tlb_impl_probe(&mmu.tlb, 1, 3) >= 0,
              "pid 1 is completely unaffected");
        CHECK(!procs[1].active && procs[1].pt == NULL, "pid 2 is torn down");
    }

    /* --------------------------------------------------------------- 17 */
    head("17.", "Block reads and write-through arrivals");
    reset_world(1);
    mm_prepage(&mm, &procs[0]);
    {
        uint8_t block[BLOCK_SIZE], readback[BLOCK_SIZE];
        uint8_t payload[4] = { 0xDE, 0xAD, 0xBE, 0xEF };

        mmu_translate(&mmu, 1, (1u << PAGE_OFFSET_BITS) | 0x30, ACC_READ, &pa);

        mm_read_block(&mm, pa, block);
        CHECK(mm.block_fetches == 1, "1 block fetch counted");

        mm_write(&mm, pa, payload, 4);
        CHECK(mm.writes == 1, "1 write-through arrival counted");

        mm_read_block(&mm, pa, readback);
        CHECK(memcmp(readback + L1_BLK_OFFSET(pa), payload, 4) == 0,
              "the 4 written bytes are visible at block offset %u",
              (unsigned)L1_BLK_OFFSET(pa));
        CHECK(memcmp(block + L1_BLK_OFFSET(pa) + 4,
                     readback + L1_BLK_OFFSET(pa) + 4,
                     BLOCK_SIZE - L1_BLK_OFFSET(pa) - 4) == 0,
              "and nothing outside them changed");
    }

    /* ------------------------------------------------------------ report */
    mmu_print_stats(&mmu, stdout);
    mm_destroy(&mm);

    printf("\n=================================================================\n");
    printf("  %d checks, %d failed%s\n", checks, failures,
           failures ? "" : "  --  ALL TESTS PASSED");
    printf("=================================================================\n");
    report_end("test_mm", "checks", checks, failures);
    return failures ? 1 : 0;
}
