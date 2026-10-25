#!/bin/bash
# flake_repro.sh TREE OUT N: the flake sweep's reproduction driver (Oct 25,
# docs/status/flake-sweep-20261025.md): each intermittent daemon test N
# times alone, keeping the daemon and client logs of every failure, then all
# of them looped three times beside one 8-job pool run of the gate. TREE is a
# built copy of the source tree on the host; OUT collects loop.log files.
TREE=$1; OUT=$2; N=$3; L=$(dirname "$0")/flake_loop.sh
mkdir -p "$OUT"; echo "repro start $(date -Is) $(cat $TREE/.source-commit)" >> "$OUT/repro.log"
for t in k37_segment_rebuild k36_recovery d4_progress report_stepdown; do
    bash $L "$TREE" "$OUT" alone-$t $N python3 tests/test_$t.py
    tail -1 "$OUT/alone-$t/loop.log" >> "$OUT/repro.log"
done
( cd "$TREE" && EFS_TEST_JOBS=8 EFS_TEST_TARGET=test-all bash scripts/make_test_summary.sh > "$OUT/pool-beside.out" 2>&1;
  mkdir -p "$OUT/pool-beside"; cp summary.txt timings.tsv "$OUT/pool-beside/"; cp tmp/fail-*.log "$OUT/pool-beside/" 2>/dev/null ) &
for t in k35_fsync_degraded k36_recovery k37_segment_rebuild d4_progress report_stepdown; do
    bash $L "$TREE" "$OUT" pool-$t 3 python3 tests/test_$t.py &
done
wait
for t in k35_fsync_degraded k36_recovery k37_segment_rebuild d4_progress report_stepdown; do
    tail -1 "$OUT/pool-$t/loop.log" >> "$OUT/repro.log"
done
head -1 "$OUT/pool-beside/summary.txt" >> "$OUT/repro.log"
echo "repro done $(date -Is)" >> "$OUT/repro.log"
