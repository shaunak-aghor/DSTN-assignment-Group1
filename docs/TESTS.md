# Test suites — what each case does, what it prints, and what it proves

Two standalone harnesses. Neither depends on `l1.c`, `l2.c` or `cache.c`, so both
run while those modules are still unfinished.

```
gcc -Wall -Wextra -g -I./include -o test_tlb tests/test_tlb.c src/TLB.c
./test_tlb                                       # 135 checks

gcc -Wall -Wextra -g -I./include -o test_mm \
    tests/test_mm.c src/MainMemory.c src/translate.c src/TLB.c
./test_mm                                        # 83 checks
```

Both exit `0` on success, `1` on any failure, so they drop straight into a
Makefile or CI. Every line is `pass` or `FAIL`; a failure additionally prints
the section name and the source line, so nothing has to be hunted for.

### Two harness details that matter

**`CHECK` evaluates its condition exactly once.** Several checks call
`tlb_impl_lookup()` inside the condition. The first version of the macro
evaluated `cond` twice — once to decide pass/fail, once to decide whether to
print the location — which silently doubled every hit and miss counter. The
symptom was `misses=10` where 5 were expected. The condition is now assigned to
`ok_` first. If you extend these tests, keep that property.

**The LRU rank invariant is re-checked after every mutation.** The TLB stores
recency as an explicit rank per entry, and the whole design depends on one
property: *the ranks of the valid entries are a permutation of `0 .. n-1`*, with
0 the most recently used. If a gap or a duplicate ever appears, replacement
silently stops being LRU while still looking plausible. `lru_ok()` verifies the
permutation and `check_lru()` is called after every fill, hit, eviction and
invalidation — that is why sections 6 and 12 print so many identical
`LRU ranks form a permutation` lines.

---

# Part A — `tests/test_tlb.c`

## 1. Configuration

Prints the derived geometry and asserts it at **compile time**, so a
configuration mistake fails the build rather than a test run.

```
    VA 18 bits = VPN 8 + offset 10   -> 256 pages/process (256 KB)
    PA 25 bits = frame 15 + offset 10 -> 32768 frames (32 MB)
    entry = 1 + 8 vpn + 15 pfn + 14 pid + 5 lru = 43 bits
    TLB storage = 32 x 43 = 1376 bits = 172 bytes
```

The `STATIC_ASSERT` lines check `VA_BITS == VPN_BITS + PAGE_OFFSET_BITS`,
`PA_BITS == FRAME_BITS + PAGE_OFFSET_BITS`, `PAGES_PER_PROC == 2^VPN_BITS`,
`NUM_FRAMES == 2^FRAME_BITS`, `2^TLB_LRU_BITS >= TLB_ENTRIES` and
`PAGE_SIZE == 2^PAGE_OFFSET_BITS`. They compile to `typedef char x[1]` when true
and `typedef char x[-1]` when false — an illegal negative array size.

**Worth noting for a report:** at an 8-bit VPN the 14-bit PID field is *wider
than the tag it qualifies*. About a third of every entry is spent on process
identity. That is still the right trade — it buys flush-free context switching —
but it is a visible cost here in a way it would not be with a 22-bit VPN.

## 2. Address decomposition

```
    pass  VA 0x2A234 fits in 18 bits
    pass  VA_VPN(0x2A234)    = 0xA8 (168)
    pass  VA_OFFSET(0x2A234) = 0x234 (564)
```

Confirms the `MemHier.h` macros against a hand-computed example.
`0x2A234 = 10 1010 0010 0011 0100` in 18 bits: top 8 bits `0xA8` = 168, low 10
bits `0x234` = 564.

## 3. Cold TLB — every lookup is a compulsory miss

Five lookups into a freshly initialised TLB, then the counters are checked.

```
    pass  0 valid entries after init
    pass  lookup(pid 1, vpn 0) misses          (x5, vpn 0..4)
    pass  misses=5 hits=0
```

Proves `tlb_impl_init` leaves nothing valid, that a miss does not accidentally
create an entry, and — via the final count — that each lookup was counted
exactly once. This is the check that caught the double-evaluation bug.

## 4. Fill and hit — the full VA → PA flow

Uses a stub `page_table_walk()` that counts calls, so the *cost* of a miss is
observable, not just its outcome.

```
    pass  1st access to VA 0x2A234 misses -> page-table walk
    pass  exactly 1 walk performed (1 memory access)
    pass  2nd access to the same page hits -- no walk
    pass  still 1 walk after the second access
    pass  both accesses produce the same PA 0x0DA234
    pass  page offset passes through untranslated (0x234)
    pass  L1 tag == frame number (both 15 bits) -- the VIPT property
    L1 view of PA 0x0DA234: tag   872 | index 35 | offset 4
    L2 view of PA 0x0DA234: tag   218 | index  35 | offset 4
```

The last assertion is the interesting one. `L1_TAG(pa) == PA_FRAME(pa)` holds
because L1's index (6) + block offset (4) = 10 bits = exactly the page offset,
which translation never changes. So the L1 set can be selected from the
*virtual* address before the TLB finishes, and the frame number the TLB returns
is used directly as the tag: a virtually-indexed, physically-tagged cache with
**zero aliasing**. L2's index (8) + offset (4) = 12 bits reaches two bits into
the frame number, so L2 cannot start until translation completes.

## 5. Re-inserting the same (pid, vpn) must not duplicate

Inserts `(pid 3, vpn 0x20) -> 0x111`, then the same pair `-> 0x222`.

```
    pass  still exactly 1 valid entry
    pass  the mapping was refreshed, not shadowed (pfn 0x222)
```

Two entries for one pair would be a latent disaster: whichever the linear scan
found first would win, so the stale one could shadow the fresh one
unpredictably. This happens for real whenever the OS remaps a page after a
fault.

## 6. LRU — the 33rd fill evicts the true LRU entry

Fills all 32 slots with vpn `0x10..0x2F`, **touches `0x10`** (the oldest) to
promote it, then inserts a 33rd mapping.

```
    pass  TLB is full (32 entries)
    pass  no evictions while filling empty slots
    pass  exactly 1 capacity eviction recorded
    pass  vpn 0x10 survived -- it was touched
    pass  vpn 0x11 was evicted -- it was the LRU
    pass  vpn 0x70 is now resident
    pass  still exactly 32 valid entries
```

The touch is what makes this a real LRU test rather than a FIFO test. Under
FIFO, `0x10` — inserted first — would have been the victim. Under LRU it
survives and `0x11` dies. The 32 `LRU ranks form a permutation during fill`
lines above come from checking the invariant after every one of the 32 inserts.

## 7. PID tagging — same VPN, different processes

```
    pass  both mappings coexist for the same VPN 0xA8
    pass  pid 7 -> frame 0x1A3
    pass  pid 9 -> frame 0x2C0
    pass  pid 5 misses -- the PID is part of the match, not just the VPN
```

The third line is the important negative: pid 5 has no entry for VPN `0xA8`
even though two other processes do. If the comparator ignored the PID, pid 5
would hit and receive another process's frame — a total protection failure.

## 8. Context switch — no flush, no cost

```
    pass  switching pid 7 -> 9 -> 7 leaves all 2 entries valid
    pass  pid 7's mapping still hits after the switch
    pass  and it counted as a hit, not a refill
    (an untagged TLB would need 2 invalidations here)
```

There is deliberately **no API call to make**. In a PID-tagged TLB a context
switch is a write to the current-PID register; the TLB itself is untouched.
The test asserts that nothing was lost and that the subsequent access was
counted as a *hit*, not as a refill. That is the entire payoff of the PID field.

## 9–11. The three invalidation paths

A valid TLB entry is an assertion that the page table still says the same thing.
Three different events can falsify it, and each has its own entry point.

**9. `invalidate_entry` — one page was evicted.**
```
    pass  3 of 4 entries remain
    pass  vpn 2 is gone
    pass  its neighbours are untouched
    pass  invalidating an absent page is a no-op
```

**10. `invalidate_pid` — a process terminated.** 11 entries across two
processes; pid 2 exits.
```
    pass  11 entries across 2 processes
    pass  process 2's 5 entries were removed
    pass  no pid-2 entry survives
    pass  process 1 is completely unaffected
```
This is the event the assignment specification calls out by name. It is not
optional housekeeping: PIDs are 14 bits and get recycled, so a new process
reusing the PID would otherwise inherit the dead one's translations.

**11. `invalidate_frame` — a frame was reclaimed.** Two processes are given
mappings to the *same* frame `0x777`, a third maps `0x778`.
```
    pass  every mapping of frame 0x777 is gone, across all processes
    pass  frame 0x778 is untouched
```
Frame-granular rather than page-granular, because a frame can be reachable from
several page tables. This is the step that stops a reused frame returning stale
data — the same landmine as test B-13.

## 12. Freed slots are refilled before anything is evicted

Fill 32, terminate the process, refill 32.
```
    pass  TLB emptied by process exit
    pass  refilled to 32 entries
    pass  0 evictions -- freed slots were reused first
```
`tlb_impl_select_victim` must prefer an invalid entry over the LRU valid one.
If it did not, the refill would report 32 "evictions" of entries that were
already dead, and the eviction statistic would be meaningless.

## 13. Statistics accounting

10 hits and 4 misses driven deliberately.
```
    pass  hits   = 10 (expected 10)
    pass  misses = 4 (expected 4)
    pass  every lookup was counted exactly once
    pass  probes and lookups never evict
```
`tlb_impl_probe` is the side-effect-free inspection call; `tlb_impl_lookup` is
the real one that counts and re-ranks. Mixing them up would corrupt every
measurement in the simulator.

## 14. TLB reach — the cliff

The behavioural experiment. A cyclic sweep over *N* pages, warmed up, then
measured over 10 passes.

```
    working set 32 pages (32 KB): hit ratio 100.00%
    pass  32 pages fit -- 0 misses after warm-up
    working set 33 pages (33 KB): hit ratio   0.00%
    pass  33 pages do not fit -- LRU thrashes to 0% on a cyclic sweep
    reach = 32 pages x 1024 B = 32 KB, and only 12.5% of a
    process's 256-page address space
```

**Read this carefully — it is the single most quotable result in the suite.**
One page past reach the hit ratio does not degrade gracefully, it collapses from
100% to *exactly zero*. A cyclic sweep is LRU's worst case: the page evicted is
always the one about to be needed. Reach is 32 KB, which is simultaneously the
size of L2 and only one eighth of a process's 256 KB address space.

## 15. Processes share the 32 entries

Same experiment, but the pages are spread across four processes.

```
    4 procs x  6 pages = 24 mappings vs 32 slots: hit  97.50% |   24 walks
    pass  24 mappings fit -- no eviction, 24 walks total
    pass  hit ratio 97.50% -- only the compulsory misses
    4 procs x 12 pages = 48 mappings vs 32 slots: hit   0.00% | 1920 walks
    pass  48 mappings do not fit -- 1888 evictions
    pass  every access walks the page table: 1920 memory accesses
```

The per-process footprint is identical in kind in both runs; only the **sum
across live processes** differs, and that is what decides whether the TLB copes.
24 mappings cost 24 walks in total. 48 mappings cost 1,920 — one per access,
each an uncached main-memory read, because page tables are never cached. The
97.50% figure is exactly 936/960: the 24 compulsory misses and nothing else.

---

# Part B — `tests/test_mm.c`

Main memory plus the complete translation flow. Also runs clean under
`-fsanitize=address,undefined` with no leaks.

## 1. Geometry — a page table is exactly one frame

```
    sizeof(PTE)       =  4 B   (29 bits packed into a uint32_t)
    sizeof(PageTable) = 1024 B  = 256 PTEs x 4 B
    sizeof(MainMemory)=  512 KB (FrameDesc x 32768 -- heap or static!)
    pass  page table (1024 B) fits one 1024 B frame
```

Also asserted at compile time in `MainMemory.c`. This is the number that makes
**pure single-level paging the correct choice at this scale** — a whole page
table fits in one frame, allocated by the same allocator as everything else,
never scattered, never itself paged. Widen the VA to 32 bits and the same
formula gives 2²² entries = a 16 MB page table per process, which is the
familiar argument for multi-level paging. Pure paging is not broken; it breaks
at scale, and there is no scale here.

The 512 KB `sizeof(MainMemory)` line is a practical warning: `FrameDesc
frames[32768]` is embedded in the struct, so it must be `static` or heap
allocated. A plain local would blow the stack.

## 2. `mm_init` and frame accounting

```
    pass  32 MB of storage allocated
    pass  all 32768 frames free
    pass  allocated frame 0
    pass  free_frames dropped to 32767
    pass  frame descriptor records pid 1, vpn 7
```
`FrameDesc` is the reverse map (frame → owner, page). Without it, eviction would
have to scan every page table to find out what a frame holds.

## 3. Pre-paging — page table plus the first 2 pages

```
    pass  page table is resident, in frame 1
    pass  its frame is marked is_page_table (never evictable)
    pass  the page table lives IN main memory, not beside it
    pass  pages 0 and 1 are present
    pass  page 2 is NOT -- demand paged
    pass  frames_held = 2 (data pages only)
    pass  3 frames consumed = 2 pages + 1 page table = MIN_FRAMES_PER_PROC
    pass  pre-paging did not inflate the demand-fault count (0)
```

Four things established at once. The page table really is stored inside
`mm->storage` at `pt_frame * PAGE_SIZE`, not in a separate allocation — the test
compares the pointers. Its frame is pinned against replacement. `MIN_FRAMES_PER_PROC
= 3` is derived, not arbitrary: 2 pre-paged pages + 1 page-table frame. And
pre-paged loads are excluded from `page_faults`, because they are planned, not
demanded — they still cost disk reads, which is why in the driver
`disk_reads` exceeds `page_faults` by exactly 2 per process.

## 4. Happy path — TLB miss, walk, then hit

```
    pass  1st access: OK (TLB miss, PTE present)
    pass  exactly 1 page-table walk = 1 memory access
    pass  no page fault -- the page was pre-paged
    pass  2nd access: OK (TLB hit)
    pass  still 1 walk -- the TLB absorbed it
    pass  L1 tag == frame 3 -- L1 can index before translation finishes
```

Separates the two kinds of miss that beginners conflate. A **TLB miss with the
PTE present** costs one memory access. A **page fault** costs a walk plus disk
plus an instruction restart. The three `MMU_OK_*` return codes exist precisely
so a caller can bill them differently.

## 5. Demand paging — and the right page arrives

```
    pass  page 37: OK (page fault serviced)
    pass  exactly 1 disk read
    pass  resident set grew to 3
    pass  the frame holds page 37 of pid 1 (marker 1/37)
```

The last check reads the frame's first bytes back. `disk_read_page()` writes a
deterministic marker derived from `(pid, vpn)`, so this proves the *right* page
landed in the *right* frame — not merely that a disk read was counted. Swapping
two pages during a fault is exactly the kind of bug a counter-only test misses.

## 6. Edge case — VA outside the 18-bit space

```
    pass  VA 0x40000: FAULT: address out of range
    pass  *pa_out untouched on a fault
    pass  rejected before any page-table access
    pass  counted as an address fault
    (without this check VA_VPN() would truncate 0x40000 to page 0)
```

`0x40000` is 2¹⁸ — one past the end. The parenthetical is the point:
`VA_VPN()` masks to 8 bits, so without an explicit range check this address
would resolve as **page 0** and silently read the wrong page. The
`*pa_out untouched` check guards the other half — a caller must never act on a
stale physical address after a fault.

## 7. Edge case — protection violation, before any disk work

```
    pass  write to a read-only page: FAULT: protection violation
    pass  no disk read was wasted on it
    pass  no frame was allocated for it
    pass  the page is still absent
    pass  reading the same page is allowed: OK (page fault serviced)
```

Ordering matters. Page 50 is both **absent and read-only**. A naive
implementation faults it in first and *then* discovers the write is illegal —
burning a frame, a disk read and possibly an eviction on an access that could
never succeed. `mmu_translate` checks `PTE.prot` immediately after the walk,
before the fault path. The final line shows a *read* of the same page proceeding
normally, so the check is discriminating by access type, not just rejecting the
page.

**Documented limitation, printed by the test itself:** after that fill,
protection is no longer re-checked on a TLB hit, because `TLBImplEntry` has no
`prot` bits and re-reading the PTE per access would cost a memory access every
time and defeat the TLB. Revoking write permission on a resident page is not
observed until its TLB entry is invalidated. The one-field fix — add `prot : 3`
to `TLBImplEntry` — needs `TLB.h`, which this work deliberately leaves alone.
The driver's `PROT` directive invalidates the entry for exactly this reason.

## 8. Edge case — unknown or inactive process

```
    pass  pid 99 was never created: FAULT: process not runnable
    pass  no Process record for pid 99
```
Guards the null-page-table dereference that would otherwise crash the simulator
on a malformed trace.

## 9. Writes set the PTE dirty bit

```
    pass  page 37 starts clean
    pass  PTE 37 is dirty -- meaning dirty vs DISK; both caches are
          write-through so no cache holds newer bytes
```
The comment is the substance. Because L1 and L2 are both write-through, no cache
line is ever the sole owner of new data, so there is **no dirty bit in either
cache** and no cache flush is needed before paging a frame out. The only dirty
bit in the system lives in the PTE and means "differs from the copy on disk".

## 10. LFU with aging — the shift register

Two pages: one referenced in every interval, one never referenced again.

```
    after 8 ticks: hot aging = 0xFF, cold aging = 0x00
    pass  a page used every interval saturates at 0xFF
    pass  an unused page decays to 0x00 -- LFU alone would have kept it
          forever on its old count
    pass  the reference bit is cleared by the tick
    pass  FrameDesc.aging mirrors PTE.aging
    pass  one touch puts a 1 in the MSB: 0x80
    pass  and an idle interval shifts the hot page to 0x7F -- recency
          outranks raw frequency
```

The last two lines are the whole argument for aging. Plain LFU has a famous
failure: a page hammered during initialisation accumulates a huge count and
becomes immortal. With aging, one touch on a cold page puts it at `0x80` while a
single idle interval drops the previously-hot page to `0x7F` — so the cold page
now outranks it. The register is exponential decay: recent references dominate
the high bits, old ones drain away.

## 11. Victim policy — lowest aging, skip page tables, honour floors

Three separate policy rules, tested one at a time on the same state.

```
    pass  picked frame 18 -- the lowest aging (0x01)
    pass  and it is not a page table
    pass  at lower_limit=8, pid 1's pages are skipped (victim now -1)
    pass  the victim no longer belongs to the pinned process
    pass  page-table frames are never chosen even at aging 0x00
```

The `-1` in the middle is a meaningful result, not an error: with pid 1 pinned
at its floor and nothing else evictable, there is genuinely no victim, and
`mm_select_victim` says so rather than violating the limit. That is what stops
global replacement from squeezing a process into thrashing. The last check
forces both page-table frames to aging `0x00` — the most attractive possible
victims — and confirms they are still never selected. Paging out a page table
would be unrecoverable: you would need a page table to find it.

## 12. `upper_limit` forces a process to evict its own page

```
    pass  resident set never exceeded upper_limit (peak 4)
    pass  and sits at the cap: 4 frames
    pass  memory was never short -- the cap alone forced the evictions
    20 pages touched, 4 frames allowed
```

The third line is what makes this a clean test: 32,764 frames were free the
whole time. Nothing was scarce. The evictions happened purely because the
process hit its own ceiling — **local replacement inside an otherwise global
policy**, which is exactly what the assignment's "upper limit strictly
maintained" clause requires.

## 13. Frame reuse invalidates the TLB — the correctness landmine

The most important test in the suite. Memory is filled, one of pid 1's
TLB-resident pages is made the coldest thing in the system, and a new page is
faulted in.

```
    pass  page 4 is cached in the TLB, mapping frame 47
    pass  main memory is full
    pass  faulting page 60: OK (page fault serviced)
    pass  the victim's PTE was cleared
    pass  its TLB entry is GONE -- otherwise a hit would hand back frame 47,
          which now holds page 60
    pass  1 TLB entries invalidated
    pass  the cache layer was told to invalidate frame 47 too
    pass  and page 60 was given exactly that frame -- the reuse is real
    pass  TLB now maps page 60 -> frame 47
```

The chain is complete and each link is checked: page 4 was in the TLB → its
frame was evicted → the PTE was cleared → the TLB entry died → the callback
fired → **the same frame 47 was handed to page 60** → the TLB now resolves page
60 to frame 47. Without the invalidation, a lookup of page 4 would still hit and
return frame 47, which now contains page 60's bytes. Silent data corruption with
no error anywhere.

The `told to invalidate frame 47 too` line records the cache-side callback. L1
and L2 are physically tagged, so their lines are stale too. That callback is a
stub until `l1.c`/`l2.c` export an invalidate-by-frame entry point; the
arithmetic they need is in `include/translate.h`:

```
frame f holds 64 blocks (1 KB / 16 B)
L1: tag == f                                  -- 64 sets x 4 ways
L2: tag == f >> 2,  sets ((f & 3) << 6) | 0..63
```

## 14. Cross-process eviction clears the VICTIM OWNER's PTE

```
    pass  pid 1 faults: OK (page fault serviced)
    pass  pid 1 took frame 57 from pid 2 (global replacement)
    pass  pid 2's PTE for page 5 was cleared -- not pid 1's
    pass  pid 2's resident set dropped 9 -> 8
    pass  and pid 2's TLB entry for it is gone
```

**This test exists because of a bug found while writing `MainMemory.c`.**
`mm_handle_fault(mm, proc, vpn)` receives only the *faulting* process, but
replacement is global — the victim can belong to anyone. Clearing the faulting
process's PTE instead of the victim owner's would leave pid 2 with a `present`
PTE pointing at a frame that now holds pid 1's page: silent, total corruption
with no fault raised.

The signature could not change (`MainMemory.h` is not modified), so
`MainMemory.c` receives the full process table through `mm_set_process_table()`,
which `mmu_init()` calls for you. This test is the regression guard.

## 15. Dirty victims are written back to disk

```
    pass  page 3 is dirty
    pass  1 write-back performed (1 total)
    both caches are write-through, so main memory already held the newest
    bytes -- no cache flush was needed first
```
A page written to, then forced out. In a write-back cache hierarchy the OS would
first have to flush the caches to get the newest bytes into memory. Here it does
not, and the note records why.

## 16. Process exit releases frames and TLB entries

```
    pass  pid 2 owns 8 TLB entries before exit
    pass  10 data frames + 1 page-table frame returned (free 32750 -> 32761)
    pass  the page-table frame is released -- only exit can free it, since
          replacement never touches it
    pass  every pid-2 TLB entry is gone -- PIDs are 14 bits and get recycled,
          so this is mandatory
    pass  pid 1 is completely unaffected
    pass  pid 2 is torn down
```

The `+1` is the point. Because page-table frames are pinned against replacement,
process exit is the **only** path that can ever free one. Miss it and every
terminated process leaks a frame permanently. The `pid 1 is completely
unaffected` check confirms the teardown is PID-scoped, not a blanket flush.

## 17. Block reads and write-through arrivals

```
    pass  1 block fetch counted
    pass  1 write-through arrival counted
    pass  the 4 written bytes are visible at block offset 0
    pass  and nothing outside them changed
```

The last check is the useful one: it compares the whole 16-byte block before and
after a 4-byte store and confirms only those 4 bytes moved. A `memcpy` with the
wrong length or a bad offset would corrupt neighbouring bytes inside the same
cache block — a bug that surfaces much later and looks like a cache problem.

---

# Coverage summary

| Concern | Where it is proved |
|---|---|
| Bit-field geometry | A-1 (compile time), A-2, B-1 |
| VIPT / L1 tag == frame | A-4, B-4 |
| TLB hit, miss, refresh | A-3, A-4, A-5, B-4 |
| LRU correctness | A-6, A-12, invariant checked everywhere |
| PID tagging, flush-free switch | A-7, A-8 |
| All three invalidation paths | A-9, A-10, A-11 |
| TLB reach and thrashing | A-14, A-15 |
| Pre-paging, `MIN_FRAMES_PER_PROC` | B-3 |
| Demand paging, correct page | B-5 |
| Address / protection / no-process faults | B-6, B-7, B-8 |
| Dirty bit semantics | B-9, B-15 |
| LFU with aging | B-10 |
| Victim policy, floors, pinned page tables | B-11 |
| `upper_limit` local replacement | B-12 |
| Frame reuse → TLB + cache invalidation | B-13, A-11 |
| Global replacement across processes | B-14 |
| Process teardown | B-16, A-10 |
| Byte-level memory access | B-17 |

**Not yet covered**, because the modules are unimplemented: everything in L1,
L2, the write buffer's interaction with the caches, and the exclusive
promote/demote path between L1 and L2.
