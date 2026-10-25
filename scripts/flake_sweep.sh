#!/bin/bash
# flake_sweep.sh TREE OUT LABEL NPOOL NSERIAL: the flake sweep's gate driver
# (docs/status/flake-sweep-20261025.md): scripts/make_test_summary.sh NPOOL
# times at EFS_TEST_JOBS=8 (test-all), then NSERIAL times at one job
# (test-long), keeping each run's summary.txt, timings.tsv, progress.txt,
# cmds.txt, build.log and tmp/fail-*.log under OUT/LABEL-{pool,serial}-NN.
# TREE is an unbuilt or built copy of the source tree (the runner builds).
TREE=$1; OUT=$2; LABEL=$3; NPOOL=$4; NSERIAL=$5
mkdir -p "$OUT"
cd "$TREE" || exit 2
echo "sweep start $(date -Is) commit $(cat .source-commit)" >> "$OUT/sweep.log"
run_one() { # name jobs target
    local d="$OUT/$1"; mkdir -p "$d"
    local t0=$(date +%s)
    EFS_TEST_JOBS=$2 EFS_TEST_TARGET=$3 bash scripts/make_test_summary.sh > "$d/runner.out" 2>&1
    local rc=$?
    cp summary.txt timings.tsv progress.txt cmds.txt "$d/" 2>/dev/null
    cp build.log "$d/build.log" 2>/dev/null
    for f in tmp/fail-*.log; do [ -e "$f" ] && cp "$f" "$d/"; done
    # keep kept artifacts of failed cases (small ones only)
    echo "$1 rc=$rc $(head -1 summary.txt) $(( $(date +%s) - t0 ))s" >> "$OUT/sweep.log"
    [ -d tmp ] && ~/bin/edelete --force --delete tmp >/dev/null
}
for i in $(seq 1 $NPOOL); do run_one $(printf "%s-pool-%02d" $LABEL $i) 8 test-all; done
for i in $(seq 1 $NSERIAL); do run_one $(printf "%s-serial-%02d" $LABEL $i) 1 test-long; done
echo "sweep done $(date -Is)" >> "$OUT/sweep.log"
