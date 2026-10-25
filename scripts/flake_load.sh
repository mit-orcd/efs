#!/bin/bash
# flake_load.sh TREE OUT LABEL TEST LOOPS ITERATIONS: the flake sweep's loaded
# reproduction (docs/status/flake-sweep-20261025.md). One 8-job pool run of
# the gate (scripts/make_test_summary.sh, test-all) as load, and beside it
# LOOPS parallel loops of tests/TEST, ITERATIONS each, through
# scripts/flake_loop.sh (failing iterations keep their logs under
# OUT/LABEL-N/fail-I/). The pool run's summary goes to OUT/LABEL-pool-summary.txt.
# Example (the step-down stall, 5 of 28 failing before the fix):
#   bash scripts/flake_load.sh ~/efs-flake2 ~/efs-artifacts/x/repro rsd test_report_stepdown.py 4 10
TREE=$1; OUT=$2; LABEL=$3; TEST=$4; LOOPS=${5:-4}; N=${6:-10}
[ -n "$TEST" ] || { echo "usage: $0 TREE OUT LABEL TEST [LOOPS] [ITERATIONS]" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"; cd "$TREE" || exit 2
( EFS_TEST_JOBS=8 EFS_TEST_TARGET=test-all bash scripts/make_test_summary.sh > "$OUT/$LABEL-pool.out" 2>&1
  cp summary.txt "$OUT/$LABEL-pool-summary.txt" ) &
for k in $(seq 1 "$LOOPS"); do
    bash "$HERE/flake_loop.sh" "$TREE" "$OUT" "$LABEL-$k" "$N" python3 "tests/$TEST" &
done
wait
echo "$LABEL done $(date -Is)" >> "$OUT/repro.log"
