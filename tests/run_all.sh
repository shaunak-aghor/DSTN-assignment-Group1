#!/bin/sh
# ============================================================================
#  tests/run_all.sh -- build and run every suite, write reports to results/
#
#  Run from the repository root:   sh tests/run_all.sh
#
#  Each suite writes its full report to results/<name>.txt and prints one
#  verdict line here. Exit status is 0 only if every suite passed.
# ============================================================================

set -u

CC=${CC:-gcc}
CFLAGS="-Wall -Wextra -g -I./include"

if [ ! -d src ] || [ ! -d tests ]; then
    echo "run this from the repository root" >&2
    exit 2
fi

mkdir -p results
rc=0

build_run () {
    name=$1; shift
    if ! $CC $CFLAGS -o "$name" "$@" 2>"results/$name.build.log"; then
        printf '%-22s BUILD FAILED -- see results/%s.build.log\n' "$name" "$name"
        rc=1
        return
    fi
    if [ -s "results/$name.build.log" ]; then
        printf '%-22s compiled WITH WARNINGS -- results/%s.build.log\n' \
               "$name" "$name"
        rc=1
    else
        rm -f "results/$name.build.log"
    fi
    "./$name" || rc=1
}

echo "Building and running all suites ..."
echo

build_run test_tlb          tests/test_tlb.c src/TLB.c
build_run test_tlb_full     tests/test_tlb_full.c src/TLB.c
build_run test_tlb_workflow tests/test_tlb_workflow.c src/MainMemory.c \
                            src/translate.c src/TLB.c
build_run test_mm           tests/test_mm.c src/MainMemory.c src/translate.c \
                            src/TLB.c

sh tests/mutate_tlb.sh || rc=1

echo
if [ "$rc" -eq 0 ]; then
    echo "ALL SUITES PASSED.  Reports in results/"
else
    echo "*** SOMETHING FAILED -- read the report files in results/ ***"
fi
ls -1 results/ | sed 's/^/    results\//'

exit "$rc"
