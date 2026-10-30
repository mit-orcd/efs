#!/bin/bash
# k48_attribution_loop.sh SRC OUTDIR RUNS: loop the instrumented W130 test
# (tests/test_w130_dir_fault.py, one cluster per run) from a built tree SRC.
# results.tsv gets run, rc, seconds, 1-min load, partial-listing flag. A
# failing run keeps its work dir and log as OUTDIR/case-N; passes keep
# nothing. Raw output stays under OUTDIR (outside Git). Stop early by
# touching OUTDIR/stop.
set -u
src=$1 out=$2 runs=$3
mkdir -p "$out" || exit 2
cd "$src" || exit 2
export EFS_TRANSPORT=tcp
for n in $(seq 1 "$runs"); do
    [ -e "$out/stop" ] && break
    tmp=$out/tmp-$n
    mkdir -p "$tmp"
    load=$(cut -d' ' -f1 /proc/loadavg)
    t0=$(date +%s)
    TMPDIR=$tmp python3 tests/test_w130_dir_fault.py > "$out/run-$n.log" 2>&1
    rc=$?
    secs=$(( $(date +%s) - t0 ))
    partial=$(grep -c 'partial listing\|missing \[' "$out/run-$n.log")
    printf '%s\t%s\t%s\t%s\t%s\n' "$n" "$rc" "$secs" "$load" "$partial" >> "$out/results.tsv"
    if [ "$rc" -eq 0 ]; then
        rm -f "$out/run-$n.log"
        ~/bin/edelete --force --delete "$tmp" > /dev/null 2>&1 || rm -rf "$tmp"
    else
        mkdir -p "$out/case-$n"
        mv "$out/run-$n.log" "$tmp" "$out/case-$n/"
    fi
done
