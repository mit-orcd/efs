#!/bin/sh
# Load reproducer for K86: PAR concurrent copies of tests/test_bench_published.py
# per round, ROUNDS rounds, each copy with its own TMPDIR. Run from a built tree.
# Usage: scripts/evidence/measurement/bench_published_par.sh OUTDIR [PAR=24] [ROUNDS=10]
# Output: OUTDIR/r<round>-<copy>.log, OUTDIR/results.tsv (round copy rc secs),
# OUTDIR/summary.txt. Failing logs are kept; passing logs are deleted.
set -u
out=${1:?usage: $0 OUTDIR [PAR] [ROUNDS]}; par=${2:-24}; rounds=${3:-10}
mkdir -p "$out"; : > "$out/results.tsv"; base=${TMPDIR:-/tmp}
r=1
while [ "$r" -le "$rounds" ]; do
    i=1
    while [ "$i" -le "$par" ]; do
        (
            t=$base/bp-$r-$i; mkdir -p "$t"; s=$(date +%s)
            TMPDIR=$t python3 tests/test_bench_published.py >"$out/r$r-$i.log" 2>&1; rc=$?
            printf '%s\t%s\t%s\t%s\n' "$r" "$i" "$rc" "$(( $(date +%s) - s ))" >>"$out/results.tsv"
            [ "$rc" -eq 0 ] && rm -f "$out/r$r-$i.log"; rm -rf "$t"
        ) &
        i=$((i + 1))
    done
    wait; r=$((r + 1))
done
{
    echo "runs $(wc -l <"$out/results.tsv") fail $(awk -F'\t' '$3!=0' "$out/results.tsv" | wc -l)"
    echo "rc=-28 logs $(grep -l 'rc=-28' "$out"/r*.log 2>/dev/null | wc -l)"
    # rc=-4 (EXIST) is also the expected line of the test's name-collision control, so it counts nothing.
    echo "report-stuck logs $(grep -l 'report-stuck' "$out"/r*.log 2>/dev/null | wc -l)"
    echo "stage=3 rc=-13 logs $(grep -l 'rc=-13.*stage=3' "$out"/r*.log 2>/dev/null | wc -l)"
} >"$out/summary.txt"
cat "$out/summary.txt"
