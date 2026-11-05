#!/bin/sh
# Task DJ: run tests/test_put_quota_reclaim.py COPIES at a time for ROUNDS
# rounds from a built tree (make test-bins tests/faults/efsd) and count the
# copies that fail. Usage: status_protection_load.sh TREE OUT [ROUNDS] [COPIES]
# Each copy's stdout/stderr goes to OUT/rR-cC.log; OUT/summary.tsv has one
# row per copy (round, copy, rc, seconds, protection line on failure).
set -u
tree=$1 out=$2 rounds=${3:-5} copies=${4:-8}
mkdir -p "$out"
printf 'round\tcopy\trc\tseconds\tprotection\n' >"$out/summary.tsv"
r=1
while [ "$r" -le "$rounds" ]; do
    c=1
    while [ "$c" -le "$copies" ]; do
        (
            t0=$(date +%s.%N)
            cd "$tree" && python3 tests/test_put_quota_reclaim.py >"$out/r$r-c$c.log" 2>&1
            rc=$?
            t1=$(date +%s.%N)
            prot=$(grep -m1 -o 'Protection: [^\\]*' "$out/r$r-c$c.log" | head -c 160)
            [ "$rc" -eq 0 ] && prot=
            printf '%s\t%s\t%s\t%.1f\t%s\n' "$r" "$c" "$rc" "$(echo "$t1 - $t0" | bc)" "$prot" >>"$out/summary.tsv"
        ) &
        c=$((c + 1))
    done
    wait
    r=$((r + 1))
done
awk -F'\t' 'NR>1{n++; if($3!=0)f++} END{printf "copies=%d failed=%d\n", n, f}' "$out/summary.tsv" | tee "$out/verdict.txt"
