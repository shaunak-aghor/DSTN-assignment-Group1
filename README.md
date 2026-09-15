# DSTN Assignment #2 — Question 3

Memory hierarchy simulator: TLB → L1 → L2 → main memory

---

## 1. Names

| Name                  | ID            |
| --------------------- | ------------- |
| Abhinandan Jain       | 2023A7PS1025G |
| Shaunak Aghor         | 2023A3PS0220G |
| Christy George Joseph | 2025H1030026G |

**Group number:** 1, q3

**Work split**

| Name                  | Modules Implemented                      | Contribution |
| --------------------- | ---------------------------------------- | ------------ |
| Abhinandan Jain       | L1 cache, write buffer, L1/L2 write path | 33%          |
| Shaunak Aghor         | L2 cache, cache.c, mmuu.c                | 33%          |
| Christy George Joseph | main memory, page tables, TLB, driver    | 33%          |

**Interaction with others outside the group:** NONE

---

## 2. Files in the directory

| file           | description                                                       |
| -------------- | ----------------------------------------------------------------- |
| `README`     | The plain-text submission README.                                 |
| `README.md`  | This file — the same content, formatted for GitHub.              |
| `Makefile`   | Build rules. Targets:`all` (default), `clean`, `tar`.       |
| `.gitignore` | Keeps`obj/`, the binary and `traces/` out of version control. |

### `include/`

| file          | description                                                                                                                                               |
| ------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `MemHier.h` | Every size, bit width and address-decomposition macro for the whole simulator. Nothing here depends on any component, and every other header includes it. |
| `TLB.h`     | `TLBEntry` (43 bits) and the TLB API.                                                                                                                   |
| `L1.h`      | `L1Line` (18 bits), `L1Set`, `L1Cache` and the L1 API.                                                                                              |
| `L2.h`      | `L2Line` (14 bits), `L2Set` with its FIFO matrix, `L2Cache` and the L2 API.                                                                         |
| `WB.h`      | `WBEntry` (22 bits), `WriteBuffer` and its API.                                                                                                       |
| `cache.h`   | `CacheSearchResult` plus `cache_read()` / `cache_write()`, which drive L1, L2 and the write buffer together.                                        |
| `MM.h`      | `PTE`, `PageTable`, `FrameDesc`, `Process`, `MM`, and the main memory API (allocation, faults, aging, replacement).                             |
| `mmu.h`     | `va_to_pa()` and the `XlateResult` flags it returns.                                                                                                  |

### `src/`

| file           | description                                                                                                                                                                   |
| -------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `TLB.c`      | PID-tagged TLB. Fully associative lookup on the pair`(pid, vpn)`, LRU-counter replacement, and three separate invalidation paths (single page, whole process, whole frame). |
| `l1.c`       | L1: probe, LRU aging, victim selection, install, evict and invalidate-by-frame.                                                                                               |
| `l2.c`       | L2: probe, FIFO-matrix aging, victim selection, allocate, promote-to-L1 (which also demotes L1's victim back down), invalidate and invalidate-by-frame.                       |
| `WB.c`       | 4-entry FIFO write buffer with coalescing: a store to a block already queued is absorbed into the existing entry instead of taking a second slot.                             |
| `cache.c`    | `cache_read()` and `cache_write()`. Runs the L1 and L2 lookups on two threads to model L1's look-aside behaviour, then applies the result.                                |
| `MM.c`       | Main memory: 32768 frame descriptors, page tables held inside frames, page-fault handling, LFU-with-aging global replacement, and the aging tick.                             |
| `mmu.c`      | Virtual-to-physical translation. TLB lookup first, then a page-table walk, then a page fault if needed.                                                                       |
| `new_main.c` | The driver. Parses arguments, creates the processes, reads the trace, runs one access at a time, owns every statistic, and prints the report at the end.                      |

### Generated / not in the repo

| path        | description                                                                                            |
| ----------- | ------------------------------------------------------------------------------------------------------ |
| `traces/` | Not in the repo. Unzip the instructor-supplied trace archive into here before running (see section 3). |
| `obj/`    | Created by`make`. Object files. Removed by `make clean`.                                           |
| `sim_q3`  | The binary produced by`make`. Removed by `make clean`.                                             |

---

## 3. How to compile and how to run

### Compiling

```sh
make
```

That is all. It creates `obj/`, compiles every `.c` in `src/`, and links them
into an executable called `sim_q3` in the top-level directory.

- **Compiler:** gcc
- **Flags:** `-Wall -Wextra -g -pthread -I./include`
- **Requires:** POSIX threads. This builds and runs on Linux.

```sh
make clean    # removes obj/ and sim_q3
make          # after a clean, rebuilds everything
```

### Running

First put the traces in place:

```sh
unzip BenchmarkTraces-20260914.zip -d traces/
```

Then:

```sh
./sim_q3 <num_processes> <accesses_per_context_switch> <trace file>
```

For example:

```sh
./sim_q3 5 10 traces/2026_27_ISEM_CC1.txt
```

**Arguments**

| argument                        | meaning                                                                                    |
| ------------------------------- | ------------------------------------------------------------------------------------------ |
| `num_processes`               | 1 to 20. Each gets its own address space and its own page table.                           |
| `accesses_per_context_switch` | After this many accesses the running PID advances to the next process. Must be at least 1. |
| `trace file`                  | Path to the address stream.                                                                |

The trace is walked once from beginning to end. Every
`<accesses_per_context_switch>` accesses the owning process rotates, so each
process sees a different slice of the same stream through its own address
space. **It is NOT one trace per process.**

Output goes to stdout: a per-level breakdown of hits, misses, evictions and
page faults, then a per-process summary of frames held.

### Changing the parameters

Five values are set as `#define`s at the top of `src/new_main.c`. To change one,
edit it and rebuild:

| knob                  | default  | effect                                    |
| --------------------- | -------- | ----------------------------------------- |
| `WRITE_PERCENT`     | 30       | share of accesses treated as stores       |
| `WB_DRAIN_INTERVAL` | 10       | accesses between background buffer drains |
| `AGE_TICK_INTERVAL` | 1000     | accesses between OS sampling passes       |
| `PROC_UPPER_LIMIT`  | 0 (=256) | max frames per process                    |
| `PROC_LOWER_LIMIT`  | 0 (=3)   | min frames per process                    |

```sh
# edit the value in src/new_main.c, then
make clean && make
```

---

## 4. Structure of the program

The driver calculates all the statistics (hits, misses, evictions, promotes,
etc.). Each module implements one structure, does exactly what it is asked, and
returns enough information for the driver to decide what happens next. No module
keeps counters of its own.

The simulator stores metadata only. There are no byte arrays anywhere. We track
tags, frame numbers and state bits, never the data itself. That is why main
memory is much smaller than the 32 MB of storage specified.

### Configuration (all from `include/MemHier.h`)

|                  |                                                                                                |
| ---------------- | ---------------------------------------------------------------------------------------------- |
| Virtual address  | 18 bits = VPN 8 + page offset 10 (no hierarchical paging)                                      |
| Physical address | 25 bits = frame 15 + page offset 10                                                            |
| Page / frame     | 1 KB, so 32768 frames in 32 MB                                                                 |
| TLB              | 32 entries, fully associative, PID-tagged, LRU                                                 |
| L1               | 4 KB, 16 B blocks, 4-way, LRU counter, write buffer of 4 blocks, look-aside, no-write-allocate |
| L2               | 32 KB, 16 B blocks, 8-way, FIFO, write-through, look-through                                   |
| L1 and L2        | **EXCLUSIVE**: a block is in one or the other, never both                                |
| Main memory      | LFU with aging, global replacement, pure paging                                                |

### The path of one access

`new_main.c` reads one hex address from the trace and decides read or write,
then:

**(a) TRANSLATE** — `mmu.c: va_to_pa()`

```
TLB hit    -> frame number, and main memory is never touched
TLB miss   -> walk the page table, which costs one real main-memory
              access because page tables are never cached
           -> if page not present then MM.c: mm_handle_fault()
                - free frame if one exists
                - otherwise pick a global LFU-with-aging victim,
                  write it back if dirty, clear the owner's PTE
                - report the reclaimed frame back to the driver
           -> set Accessed, and Dirty if this is a store
           -> insert the mapping into the TLB
```

**(b) INVALIDATION** — if a frame was reclaimed, the driver invalidates every L1
and L2 line belonging to it. Both caches are physically tagged, so a surviving
line would hand back the previous page's contents.

**(c) CACHE** — `cache.c`

```
On a read (cache_read):
    L1 hit  -> done, line becomes most recently used
    WB hit  -> the block has a store pending, forwarded
    L2 hit  -> l2_promote(): invalidate in L2, evict L1's victim,
               install the block in L1, demote that victim into L2
    miss    -> the driver fetches the block from memory, evicts
               L1's victim, installs, and demotes the victim to L2

On a write (cache_write), no-write-allocate:
    L1 hit  -> promote the line, queue the store in the write buffer
    WB hit  -> queue behind the pending store so ordering holds
    otherwise -> nothing is installed in either cache; the store goes
               straight to memory and the CPU stalls, because L2 has
               no buffer of its own
```

**(d) BACKGROUND** — every `WB_DRAIN_INTERVAL` accesses one write-buffer entry
drains to memory, every `AGE_TICK_INTERVAL` accesses `mm_age_tick()` runs the OS
sampling pass.

### The aging tick

`mm_age_tick()` is how LFU-with-aging gets its information:

1. read each resident page's Accessed bit from the page table
2. shift it into that frame's 8-bit aging register: `aging = (aging >> 1) | (A ? 0x80 : 0)`
3. clear the Accessed bit
4. invalidate that page's TLB entry

### Module dependencies

```
new_main.c  ->  mmu.h, MM.h, cache.h
cache.c     ->  L1.h, L2.h, WB.h
l2.c        ->  L1.h        (l2_promote drives both caches)
mmu.c       ->  TLB.h, MM.h
everything  ->  MemHier.h
```

---

## 5. What is and is not implemented

The assignment is complete in the sense that every required structure is
implemented and the simulator runs all five benchmark traces end to end without
crashing, producing a full statistics report.

### Implemented and working

- 18-bit virtual to 25-bit physical translation
- PID-tagged, fully associative 32-entry TLB with LRU replacement and three
  invalidation paths
- Page tables resident in main memory frames, never cached
- Demand paging with pre-paging of the first two pages per process
- Global LFU-with-aging page replacement, adhering to per-process lower and
  upper frame limits
- L1: 4-way, LRU counter, no-write-allocate, look-aside
- L2: 8-way, FIFO via an 8×8 triangular matrix, write-through
- L1/L2 exclusivity, including the swap on an L2 hit
- 4-entry FIFO write buffer with coalescing and background draining
- Physically-tagged cache invalidation when a frame is reclaimed
- Multi-process time slicing over a single trace
- Full statistics for every level

### Not implemented

- **No cost model.** Nothing carries a latency / clock cycles, so the simulator
  reports hit and miss counts but cannot compute AMAT or effective access time.
  This also means look-aside versus look-through is invisible: the two differ
  only in timing, and we have no timing.
- **No protection enforcement.** `TLBEntry` has no protection bits, so `prot` in
  the PTE is set at fault time but never checked on an access. A write to a
  read-only page is not caught.
- **Byte-level granularity.** Everything is tracked at 16-byte block
  granularity, so we never model which bytes within a block a store touched.

### What made it hard

- The traces carry a bare hex address per line and nothing else: no PID, no
  read/write flag, no access size. Determining a way to differentiate between
  reads and writes was tough.
- Exclusivity between L1 and L2 was hard, because a block has to leave one cache
  and arrive in the other with no window where it is in neither or both. Getting
  the eviction/install ordering right in `l2_promote` took several attempts.
- Modelling the memory in such a way that the page tables are modelled and
  consume simulator and simulated machine memory, but the data frames only
  consume the simulated machine memory.
- Modelling the 18-bit VA and the 25-bit PA without hierarchical paging.

---

## 6. Known bugs

### Bug 1 — virtual addresses are truncated, and access type is synthesised

**Symptom**

Two separate compromises forced by the trace format, which gives one bare 32-bit
hex address per line and nothing else.

**Cause and impact**

**(a)** `MemHier.h` defines an 18-bit virtual address space but the traces carry
32-bit addresses, so we keep the low 18 bits. Addresses differing only above bit
17 collapse onto the same page.

**(b)** The traces contain no read/write information. We synthesise a 70/30
split with a seeded linear congruential generator, so runs are reproducible.

**How we would fix it**

**(a)** A hierarchical page table. A 22-bit VPN split three ways would need about
5 frames per process instead of the 16,384 a flat table would take, and would
remove the truncation entirely.

**(b)** Nothing can recover information the trace does not carry.
