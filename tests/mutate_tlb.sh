#!/bin/sh
# ============================================================================
#  tests/mutate_tlb.sh -- do the TLB tests actually have teeth?
#
#  A passing test suite only proves the tests passed. This script proves the
#  tests would CATCH a bug: it deliberately breaks src/TLB.c in six specific
#  ways, rebuilds, and checks that test_tlb reports a failure each time.
#
#  A mutation that SURVIVES (tests still pass) is a hole in the suite.
#
#  Run from the repository root:   sh tests/mutate_tlb.sh
#  The full report goes to results/mutate_tlb.txt; only the verdict is
#  printed to the terminal. Pass "-" as the first argument to print
#  everything to the terminal instead.
#
#  src/TLB.c is never modified -- everything happens in a scratch copy.
# ============================================================================

set -u

ROOT=$(pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

if [ ! -f "$ROOT/src/TLB.c" ] || [ ! -f "$ROOT/tests/test_tlb.c" ]; then
    echo "run this from the repository root (src/TLB.c not found)" >&2
    exit 2
fi

cp -r "$ROOT/include" "$WORK/" || exit 2
cp "$ROOT/tests/test_tlb.c" "$WORK/" || exit 2

REPORT="results/mutate_tlb.txt"
if [ "${1:-}" = "-" ]; then
    REPORT=/dev/stdout
else
    mkdir -p results
    : > "$REPORT"
fi

# Everything printed by this script goes to the report.
exec 3>&1              # keep the real terminal on fd 3
exec 1>>"$REPORT"

killed=0
survived=0

# apply <description> <search> <replace>
apply () {
    desc=$1; search=$2; replace=$3

    cp "$ROOT/src/TLB.c" "$WORK/TLB.c"

    SEARCH="$search" REPLACE="$replace" python3 - "$WORK/TLB.c" <<'PY'
import os, sys
path   = sys.argv[1]
search = os.environ["SEARCH"]
repl   = os.environ["REPLACE"]
src    = open(path).read()
if search not in src:
    sys.exit(3)                       # source drifted: pattern no longer present
open(path, "w").write(src.replace(search, repl, 1))
PY
    rc=$?
    if [ $rc -eq 3 ]; then
        echo "  SKIP    $desc"
        echo "          (pattern not found -- TLB.c changed, update this script)"
        return
    fi

    if ! gcc -w -I"$WORK/include" -o "$WORK/mut" "$WORK/test_tlb.c" "$WORK/TLB.c" \
         2>/dev/null; then
        echo "  KILLED  $desc"
        echo "          (did not compile -- still a detected mutation)"
        killed=$((killed + 1))
        return
    fi

    if "$WORK/mut" >"$WORK/out.txt" 2>&1; then
        echo "  SURVIVED  $desc"
        echo "            *** the tests did NOT notice this bug ***"
        survived=$((survived + 1))
    else
        echo "  KILLED  $desc"
        grep -E '^[[:space:]]+FAIL' "$WORK/out.txt" | head -2 \
            | sed 's/^[[:space:]]*/            /'
        killed=$((killed + 1))
    fi
}

echo "================================================================"
echo "  Mutation testing src/TLB.c against tests/test_tlb.c"
echo "  KILLED = the bug was caught.  SURVIVED = a hole in the suite."
echo "================================================================"
echo

# --- 1. the identifier half of the comparator -------------------------------
apply "M1  comparator ignores the PID (plain TLB, needs a flush per switch)" \
"t->entries[i].pid == (pid & (uint32_t)MASK(PID_BITS)) &&" \
"1 &&"

# --- 2. replacement is no longer LRU ----------------------------------------
apply "M2  select_victim always returns entry 0" \
"for (i = 0; i < TLB_ENTRIES; i++)
        if (!t->entries[i].valid)
            return (int)i;                 /* a free entry costs nothing */" \
"return 0;"

# --- 3. recency is never updated on a hit -----------------------------------
apply "M3  touch() does not age the other entries" \
"if (j != i && t->entries[j].valid && t->entries[j].lru < rank)
            t->entries[j].lru++;" \
"if (0) t->entries[j].lru++;"

# --- 4. process termination leaves stale entries behind ---------------------
apply "M4  invalidate_pid matches nothing (dead PIDs keep their mappings)" \
"t->entries[i].pid == (pid & (uint32_t)MASK(PID_BITS)))
            tlb_impl_forget(t, i);" \
"t->entries[i].pid == 0xFFFFFFFFu)
            tlb_impl_forget(t, i);"

# --- 5. a remapped page gets two entries ------------------------------------
apply "M5  insert skips the duplicate check (stale entry can shadow the new one)" \
"int i = tlb_impl_probe(t, pid, vpn);

    /* Already cached" \
"int i = -1;

    /* Already cached"

# --- 6. the rank permutation is left with a hole ----------------------------
apply "M6  forget() skips the rank repair (ranks stay ordered but not a permutation)" \
"for (j = 0; j < TLB_ENTRIES; j++)
        if (t->entries[j].valid && t->entries[j].lru > rank)
            t->entries[j].lru--;" \
"(void)rank;"

# --- 7. a reused frame keeps its stale translation --------------------------
apply "M7  invalidate_frame matches nothing (reused frame serves the old page)" \
"t->entries[i].pfn == (pfn & (uint32_t)MASK(FRAME_BITS)))
            tlb_impl_forget(t, i);" \
"t->entries[i].pfn == 0xFFFFFFFFu)
            tlb_impl_forget(t, i);"

echo
echo "================================================================"
echo "  killed $killed, survived $survived"
if [ "$survived" -eq 0 ]; then
    echo "  every injected bug was caught by the suite"
else
    echo "  *** $survived mutation(s) went unnoticed -- add tests for them ***"
fi
echo "================================================================"

# One-line verdict on the real terminal.
exec 1>&3 3>&-
printf '%-22s %3d killed   %d survived   %s%s\n' \
    "mutate_tlb" "$killed" "$survived" \
    "$([ "$survived" -eq 0 ] || echo '*** HOLES IN SUITE ***  ')" \
    "$REPORT"

[ "$survived" -eq 0 ]
