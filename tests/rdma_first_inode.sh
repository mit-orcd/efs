#!/bin/bash
# Private 3-node localhost raft + auto/RDMA first mkdir. Does NOT touch
# 19820/19810. Kill only by these ports / this fuse argv.
set -euo pipefail
ADDR=${ADDR:-172.16.223.61}
PORT=${PORT:-19950}
WORK=/tmp/efs-rdma-first
MNT=/tmp/efs-rdma-first-mnt
HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE"

kill_ours() {
    ps -eo pid,args | awk '/[e]fsd / && / --port 1995[012]( |$)/ {print $1}' | xargs -r kill -9
    ps -eo pid,args | awk '/[e]fs-fuse / && /19950/ {print $1}' | xargs -r kill -9
    timeout 3 fusermount3 -uz "$MNT" 2>/dev/null || true
}

trap kill_ours EXIT
rm -rf "$WORK"
mkdir -p "$WORK/store" "$MNT"
kill_ours
# D39: the configured member list; efs-mgmt declares as management with it.
for id in 1 2 3; do echo "$id $ADDR:$((PORT + id - 1))"; done > "$WORK/cluster.conf"
export EFS_CLUSTER_CONF="$WORK/cluster.conf"

start_one() {
    local id=$1
    local port=$((PORT + id - 1))
    EFS_TRANSPORT=auto setsid ./efsd --cluster "$WORK/cluster.conf" \
        --addr "$ADDR" --port "$port" --storage "$WORK/store/s$id" \
        --quota 1G --writers 0 --no-direct-io \
        >"$WORK/s$id.log" 2>&1 </dev/null &
    local i
    for i in $(seq 1 40); do
        grep -q listening "$WORK/s$id.log" 2>/dev/null && return 0
        sleep 0.1
    done
    echo "START_FAIL node $id"
    tail -20 "$WORK/s$id.log" || true
    kill_ours
    exit 2
}

start_one 1
start_one 2
start_one 3

sleep 0.4
mk=1
for _ in $(seq 1 15); do
    for off in 0 1 2; do
        if ./efs-mgmt raft-mkfs "$ADDR:$((PORT + off))"; then
            mk=0
            break 2
        fi
    done
    sleep 0.3
done
echo MKFS_RC=$mk
if [ "$mk" != 0 ]; then
    echo MKFS_FAIL
    tail -20 "$WORK"/s*.log || true
    exit 2
fi

rm -f "$WORK/fuse.log"
EFS_TRANSPORT=rdma EFS_RDMA_FIRST=1 setsid ./efs-fuse "$ADDR:$PORT" \
    "$MNT" >"$WORK/fuse.log" 2>&1 </dev/null &

served=0
for _ in $(seq 1 50); do
    if grep -q "fuse serving" "$WORK/fuse.log" 2>/dev/null; then
        served=1
        break
    fi
    sleep 0.1
done
echo SERVED=$served FSTYPE=$(findmnt -n -o FSTYPE "$MNT" 2>/dev/null || echo none)
if [ "$served" != 1 ]; then
    echo MOUNT_FAIL
    cat "$WORK/fuse.log"
    kill_ours
    exit 2
fi

set +e
timeout 8 mkdir "$MNT/first"
MRC=$?
timeout 8 sh -c "echo hi > \"$MNT/first/f\" && cat \"$MNT/first/f\""
WRC=$?
set -e
echo MKDIR_RC=$MRC WRITE_RC=$WRC
echo --- fuse.log ---
grep -E "RDMA transport|WAIT TIMEOUT|upgrade ok|tcp EOF|teardown|SOCKET CLOSED|meta ready" \
    "$WORK/fuse.log" | head -40
ls -la "$MNT/first" 2>/dev/null || true

kill_ours
rmdir "$MNT" 2>/dev/null || true
rm -rf "$WORK"
echo CLEAN_DONE
exit $( [ "$MRC" = 0 ] && [ "$WRC" = 0 ] && echo 0 || echo 1 )
