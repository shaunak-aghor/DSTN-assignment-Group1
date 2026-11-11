DSTN ASSIGNMENT #2 -- QUESTION 3
Memory Hierarchy Simulator: TLB -> L1 -> L2 -> Main Memory



1. NAMES
--------------------------------------------------------------------------------

  Name                     ID
  -----------------------  --------------
  Abhinandan Jain          2023A7PS1025G
  Shaunak Aghor            2023A3PS0220G
  Christy George Joseph    2025H1030026G

Group number: 1, q3

Work split:

  Name                     Modules Implemented                         Contribution
  -----------------------  -------------------------------------------  ------------
  Abhinandan Jain          L1 cache, write buffer, L1/L2 write path      33%
  Shaunak Aghor            L2 cache, cache.c, mmu.c                     33%
  Christy George Joseph    main memory, page tables, TLB, driver        33%

Interaction with others outside the group: NONE


2. FILES IN THE DIRECTORY
--------------------------------------------------------------------------------

  README.txt     This file.
  Makefile       Build rules. Targets: all (default), clean.
  .gitignore     Keeps obj/, the binary and traces/ out of version control.

include/

  MemHier.h:    Every size, bit width and address-splitting macro for the
                whole simulator. Every other header includes it.
  stats.h:      The one global `stats` struct holding every counter.
  TLB.h:        TLBEntry (59 bits) and the TLB functions.
  L1.h:         L1Line (20 bits), L1Set, L1Cache and the L1 functions.
  L2.h:         L2Line (16 bits), L2Set with its 3-bit FIFO pointer, L2Cache
                and the L2 functions.
  WB.h:         WBEntry (24 bits), WriteBuffer (3 store slots + 1 swap slot).
  cache.h:      cache_read() / cache_write(), which drive L1, L2 and the WB.
  MM.h:         PTE, PageTable, FrameDesc, Process, MM and the main memory
                functions (page walk, page faults, replacement, aging).
  mmu.h:        va_to_pa().

src/

  TLB.c:        PID-tagged, fully associative TLB with LRU counters.
  l1.c:         L1: probe, LRU aging, victim selection, install, evict,
                invalidate-by-frame.
  l2.c:         L2: probe, FIFO victim selection, allocate, invalidate,
                invalidate-by-frame.
  WB.c:         The write buffer's store queue (FIFO, with coalescing).
  cache.c:      The look-aside search of L1 and L2 on two threads, the
                read and write paths, and the L1 <-> L2 swap.
  MM.c:         Main memory: frame table, 3-level page tables, page faults,
                LFU-with-aging replacement and the aging tick.
  mmu.c:        Virtual-to-physical translation: TLB, then page-table walk,
                then page fault.
  main.c:       The driver. Parses arguments, creates the processes, reads
                the trace, runs one access at a time, prints the report.

Generated / not in the repo:

  traces/    Unzip the instructor-supplied trace archive here (section 3).
  obj/       Object and dependency files, created by make.
  sim_q3     The binary produced by make.


3. HOW TO COMPILE AND HOW TO RUN
--------------------------------------------------------------------------------

COMPILING

    make

This compiles every .c in src/ into obj/ and links them into sim_q3.

  Compiler:  gcc
  Flags:     -Wall -Wextra -g -pthread -I./include -MMD -MP
  Requires:  POSIX threads (Linux).

-MMD -MP make the compiler record which headers each file uses, so changing
a header rebuilds every file that includes it.

    make clean    # removes obj/ and sim_q3

RUNNING

    unzip BenchmarkTraces-20260914.zip -d traces/
    ./sim_q3 <num_processes> <accesses_per_context_switch> <trace file>

For example:

    ./sim_q3 5 10 traces/2026_27_ISEM_CC1.txt

Arguments:

  num_processes:                 1 to 10000 (MAX_PROCS in main.c). Each
                                 process needs 5 frames to start, so at most
                                 6553 fit in memory.
  accesses_per_context_switch:   After this many accesses the running process
                                 changes to the next one. At least 1.
  trace file:                    Path to the address stream.

The trace is read once from start to end. Every
<accesses_per_context_switch> accesses the running process changes, so each
process sees a different slice of the same stream through its own address
space. It is NOT one trace per process.

The report on stdout gives hits, misses, evictions and stalls for every
level, then the pages and table frames each process holds.

CHANGING THE PARAMETERS

These are #defines at the top of src/main.c. Edit one, then run make:

  parameter               default    effect
  ----------------------  ---------  ------------------------------------------
  WRITE_PERCENT           30         share of accesses treated as stores
  WB_DRAIN_INTERVAL       3          accesses between background WB drains
  AGE_TICK_INTERVAL       1000       accesses between OS aging passes
  PROC_UPPER_LIMIT        0          max data pages per process (0 = no limit)
  PROC_LOWER_LIMIT        0          min data pages per process (0 = 2)

Sizes and bit widths are in include/MemHier.h.


4. STRUCTURE OF THE PROGRAM
--------------------------------------------------------------------------------

The simulator stores metadata only: tags, frame numbers and status bits.
There is no data anywhere. Each module updates the counters for its own
level in the global `stats` struct (stats.h), and main.c prints them.

CONFIGURATION (include/MemHier.h)

  Virtual address     32 bits = VPN 22 + page offset 10
                      VPN = outer 6 | middle 8 | inner 8   (3-level paging)
  Physical address    27 bits = frame 17 + page offset 10
                      This can address 128 MB, 2 bits more than the 32 MB
                      installed, so memory can grow later without changing
                      any address format. VA (32) > PA (27).
  Page / frame        1 KB, 32768 frames installed (32 MB)

  TLB                 32 entries, fully associative, PID-tagged, LRU
  L1                  4 KB, 16 B blocks, 4-way, LRU, write-through,
                      no-write-allocate, look-aside
  L2                  32 KB, 16 B blocks, 8-way, FIFO, write-through
  L1 and L2           EXCLUSIVE: a block is in one or the other, never both
  Write buffer        4 blocks in line with L1: 3 store slots + 1 swap slot
  Main memory         LFU with aging, global replacement, per-process limits

BIT LAYOUTS

  TLB entry    59 bits   valid 1 + vpn 22 + pfn 17 + pid 14 + lru 5
  L1 line      20 bits   valid 1 + tag 17 + lru 2
  L2 line      16 bits   valid 1 + tag 15         (+ 3-bit FIFO pointer per set)
  WB entry     24 bits   valid 1 + block address 23
  PTE          20 bits   present 1 + frame 17 + referenced 1 + dirty 1

REPLACEMENT POLICIES

  TLB and L1 (LRU counters): each entry has a counter, 0 = least recently
  used, N-1 = most recently used. On a use the entry becomes N-1 and every
  entry above its old value moves down by one. The victim is a free entry,
  or else the one at 0.

  L2 (FIFO pointer): each set has a 3-bit pointer to its oldest way. The
  victim is a free way, or else the way the pointer names, and the pointer
  then moves on by one (mod 8). We do not use an 8x8 FIFO matrix: that costs
  56 bits per set and is only worth it for very small caches.

  Main memory (LFU with aging): every frame has an 8-bit aging register.
  The victim is the data page with the smallest value (the coldest), skipping
  any process already at its lower limit. Page tables are never evicted.

PAGE TABLES (3 levels)

  Every table is one 1 KB frame of 256 four-byte PTEs (the outer table uses
  64 of them). The outer table is created with the process. Middle and inner
  tables are created only when a walk first needs them. Table frames are
  pinned: they are never chosen for eviction. In the outer and middle tables a
  PTE's `frame` is where the next table is; in the inner table it is where the
  page is.

  Every data frame's descriptor also points back at the PTE that maps it (a
  reverse map), so evicting a page or aging it needs no walk.

THE PATH OF ONE ACCESS

main.c reads one hex address from the trace and decides read or write, then:

(a) TRANSLATE -- mmu.c: va_to_pa()

    TLB hit    -> frame number, no memory access. A store also marks the
                  page dirty.

    TLB miss   -> 3-level page-table walk (3 memory reads, page tables are
                  never cached). Missing middle/inner tables are created.
               -> if the page is not present: page fault, MM.c: mm_handle_fault()
                    - if the process is at its upper limit, evict one of its
                      own pages
                    - take a free frame, or else evict the globally coldest
                      page (written back to disk if dirty)
               -> set Referenced, and Dirty if this is a store
               -> insert the mapping into the TLB

(b) INVALIDATE -- every frame reclaimed during (a) is listed in mm.evicted.
                  The TLB, L1 and L2 drop everything from those frames, since
                  the caches are physically tagged.

(c) CACHE -- cache.c. L1 and L2 are searched at the same time on two threads
             (look-aside). The CPU never reads the write buffer.

    Read:
        L1 hit   -> done, the line becomes most recently used
        L1 miss  -> if a store to this block is still in the WB, wait for the
                    WB to drain past it (L2 and memory are out of date until then)
                 -> L2 hit: swap the block into L1 / L2 miss: fetch from memory
                 -> the swap, through the WB's swap slot:
                      1. L1's victim     -> swap slot
                      2. the block       -> L1 (leaving L2, if it came from there)
                      3. the swap slot   -> L2

    Write (write-through, no-write-allocate):
        every store goes through the WB. If the WB has no free store slot,
        the CPU stalls until the oldest entry drains. Only then:
        L1 hit   -> L1 line updated (most recently used) and the store queued
        L1 miss  -> nothing is allocated; the store is queued and updates L2
                    (if L2 holds the block) and memory when it drains
        A store to a block already queued merges into that entry.

(d) BACKGROUND -- every WB_DRAIN_INTERVAL accesses one WB entry drains to
                  memory; every AGE_TICK_INTERVAL accesses the aging tick runs.

THE AGING TICK

mm_age_tick() is how LFU-with-aging gets its information. For every data page:

    1. read the page's Referenced bit
    2. shift it into the frame's aging register:
       aging = (aging >> 1) | (Referenced ? 0x80 : 0)
    3. clear the Referenced bit
    4. drop the page's TLB entry, so its next access walks the page table and
       sets Referenced again

MODULE DEPENDENCIES

    main.c  ->  mmu.h, MM.h, cache.h, stats.h
    cache.c ->  L1.h, L2.h, WB.h, stats.h
    mmu.c   ->  MM.h (which includes TLB.h and WB.h), stats.h
    MM.c    ->  MM.h, stats.h
    TLB.c, l2.c, WB.c  ->  stats.h
    everything  ->  MemHier.h


5. WHAT IS AND IS NOT IMPLEMENTED
--------------------------------------------------------------------------------

The simulator runs a trace from start to end and prints a full statistics
report.

IMPLEMENTED AND WORKING

  - 32-bit virtual to 27-bit physical translation
  - 3-level page tables kept in main memory frames, created on demand, pinned
  - PID-tagged, fully associative 32-entry TLB with LRU counters
  - Demand paging, with pages 0 and 1 of each process pre-paged
  - Global LFU-with-aging page replacement with per-process lower and upper
    limits, and dirty-page writeback
  - L1: 4-way, LRU counters, write-through, no-write-allocate, look-aside
    search on a separate thread from L2
  - L2: 8-way, FIFO pointer, write-through
  - L1/L2 exclusivity, with the swap done through the WB's swap slot
  - Write buffer in line with L1: 3 store slots with coalescing, stalls when
    full, never read by the CPU, plus 1 swap slot
  - Physically-tagged cache invalidation when a frame is reclaimed
  - Several processes time-sliced over one trace
  - Statistics for every level

NOT IMPLEMENTED

  - No cost model. Nothing has a latency, so the simulator reports counts but
    cannot compute AMAT. This also means look-aside and look-through look
    the same in the results: they differ only in timing.

  - No protection bits. Every page can be read and written.

  - Block granularity only. We never model which bytes in a block a store
    touched.

WHAT MADE IT HARD

  - The traces carry a bare hex address per line and nothing else: no PID,
    no read/write flag, no access size. Deciding how to tell reads from
    writes was tough.

  - Keeping L1 and L2 exclusive: a block has to leave one cache and arrive in
    the other with no moment where it is in neither or both. The WB's swap
    slot holds L1's victim during that exchange.

  - Keeping L1 and the write buffer consistent: L1 may only be written once
    the WB can accept the store, and a read may not use L2 or memory while a
    newer store to that block is still waiting in the WB.

  - Modelling page tables that really occupy frames of the simulated memory,
    created level by level as the walk needs them.


6. KNOWN BUGS AND LIMITATIONS
--------------------------------------------------------------------------------

1 -- Access type is made up

    The traces contain no read/write information. We make WRITE_PERCENT
    (30%) of accesses stores, using a seeded random number generator, so
    every run is reproducible. Nothing can recover information the trace
    does not carry.

2 -- Pinned page tables can fill memory

    Page-table frames are never evicted. An address pattern scattered over
    the whole 32-bit space needs a new inner table for almost every page, and
    with small memories the tables can fill it: the run then stops with an
    OUT OF MEMORY message. Traces with normal locality need only a few
    tables per process, so with 32768 frames this should not happen.
    Evicting empty inner tables would fix it.

3 -- Write buffer drain rate is a fixed interval

    Every store goes through the WB and about 3 in 10 accesses are stores,
    so the background drain runs every WB_DRAIN_INTERVAL = 3 accesses to
    keep up. With a larger interval (e.g. 10) the WB is full most of the
    time and most stores stall. A real WB drains whenever memory is free;
    we approximate that with this fixed interval.

4 -- The simulator is slow

    Every access starts two threads for the look-aside search, which costs
    much more than the search itself (about 50 s per 200,000 accesses).
    The L2 thread is stopped with an asynchronous pthread_cancel when L1
    hits. It has worked reliably in our runs, but asynchronous cancellation
    is risky in C in general.
