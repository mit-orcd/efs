#!/bin/bash
# Task FB: interleave two efs-bench builds on the 256-worker, 1 s io case and
# print verdict, idle workers and the latest worker start per run.
# Usage: io_start_compare.sh OLD_BENCH NEW_BENCH RUNS [CPU_LIST|all] [SCRATCH_DIR]
old=$1 new=$2 runs=${3:-10} cpus=${4:-all} scratch=${5:-${TMPDIR:-/tmp}}
[ -x "$old" ] && [ -x "$new" ] || { echo "usage: $0 OLD_BENCH NEW_BENCH RUNS [CPU_LIST|all] [SCRATCH_DIR]" >&2; exit 2; }
pre=(); [ "$cpus" = all ] || pre=(taskset -c "$cpus")
for k in $(seq 1 "$runs"); do for b in "$old" "$new"; do
    r=$(mktemp -d "$scratch/efs-io-start.XXXX")
    out=$("${pre[@]}" "$b" --bench io --storage "$r/s" --rw write --io-size 4K --qd 256 --window 1 --time 1 2>&1)
    rmdir "$r/s" "$r"
    tag=old; [ "$b" = "$new" ] && tag=new
    echo "$tag cpus=$cpus $(echo "$out" | grep -o '^BENCH_[A-Z]*\|idle_workers=[0-9]*\|max_start_us=[0-9]*' | tr '\n' ' ')load=$(cut -d' ' -f1 /proc/loadavg)"
done; done
