# Testing the TLB workflow

Every suite writes its **full report to `results/<name>.txt`** and prints only a
one-line verdict to the terminal. Nothing floods the screen.

---

## Quick start — run everything

```sh
cd <repo root>
sh tests/run_all.sh
```

```
Building and running all suites ...

test_tlb               135 checks     0 failed   results/test_tlb.txt
test_tlb_full           15 scenarios  0 failed   results/test_tlb_full.txt
test_tlb_workflow       23 cases      0 failed   results/test_tlb_workflow.txt
test_mm                 83 checks     0 failed   results/test_mm.txt
mutate_tlb               7 killed   0 survived   results/mutate_tlb.txt

ALL SUITES PASSED.  Reports in results/
```

Exit status is `0` only if every suite passed *and* every file compiled without
warnings. A suite that compiles with warnings is reported as a failure and its
compiler output is kept in `results/<name>.build.log`.

---

## The TLB workflow suite on its own

This is the one that covers pre-paging and page-fault handling.

```sh
gcc -Wall -Wextra -g -I./include -o test_tlb_workflow \
    tests/test_tlb_workflow.c src/MainMemory.c src/translate.c src/TLB.c

./test_tlb_workflow
```

```
test_tlb_workflow       23 cases      0 failed   results/test_tlb_workflow.txt
```

Then read the report:

```sh
less results/test_tlb_workflow.txt          # all 23 cases
grep -n "Result" results/test_tlb_workflow.txt   # just pass/fail per case
grep -B12 "FAIL" results/test_tlb_workflow.txt   # only what broke
```

Each case is reported like this:

```
TC-03  First touch of a PRE-PAGED page: TLB miss, but no fault
==========================================================================
  Input     : read VA 0x00434  (page 1, offset 0x034)
  Setup     : page 1 pre-paged and present; TLB empty
  Expected  : status                 = OK (TLB miss, PTE present)
              TLB misses             = 1
              page-table walks       = 1
              page faults            = 0
              disk reads             = 0
  Actual    : status                 = OK (TLB miss, PTE present)
              ...
  Result    : PASS
  Why       : ...
```

Any field that differs is flagged inline with `<-- MISMATCH`, so a failure
points straight at the value that went wrong rather than just saying "failed".

---

## Choosing where the output goes

```sh
./test_tlb_workflow                       # results/test_tlb_workflow.txt
./test_tlb_workflow report.txt            # a path you choose
./test_tlb_workflow -                      # straight to the terminal
./test_tlb_workflow - | less               # ... and page through it
sh tests/mutate_tlb.sh -                   # same convention for the script
```

---

## All four suites individually

```sh
# 1. TLB unit tests -- LRU, PID tagging, the three invalidation paths
gcc -Wall -Wextra -g -I./include -o test_tlb tests/test_tlb.c src/TLB.c
./test_tlb

# 2. Boundary values, field truncation, PID recycling, and 950,000
#    randomized operations compared against an independent reference model
gcc -Wall -Wextra -g -I./include -o test_tlb_full tests/test_tlb_full.c src/TLB.c
./test_tlb_full

# 3. TLB WORKFLOW -- pre-paging and page-fault handling  (23 cases)
gcc -Wall -Wextra -g -I./include -o test_tlb_workflow \
    tests/test_tlb_workflow.c src/MainMemory.c src/translate.c src/TLB.c
./test_tlb_workflow

# 4. Main memory and the full VA -> PA translation flow
gcc -Wall -Wextra -g -I./include -o test_mm \
    tests/test_mm.c src/MainMemory.c src/translate.c src/TLB.c
./test_mm
```

`test_tlb` and `test_tlb_full` link **only** `src/TLB.c`, so they run even while
`l1.c` and `l2.c` are unfinished.

---

## Proving the tests would catch a bug

A passing suite only proves the tests passed. This deliberately breaks
`src/TLB.c` seven ways in a scratch copy and checks the suite notices each one:

```sh
sh tests/mutate_tlb.sh
cat results/mutate_tlb.txt
```

```
  KILLED  M1  comparator ignores the PID
  KILLED  M2  select_victim always returns entry 0
  KILLED  M3  touch() does not age the other entries
  KILLED  M4  invalidate_pid matches nothing
  KILLED  M5  insert skips the duplicate check
  KILLED  M6  forget() skips the rank repair
  KILLED  M7  invalidate_frame matches nothing
  killed 7, survived 0
```

`KILLED` = the bug was caught. **`SURVIVED` means a hole in the suite** — a bug
the tests cannot see. `SKIP` means `TLB.c` changed so a pattern no longer
matches, and the script needs updating; it does **not** mean you are fine.

`src/TLB.c` is never modified. Confirm with `git status src/TLB.c` afterwards.

---

## Deeper checks

**Line coverage** — which lines never executed:

```sh
gcc --coverage -O0 -I./include -o cov_tlb tests/test_tlb.c src/TLB.c
./cov_tlb -
gcov cov_tlb-TLB.gcda | grep -A1 "File 'src/TLB.c'"
grep -n '####' TLB.c.gcov          # lines never run
```

Expect `Lines executed:100.00% of 98` for `TLB.c`.

**Sanitizers** — catches out-of-bounds access and bitfield overflow that no
assertion would notice:

```sh
gcc -g -O1 -fsanitize=address,undefined -I./include -o san_tlb \
    tests/test_tlb_workflow.c src/MainMemory.c src/translate.c src/TLB.c
./san_tlb
```

A clean run prints nothing extra and exits 0.

---

## Reading a failure

The terminal line tells you which suite and which file to open:

```
test_tlb_workflow       23 cases      1 failed   *** FAILURES ***  results/test_tlb_workflow.txt
```

```sh
grep -n "FAIL\|MISMATCH" results/test_tlb_workflow.txt
```

The `test_tlb` and `test_mm` suites additionally print the source line of the
failed check:

```
    FAIL  vpn 0x11 was evicted -- it was the LRU
          ^ in "LRU -- the 33rd fill evicts the true LRU entry" at tests/test_tlb.c:312
```

---

## What is covered

| Suite | Covers |
|---|---|
| `test_tlb.c` | LRU counters, PID tagging, flush-free context switch, the three invalidation paths, TLB reach, statistics |
| `test_tlb_full.c` | Every bit field at its extremes, field truncation, PID recycling, and 950,000 randomized ops diffed against a reference model on hit/miss, frame, resident set **and recency order** |
| `test_tlb_workflow.c` | **Pre-paging** (TC-01…03, 10, 12, 20) and **page faults** (TC-05, 06, 11, 16, 17, 18, 19, 21), plus address / protection / dead-process faults and process exit |
| `test_mm.c` | Frame allocation, LFU-with-aging, victim policy, `lower_limit` / `upper_limit`, cross-process eviction, write-back |
| `mutate_tlb.sh` | Whether the above would actually catch a regression |

**Not covered:** `l1.c`, `l2.c`, `cache.c`, `wb.c`, and the exclusive
promote/demote path between L1 and L2 — those modules are still unimplemented.

`cache.c` also uses `sem_init()`, which is **not implemented on macOS** (returns
`-1`/`ENOSYS`). It compiles and links, but calling `cache_search()` there will
misbehave. No suite calls it.

---

## Housekeeping

`results/` is generated output and should not be committed. Add to `.gitignore`:

```
results/
test_tlb
test_tlb_full
test_tlb_workflow
test_mm
*.gcov
*.gcda
*.gcno
```
