#!/bin/bash
# k32_load_check.sh TREE [K32_RUNS] [OWNER_RUNS]: run the K32 entry-count
# test K32_RUNS times (default 48) and test_put_ticket_owner OWNER_RUNS
# times (default 16) under scripts/make_test_summary.sh at EFS_TEST_JOBS=8,
# the parallel load that made test_k32_entry_count fail (65+64 admissions,
# W155 evidence). TREE must be a scratch copy of the source (git archive):
# this script appends a k32-load-check target to TREE/Makefile. Results:
# TREE/summary.txt and TREE/tmp/fail-*.log; exit status is the runner's.
set -eu
tree=$1; k=${2:-48}; o=${3:-16}
[ -f "$tree/Makefile" ] && [ ! -d "$tree/.git" ] || { echo "k32_load_check: TREE must be a scratch copy without .git" >&2; exit 2; }
grep -q '^k32-load-check:' "$tree/Makefile" || {
    printf '\nk32-load-check: all\n'
    for i in $(seq 1 "$k"); do printf '\tpython3 tests/test_k32_entry_count.py\n'; done
    for i in $(seq 1 "$o"); do printf '\tpython3 tests/test_put_ticket_owner.py\n'; done
} >> "$tree/Makefile"
cd "$tree"
EFS_TEST_JOBS=8 EFS_TEST_TARGET=k32-load-check scripts/make_test_summary.sh
