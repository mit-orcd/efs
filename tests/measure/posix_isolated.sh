#!/bin/bash
# posix_isolated.sh — W7: run the slow POSIX suite-1 tests one at a time on
# an idle cluster and time each. The suite's per-test budget is 15 s
# (POSIX_TEST_SEC); the question is whether a test is over budget because
# of per-op metadata latency (mkdir/create/close round trips at N ms x
# count) or something else. Since the Sep 20 wakeup fix (mkdir med 7 ms)
# they should all pass; if one is back over budget, the cluster was not
# idle or a wakeup regressed (project-state rule) — check preflight's
# commit-rate line first.
#
# Usage (from node9901 via efs-bg.sh, ~2-4 min):
#   bash tests/measure/posix_isolated.sh
#   TESTS="concurrent_creates_same_dir mtime_monotonic_many_writes" HOST=fcstor008 bash tests/measure/posix_isolated.sh
#
# The client runs the suite from its node-local /tmp/efs copy (same tree as
# the mounted binary; no build). Hand the user: the per-test seconds and
# per-op ms (secs / ops), against the 15 s budget. Never raise the budget.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); . "$HERE/lib.sh"
HOST="${HOST:-fcstor007}"
TESTS="${TESTS:-concurrent_creates_same_dir mtime_monotonic_many_writes dir_deep_nesting dir_deep_nesting_beyond_64 names_crazy_dirs dir_many_files}"
# ops per test, for a per-op figure (from the test bodies)
ops_of() { case "$1" in
    concurrent_creates_same_dir) echo 160;; mtime_monotonic_many_writes) echo 80;;
    dir_deep_nesting) echo 64;; dir_deep_nesting_beyond_64) echo 200;; *) echo 0;; esac; }
mkout posix-isolated
preflight_or_die

printf 'test\tresult\tsecs\tops\tms_per_op\tdetail\n' > "$OUT/table.tsv"
for t in $TESTS; do
    say "== $t on $HOST"
    ssh_ 120 "$HOST" "cd /tmp/efs && rm -f /tmp/pi-$t.tsv; TIMEFORMAT='WALL %R'; { time POSIX_JOBS=1 python3 tests/posix/posix_suite.py $MNT --filter $t --results /tmp/pi-$t.tsv --tag pi >/tmp/pi-$t.out 2>&1; } 2>&1; grep -E '^(pass|FAIL|SKIP) ' /tmp/pi-$t.out; grep -v '^#' /tmp/pi-$t.tsv | grep -v '^test' " > "$OUT/$t.txt"
    cat "$OUT/$t.txt"
    w=$(grep -o 'WALL [0-9.]*' "$OUT/$t.txt" | cut -d' ' -f2)
    res=$(grep -E "^$t\b" "$OUT/$t.txt" | awk -F'\t' '{print $2}' | head -1)
    det=$(grep -E "^$t\b" "$OUT/$t.txt" | awk -F'\t' '{print $3}' | head -1 | cut -c1-80)
    ops=$(ops_of "$t"); mpo=$([ "$ops" -gt 0 ] && echo "scale=1; $w*1000/$ops" | bc || echo -)
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$t" "${res:-?}" "$w" "$ops" "$mpo" "$det" >> "$OUT/table.tsv"
done
{
    echo "posix suite-1 slow tests, isolated, build=$BUILD, $(date -u +%F), host=$HOST, budget 15 s each"
    echo "(secs includes ~1 s python start; --filter is a SUBSTRING match, so dir_deep_nesting's wall also includes dir_deep_nesting_beyond_64 — read the per-test lines in <test>.txt when that matters)"
    column -t -s $'\t' "$OUT/table.tsv"
} > "$OUT/SUMMARY.txt"
cat "$OUT/SUMMARY.txt"
