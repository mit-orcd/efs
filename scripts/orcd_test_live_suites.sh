#!/bin/bash
# orcd_test_live_suites.sh OUTDIR [MOUNT] [PEER]
#
# Mounted correctness harnesses for the orcd-test cluster (4 storage nodes
# fcstor003-006 at 172.16.223.57-60:17432), run on one client host with two
# efs-fuse mounts of the same export (default /tmp/efs-mount and
# /tmp/efs-mount2; the second is made with scripts/client.sh first). Both
# mounts need EFS_COMMANDS=1 and servers EFS_COMMANDS_SERVER=1.
# Run from the deployed source tree (cd /tmp/efs). Each harness writes its
# own fixture under MOUNT; OUTDIR gets one log per harness and summary.tsv
# (name, rc, seconds). Never run against a production namespace.
set -u
OUT=$1; MNT=${2:-/tmp/efs-mount}; PEER=${3:-/tmp/efs-mount2}
mkdir -p "$OUT"
export EFS_QUERY_TEST_PORT=17432 EFS_QUERY_TEST_HOST=172.16.223.57
export EFS_QUERY_TEST_NODES='{"1":["172.16.223.57",17432],"2":["172.16.223.58",17432],"3":["172.16.223.59",17432],"4":["172.16.223.60",17432]}'
export EFS_QUERY_TEST_FIXTURE=command-load-$(date -u +%Y%m%dT%H%M%S)
NODES="--node-address 172.16.223.57:17432 --node-address 172.16.223.58:17432 --node-address 172.16.223.59:17432 --node-address 172.16.223.60:17432"
WORK=$MNT/live-pages-$(date -u +%Y%m%dT%H%M%S); mkdir -p "$WORK"
printf 'name\trc\tseconds\n' > "$OUT/summary.tsv"
run() { # name timeout command...
    local name=$1 limit=$2; shift 2
    local t0=$(date +%s)
    echo "=== $name $(date -u +%T)"
    timeout "$limit" "$@" > "$OUT/$name.log" 2>&1; local rc=$?
    printf '%s\t%s\t%s\n' "$name" "$rc" "$(( $(date +%s) - t0 ))" >> "$OUT/summary.tsv"
    echo "$name rc=$rc"; tail -2 "$OUT/$name.log" | cut -c1-300
}
run posix_2client      1800 python3 -u tests/posix/posix_2client.py --local "$MNT" "$PEER" --results "$OUT/posix_2client.tsv"
run efs_commands       1800 python3 -u tests/live/efs_commands.py "$MNT" "$PEER"
run efs_command_options 1800 python3 -u tests/live/efs_command_options.py "$MNT" "$PEER"
run efs_command_pages  1800 python3 -u tests/live/efs_command_pages.py "$MNT" "$PEER" "$WORK"
run virtual_interfaces 3000 python3 -u tests/live/virtual_interfaces.py "$MNT" "$PEER" --freshness-s 150
run query_page_close   1200 python3 -u tests/live/query_page_close.py "$MNT"
run query_large_delta  2400 python3 -u tests/live/query_large_delta.py "$MNT" "$PEER"
run efs_commands_load  2400 python3 -u tests/live/efs_commands_load.py "$MNT" "$PEER"
run chmod_truncate     1800 python3 -u tests/live/chmod_truncate.py "$MNT"
run concurrent_extend_eof 1800 python3 -u tests/live/concurrent_extend_eof.py "$MNT"
run exclusive_create_visibility 1800 python3 -u tests/live/exclusive_create_visibility.py "$MNT" --spawn
# Stock fork() writers: on RHEL 9 kernels without upstream 738adade96b2 this
# fails by W121 (stale application bytes after a correct reply); informational.
run w121_exclusive_fork 1800 python3 -u tests/live/exclusive_create_visibility.py "$MNT"
run fsync_admission    1800 python3 -u tests/live/fsync_admission.py "$MNT"
# shellcheck disable=SC2086
run mtime_barrier      1800 python3 -u tests/live/mtime_barrier.py "$MNT" ./efs-mgmt 172.16.223.57:17432 $NODES
awk -F'\t' 'NR>1 && $2!=0 && $1!~/^w121_/ {bad++} END {print (bad ? "FAIL " bad : "PASS all"), "harnesses"}' "$OUT/summary.tsv"
