# Testing — quick reference

Run everything from the **repository root**.
Full reports land in `results/`; the terminal only shows verdict lines.

> Longer version with coverage, sanitizers and troubleshooting:
> [`tests/README.md`](tests/README.md)

---

## Run everything

```sh
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

Exits `0` only if every suite passes **and** every file compiles without
warnings.

---

## Test the TLB workflow

Pre-paging of the first 2 pages, and page-fault handling — 23 cases.

**Build:**

```sh
gcc -Wall -Wextra -g -I./include -o test_tlb_workflow \
    tests/test_tlb_workflow.c src/MainMemory.c src/translate.c src/TLB.c
```

**Run:**

```sh
./test_tlb_workflow
```

```
test_tlb_workflow       23 cases      0 failed   results/test_tlb_workflow.txt
```

**Read the report:**

```sh
less results/test_tlb_workflow.txt            # all 23 cases in full
grep -n Result results/test_tlb_workflow.txt  # just pass/fail per case
grep -B12 FAIL results/test_tlb_workflow.txt  # only what broke
```

Already built? Just `./test_tlb_workflow` — no rebuild needed unless you
changed the source.

---

## Where the output goes

```sh
./test_tlb_workflow                 # results/test_tlb_workflow.txt  (default)
./test_tlb_workflow report.txt      # a path you choose
./test_tlb_workflow -               # straight to the terminal
./test_tlb_workflow - | less        # ... and page through it
```

Same convention for every suite and for `tests/mutate_tlb.sh`.

---

## The other suites

```sh
# TLB unit tests -- LRU, PID tagging, the three invalidation paths
gcc -Wall -Wextra -g -I./include -o test_tlb tests/test_tlb.c src/TLB.c
./test_tlb

# Boundary values, PID recycling, 950,000 randomized ops vs a reference model
gcc -Wall -Wextra -g -I./include -o test_tlb_full tests/test_tlb_full.c src/TLB.c
./test_tlb_full

# Main memory + the full VA -> PA translation flow
gcc -Wall -Wextra -g -I./include -o test_mm \
    tests/test_mm.c src/MainMemory.c src/translate.c src/TLB.c
./test_mm
```

`test_tlb` and `test_tlb_full` link **only** `src/TLB.c`, so they run while
`l1.c` and `l2.c` are still unfinished.

---

## Prove the tests would catch a bug

Breaks `src/TLB.c` seven ways in a scratch copy and checks the suite notices
each one. Your `src/TLB.c` is never modified.

```sh
sh tests/mutate_tlb.sh
cat results/mutate_tlb.txt
```

`KILLED` = bug caught. **`SURVIVED` = a hole in the suite.**
`SKIP` = `TLB.c` changed and the script needs updating — not a pass.

---

## Build the simulator itself

```sh
make
./sim_q3 -v traces/demo.trace
./sim_q3 --help
```

---

## Reading a failure

The terminal line names the suite and the file:

```
test_tlb_workflow       23 cases      1 failed   *** FAILURES ***  results/test_tlb_workflow.txt
```

Then:

```sh
grep -n "FAIL\|MISMATCH" results/test_tlb_workflow.txt
```

Each case prints `Expected` against `Actual`, with the offending field tagged
`<-- MISMATCH`:

```
  Expected  : status                 = FAULT: protection violation
  Actual    : status                 = OK (TLB hit)     <-- MISMATCH
  Result    : *** FAIL ***
```

---

## Don't commit generated output

Add to `.gitignore`:

```
results/
obj/
sim_q3
test_tlb
test_tlb_full
test_tlb_workflow
test_mm
*.gcov
*.gcda
*.gcno
```
