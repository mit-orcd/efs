#!/bin/bash
# gate_load_repro.sh MODE RUNS OUTDIR [TREE]
# Task BM: repeat the 8-job gate (or its slow half) and keep one directory per
# run with summary.txt, timings.tsv and the per-failure logs, so a test that
# fails only under load gets a rate, not an anecdote.
#   MODE  loaded  EFS_TEST_JOBS=${BM_JOBS:-8} EFS_TEST_TARGET=test-long (the live-cluster
#                 phase of the gate, where the four BM tests fail)
#         full    EFS_TEST_JOBS=8, EFS_TEST_TARGET=test-all
#         solo    the four BM tests one after another on an idle host
# TREE is a built source tree (default: the tree this script is in). Raw output
# stays in OUTDIR (outside Git); each run's TMPDIR is deleted when it ends.
set -u
mode=${1:?mode}; runs=${2:?runs}; out=${3:?outdir}
tree=${4:-$(cd "$(dirname "$0")/.." && pwd)}
mkdir -p "$out"; cd "$tree" || exit 2
TESTS="test_k54_member_down_fence test_k38_shared_chunk_pressure test_d4_progress test_k37_segment_rebuild"
for n in $(seq 1 "$runs"); do
    d=$out/$mode-$n; mkdir -p "$d"
    export TMPDIR=${BM_TMPBASE:-$HOME/efs-bm-tmp}/$mode-$n; mkdir -p "$TMPDIR"
    if [ "$mode" = solo ]; then
        for t in $TESTS; do
            s=$(date +%s)
            python3 tests/$t.py > "$d/$t.log" 2>&1; rc=$?
            echo "$t rc=$rc $(( $(date +%s) - s ))s" >> "$d/solo.txt"
        done
    else
        [ "$mode" = full ] && target=test-all || target=test-long
        EFS_TEST_JOBS=${BM_JOBS:-8} EFS_TEST_TARGET=$target sh scripts/make_test_summary.sh > "$d/run.out" 2>&1
        cp summary.txt timings.tsv "$d"/ 2>/dev/null
        cp tmp/fail-*.log "$d"/ 2>/dev/null
        cp -r "$TMPDIR"/case-* "$d/" 2>/dev/null && true
    fi
    rm -rf "$TMPDIR"
    echo "run $mode-$n done $(date +%H:%M:%S)" >> "$out/progress.txt"
done
