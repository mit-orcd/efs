#!/bin/bash
# measure_put_durability.sh MOUNT FIXTURE READ_FILE NODES OUT_DIR [RUNS]
#
# 2 GiB write and read throughput on one client, for asynchronous PUT
# acknowledgement (docs/status/intel-async-durability-20261015.md).
#
#   MOUNT      EFS mount, e.g. /data1/efs/intel-mnt
#   FIXTURE    local file of at least 2 GiB with nonzero data (page cached)
#   READ_FILE  existing file of at least 6 GiB on the mount (read-only)
#   NODES      comma-separated node:port list for efs-mgmt sync-status
#   OUT_DIR    new local directory for results
#
# Per run (default 3): dd 2 GiB bs=1M without fsync, dd 2 GiB with
# conv=fsync (includes the COMMIT), then a 2 GiB read at offsets 0/2/4 GiB of
# READ_FILE. Each written file is verified by SHA-256 against the fixture's
# first 2 GiB, then removed. sync-status is captured right after the first
# unsynced write and again 2 s later. Prints one line per measurement.
set -euo pipefail
[ $# -ge 5 ] || { sed -n '2,19p' "$0" >&2; exit 2; }
mnt=$1 fix=$2 rfile=$3 nodes=$4 out=$5 runs=${6:-3}
mgmt=$(dirname "$0")/../efs-mgmt
mkdir "$out"
dir="$mnt/put-durability-$(date +%s)"
mkdir "$dir"
want=$(head -c $((2 << 30)) "$fix" | sha256sum | cut -c1-64)
cat "$fix" > /dev/null
IFS=, read -r -a node_list <<< "$nodes"

timed() { # label command...
    local t1 t2
    t1=$(date +%s.%N); "${@:2}"; t2=$(date +%s.%N)
    python3 -c "import sys; print('%-14s %7.1f MiB/s %6.2f s' % (sys.argv[1], 2048/(float(sys.argv[3])-float(sys.argv[2])), float(sys.argv[3])-float(sys.argv[2])))" "$1" "$t1" "$t2"
}
verify() {
    local got
    got=$(dd if="$1" bs=1M status=none | sha256sum | cut -c1-64)
    [ "$got" = "$want" ] && echo "verify $1 ok" || { echo "verify $1 MISMATCH" >&2; exit 1; }
    rm -- "$1"
}

for i in $(seq 1 "$runs"); do
    timed "write" dd if="$fix" of="$dir/w$i.bin" bs=1M count=2048 status=none
    if [ "$i" = 1 ] && [ -x "$mgmt" ]; then
        "$mgmt" sync-status "${node_list[@]}" > "$out/sync-status-after-write.txt" 2>&1 || true
        sleep 2
        "$mgmt" sync-status "${node_list[@]}" > "$out/sync-status-plus-2s.txt" 2>&1 || true
    fi
    timed "write+fsync" dd if="$fix" of="$dir/f$i.bin" bs=1M count=2048 conv=fsync status=none
    off=$(( ((i - 1) % 3) * 2048 ))
    timed "read" dd if="$rfile" of=/dev/null bs=1M skip="$off" count=2048 status=none
    verify "$dir/w$i.bin"
    verify "$dir/f$i.bin"
done
rmdir "$dir"
