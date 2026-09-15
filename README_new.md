================================================================================
  DSTN Assignment #2 -- Question 3
  Memory hierarchy simulator: TLB -> L1 -> L2 -> Main Memory
  CS F446, I Semester 2026-2027   |   BITS Pilani, K. K. Birla Goa Campus
================================================================================


1. NAMES
================================================================================
  Shaunak Aghor            <<< contribution % and modules >>>
  Abhinandan Jain          <<< contribution % and modules >>>
  Christy George Joseph    <<< contribution % and modules >>>

  Interaction with others outside the group: <<< none / list them >>>


2. FILES
================================================================================
  Makefile              Builds ./sim_q3.

  include/
    MemHier.h           All sizes, bit widths and address macros. Every other
                        header includes it; nothing else defines geometry.
    TLB.h  / src/TLB.c        32 entries, fully associative, PID-tagged, LRU.
    L1.h   / src/l1.c         4 KB, 16 B blocks, 4-way, LRU counter.
    L2.h   / src/l2.c         32 KB, 16 B blocks, 8-way, FIFO matrix.
    WB.h   / src/WB.c         4-block write buffer, FIFO, coalescing.
    cache.h/ src/cache.c      cache_read / cache_write: L1 + L2 + buffer.
    MM.h   / src/MM.c         Frame table, page faults, LFU-with-aging.
    mmu.h  / src/mmu.c        va_to_pa(): translation and the page-table walk.

  src/new_main.c        Driver: arguments, setup, access loop, statistics.

  traces/               Not included. Unzip the supplied benchmarks here.


3. COMPILE AND RUN
================================================================================
  COMPILE
      make                  builds ./sim_q3
      make clean            removes obj/ and the binary

  TRACES (needed before running)
      unzip BenchmarkTraces-20260914.zip -d traces/

  RUN
      ./sim_q3 <num_processes> <accesses_per_context_switch> <trace file>

      Example:
          ./sim_q3 5 10 traces/2026_27_ISEM_CC1.txt

      num_processes                1..16, each with its own address space
      accesses_per_context_switch  the running PID advances after this many
      trace file                   one bare hex virtual address per line

      The trace is walked ONCE. The owning process rotates every quantum, so
      each process sees a different slice of the same stream through its own
      page table. It is not one trace per process.

  TUNING
      Five constants at the top of src/new_main.c; edit and rebuild.

      WRITE_PERCENT      30     share of accesses treated as stores
      AGE_TICK_INTERVAL  1000   accesses between OS aging passes
      WB_DRAIN_INTERVAL  10     accesses between background buffer drains
      PROC_LOWER_LIMIT   3      min frames per process (page table included)
      PROC_UPPER_LIMIT   256    max frames per process

      PROC_UPPER_LIMIT matters most. 32768 frames is far more than these
      traces need, so at 256 nothing is ever evicted and page replacement
      never runs. Lower it to 8 to exercise it.


4. STRUCTURE
================================================================================
  Each module owns one structure and one policy. No module keeps statistics --
  each reports what happened through its return value, and the driver counts.
  The DRIVER owns the ordering.

  ONE ACCESS

    va_to_pa()
        TLB hit   -> done. Memory is not touched and the Accessed bit is not
                     re-set; that is what makes a TLB worth having.
        TLB miss  -> walk the page table (one memory access: page tables live
                     in memory and are never cached)
                     not present -> mm_handle_fault(): drain the buffer of any
                                    store to the victim, reclaim a frame,
                                    report it back
                     set Accessed (and Dirty on a store), fill the TLB
        |
        v
    driver: if a frame was reclaimed, invalidate it in L1 and L2.
            va_to_pa cleared the TLB itself, but L1/L2 are physically tagged
            and their lines would serve the old page.
        |
        v
    WRITE                              READ
      L1 hit -> buffer it                L1 hit -> done
      WB hit -> queue behind it          WB hit -> forwarded
      else   -> straight to memory,      L2 hit -> promote to L1, demote L1's
                CPU stalls (L2 has                 victim into L2 (exclusive)
                no buffer of its own)    miss   -> fetch, install in L1, demote
        |                                          L1's victim into L2
        v
    every 10 accesses    drain one buffer entry to memory
    every 1000 accesses  mm_age_tick(): the OS sampling pass

  THE AGING TICK
    Hardware gives one bit per page: the PTE Accessed bit, set only by the
    page-table walker. mm_age_tick() reads it, shifts it into the frame's
    8-bit aging register, clears it, and INVALIDATES that TLB entry. The last
    step is essential -- if the translation stays cached the walker never
    runs again, the bit can never be set, and a hot page reads as cold.

  DESIGN POINTS
    - L1 and L2 are EXCLUSIVE: a block is in one or the other, never both.
      Every path that moves a block up also moves the displaced one down.
    - Page tables occupy real frames, count against each process's budget,
      and are pinned so replacement can never choose them.
    - Main memory is metadata only, like the caches. There is no 32 MB array.
    - The TLB is PID-tagged, so a context switch flushes nothing.
    - The write buffer COALESCES: repeated stores to one 16 B block share an
      entry, which is what makes its capacity 4 BLOCKS as Q3 specifies.


5. WHAT IS DONE, AND WHAT IS NOT
================================================================================
  DONE
    - TLB, L1, L2, write buffer and main memory to the Q3 specification.
    - Address translation with page faults, and correct invalidation of the
      TLB, L1 and L2 whenever a frame is reclaimed.
    - Global LFU-with-aging replacement with per-process frame limits,
      resident pinned page tables, and 2-page pre-paging.
    - Multiprogramming: N processes time-slicing one stream, configurable
      context-switch quantum.
    - Full statistics for every level.

  NOT DONE / FUTURE WORK
    a) HIERARCHICAL PAGE TABLES.  The address space is 18 bits with a flat
       256-entry page table, so the driver masks each 32-bit trace address to
       its low 18 bits. Addresses differing only above bit 17 then alias: on
       CC1, 169 distinct pages become 156.
       Next step: a 3-level table. A 22-bit VPN split 6|8|8 gives nodes of
       256 entries = 1 KB = exactly one frame. A flat 22-bit table would need
       16 MB per process, which is impossible; measured on these traces a
       3-level table needs only 5-6 frames per process, because the addresses
       cluster into a heap region and a stack region.

    b) REAL ACCESS TYPES.  The traces carry addresses only -- no read/write
       flag. 30% of accesses are marked as stores by a seeded generator, so
       runs are reproducible but the true store pattern is not modelled.

    c) PROTECTION.  PTE.prot is set to RWX and never checked. It could not be
       checked meaningfully anyway, since the access type is synthesised.
       Next step: protection bits in the TLB entry and a trace format that
       carries the real access type.

    d) A COST MODEL.  Nothing has a latency, so the simulator reports hit and
       miss counts but no AMAT. This is also why look-aside and look-through
       cannot be told apart in the output: the difference is purely timing.
       Next step: charge max(t_L1, t_L2) on an L1 miss instead of the sum,
       and report AMAT. That would also let cache.c drop its two threads,
       which model look-aside but produce cache state identical to a plain
       sequential probe.

    e) l2_write_through() is declared in L2.h but not implemented. It is not
       called: L2's write-through behaviour is handled in cache_write().
       It should be implemented or the declaration removed.

  GREATEST DIFFICULTIES
    - The traces contain only addresses. Q3's write buffer, L2 write-through
      and the dirty bit all need stores to exist, so the access type had to
      be synthesised.
    - Invalidation ordering when a frame is reclaimed. A stale TLB entry is
      worse than a stale cache line: a TLB hit skips the page table, so there
      is no fault to catch it and a process silently reads another's memory.
      Hence the TLB is always cleared first.
    - The write buffer against page replacement. A queued store holds a
      physical address, so if its frame is reclaimed first the write lands in
      the wrong page. Eviction therefore drains the buffer of that frame
      before taking it.


6. KNOWN BUGS
================================================================================
  1. WRITE-BUFFER FORWARDING IS BLOCK-GRANULAR.
     An entry records a 16 B block, not which bytes are pending, so a load of
     any byte in a queued block is reported as a buffer hit even when the
     store touched different bytes. This over-reports forwards.
     Fix: store the byte offset (the entry has 10 spare bits) and compare the
     requested range against it.

  2. A STORE THAT STRADDLES TWO BLOCKS IS RECORDED IN ONLY ONE.
     STORE_WIDTH is 4 B and a block is 16 B, so a store at offset 13-15 spans
     into the next block; only the first is queued.
     Fix: split such a store into two entries.

  3. STORE ORDERING IS NOT PRESERVED ACROSS THE TWO WRITE PATHS.
     A store that hits L1 is buffered while one that misses goes straight to
     memory, so the second can reach memory first. Not observable here (one
     CPU, no DMA, no data in the hierarchy, and loads check the buffer), but
     it would matter on a multiprocessor.
     Fix: route every store through the buffer, as real hardware does.

  NO CRASHES, LEAKS OR RUN-TIME ERRORS.
  Runs to completion on all five traces with 1, 2, 3, 5, 10 and 20 processes
  and a range of quanta. Checked with leaks(1): 0 leaks, 0 bytes.

  CONSISTENCY CHECKS
  These identities are verified to hold on every run, and are the main
  evidence that the statistics are correct:

      L1 misses (read + write)        == L2 accesses (hits + misses)
      L1 read misses - L2 promotions  == block fetches from memory
      WB drains + unbuffered stores   == write arrivals at memory
      TLB hits + TLB misses           == total accesses

================================================================================
