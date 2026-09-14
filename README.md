# DSTN Assignment #2 — Question 3

Memory-hierarchy simulator: **TLB → L1 → L2 → main memory**, in C.

Everything is **metadata only** — no byte arrays anywhere. Like a real cache
simulator, we track tags, frame numbers and state bits, never data. That is why
main memory is ~320 KB of structs rather than 32 MB of storage.

---

## Quick start

```sh
# 1. traces are NOT in the repo (4 MB of instructor-supplied data)
unzip BenchmarkTraces-20260914.zip -d traces/

# 2. build
make

# 3. run:  ./sim_q3 <num_processes> <accesses_per_context_switch> <trace file>
./sim_q3 5 10 traces/2026_27_ISEM_CC1.txt
```

`make clean` removes `obj/` and the binary.

### The three arguments

| arg | meaning |
|---|---|
| `num_processes` | 1–16 address spaces, each with its own page table |
| `accesses_per_context_switch` | after this many accesses the running PID advances |
| `trace file` | the address stream to run |

**One trace, time sliced.** The file is walked once from start to finish; every
`<accesses_per_context_switch>` accesses the owning PID rotates, so each process
sees a different slice of the same stream through its own address space. It is
not one trace per process.

Sanity check — `./sim_q3 5 10 traces/2026_27_ISEM_CC1.txt` should report
223,342 accesses, 22,334 context switches, TLB ~90.6%, L1 ~75%, L2 ~62%.

### What more processes actually cost

Same trace, same total work, only the number of address spaces changes — so
this isolates **interference**: five processes competing for one 32-entry TLB
and one 4 KB L1.

| procs | TLB hit% | TLB evictions | L1 hit% | L2 hit% | page faults |
|---|---|---|---|---|---|
| 1 | 98.45 | 0 | 95.15 | 10.39 | 153 |
| 2 | 96.81 | 1,467 | 91.79 | 20.06 | 297 |
| 5 | 90.59 | 13,967 | 74.98 | 62.49 | 711 |
| 10 | 82.55 | 31,832 | 56.70 | 72.68 | 1,382 |

L2's hit rate *rises* because it is exclusive: everything L1 loses lands there,
so more L1 misses means more L2 hits. Page faults scale with the process count
because each address space faults in its own copy of the working set.

---

## Q3 configuration

| | |
|---|---|
| **TLB** | 32 entries, fully associative, **PID-tagged** (no flush on context switch), LRU |
| **L1** | 4 KB, 16 B blocks, 4-way, LRU counter, **write buffer (4 blocks)**, look-aside |
| **L2** | 32 KB, 16 B blocks, 8-way, **FIFO**, write-through, look-through |
| **L1/L2** | **exclusive** — a block lives in one or the other, never both |
| **MM** | 32 MB = 32768 frames of 1 KB, **LFU with aging**, global replacement, pure paging |

---

## Files

| file | what it does |
|---|---|
| `include/MemHier.h` | all sizes, bit widths and address-decomposition macros |
| `TLB.{h,c}` | PID-tagged TLB: lookup, insert, three kinds of invalidation |
| `l1.{c}` / `L1.h` | L1: probe, LRU aging, victim selection, install, evict |
| `l2.{c}` / `L2.h` | L2: probe, FIFO matrix, promote to L1, allocate |
| `WB.{h,c}` | 4-block write buffer, FIFO, **coalescing** |
| `cache.{h,c}` | `cache_read` / `cache_write` — ties L1, L2 and the buffer together |
| `MM.{h,c}` | frame table, free list, page faults, LFU-with-aging replacement |
| `mmu.{h,c}` | `va_to_pa()` — translation and the page-table walk |
| `src/new_main.c` | the driver: reads traces, runs the loop, prints stats |

---

## How one access flows

This is the part worth reading — **the driver owns the ordering**, and each
module hands back what the next one needs.

```
  va_to_pa(tlb, mm, proc, va, acc, wb, &pa, &evicted)
      │  TLB hit?  -> done, MAIN MEMORY IS NEVER TOUCHED
      │  TLB miss? -> walk the page table (1 memory access)
      │               not present? -> mm_handle_fault()
      │                               drains the write buffer of the victim,
      │                               then reports the reclaimed frame
      │               set Accessed (and Dirty on a store) in the PTE
      │               insert into the TLB
      ▼
  driver: if a frame was reclaimed, scrub it from L1 and L2
          (they are PHYSICALLY tagged — their lines would serve the old page)
      ▼
  cache_write(...)                     |  cache_read(...)
    L1 hit  -> queue in write buffer   |    L1 hit  -> done
    WB hit  -> queue behind it         |    WB hit  -> forwarded
    else    -> straight to memory,     |    L2 hit  -> promote to L1,
               CPU stalls (L2 has no   |               demote L1's victim to L2
               buffer of its own)      |    miss    -> fetch, install in L1,
                                       |               demote L1's victim to L2
      ▼
  every 10 accesses:    drain one write-buffer entry to memory
  every 1000 accesses:  mm_age_tick() — the OS sampling pass
```

### The aging tick, and why it shoots down the TLB

`mm_age_tick()` is how LFU-with-aging actually gets its information:

1. read each resident page's **Accessed** bit out of the page table
2. shift it into that frame's 8-bit aging register: `aging = (aging>>1) | (A?0x80:0)`
3. **clear** the Accessed bit
4. **invalidate that TLB entry**

Step 4 is not optional. Hardware only sets the Accessed bit during a page-table
*walk*. If the translation stays cached, every further access is a TLB hit, the
walker never runs, and the bit can never be set again — a red-hot page would
look stone cold and get evicted. Dropping the entry forces the next access to
walk.

---

## Tunable knobs

Compile-time, in `src/new_main.c`, overridable without editing:

```sh
make clean && make CFLAGS="-Wall -Wextra -g -pthread -I./include -DPROC_UPPER_LIMIT=8"
```

| knob | default | effect |
|---|---|---|
| `WRITE_PERCENT` | 30 | share of accesses treated as stores |
| `WB_DRAIN_INTERVAL` | 10 | accesses between background buffer drains |
| `AGE_TICK_INTERVAL` | 1000 | accesses between OS sampling passes |
| `PROC_UPPER_LIMIT` | 0 (=256) | max frames per process |
| `PROC_LOWER_LIMIT` | 0 (=3) | min frames per process |

**`PROC_UPPER_LIMIT` is the interesting one.** At the default, 32768 frames is
far more than these traces touch, so **no page is ever evicted** and the
replacement policy never runs. Set it to 8 and you get ~27,000 evictions and
~12,000 write-backs.

`WB_DRAIN_INTERVAL` trades stalls against memory traffic, because draining
early defeats the buffer's write combining:

| interval | full stalls | memory writes |
|---|---|---|
| 1 | 0 | 110,984 |
| 3 | 71 | 94,290 |
| 10 (default) | 26,100 | 70,986 |
| never | 59,773 | 67,885 |

---

## Known limitations

1. **Virtual addresses are masked to 18 bits.** `MemHier.h` defines an 18-bit
   address space but the traces carry 32-bit addresses, so the low 18 bits are
   used. Addresses differing only above bit 17 alias — CC1 touches 169 distinct
   pages but only 156 survive. The fix is a hierarchical page table (a 22-bit
   VPN needs 3 levels; ~5 frames per process instead of 16,384 for a flat one).

2. **Traces carry no read/write information** — every line is a bare address.
   The 70/30 split is synthesised by a seeded LCG, so runs are reproducible but
   the workload's real write pattern is not modelled.

3. **No page replacement at default limits** — see `PROC_UPPER_LIMIT` above.

4. **`cache.c` uses real threads** to model L1's look-aside lookup. Look-aside
   is a *timing* property, so this buys nothing a simulator can observe; the
   sequential result is identical. Also, `sem_init` is unimplemented on macOS
   (works on Linux).

5. **Write-buffer forwarding is block-granular.** An entry records a 16 B
   block, not which bytes, so a load forwards from a queued store even when the
   store touched different bytes of that block.

6. **No protection checking.** `TLBEntry` has no protection bits, so `prot` in
   the PTE is set but never enforced.

7. **No cost model.** Nothing has a latency, so there is no AMAT — only hit and
   miss counts. Look-aside vs look-through is invisible without it.

---

## Where the old code went

`MainMemory.{c,h}`, `translate.{c,h}`, `old_main.c`, `tests/` and `docs/` were
removed on the `finalizing` branch. They are all still in git history:

```sh
git checkout main -- src/MainMemory.c      # etc.
```
