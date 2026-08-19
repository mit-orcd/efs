#!/bin/bash
# v6 -> v7 wire-format migration test on a throwaway local cluster.
# Phase A: build a v6 export (base binaries), create files with fdatasync so
#          v6 metadata pages land on disk.
# Phase B: restart the SAME storage with v7 binaries; servers must rebuild from
#          v6 pages, the first client flush must migrate the blob to v7, and all
#          pre- and post-migration files must read back intact.
set -u
V6="${EFS_V6_BIN:-/tmp/efs-base}"
V7="${EFS_V7_BIN:-/tmp/efs}"
D=/tmp/efs-migrate
NA=300   # files created under v6
NB=300   # files created under v7 (post-migration)

pkill -x efs-fuse 2>/dev/null; pkill -x efsd 2>/dev/null; sleep 1
fusermount3 -u $D/mnt 2>/dev/null
rm -rf $D; mkdir -p $D/s1 $D/s2 $D/s3 $D/mnt

start_cluster() {  # $1 = bin dir
  local B=$1
  $B/efsd --node-id 1 --addr 127.0.0.1 --port 4101 --storage $D/s1 >$D/s1.log 2>&1 & P1=$!
  $B/efsd --node-id 2 --addr 127.0.0.1 --port 4102 --storage $D/s2 --join 127.0.0.1:4101 >$D/s2.log 2>&1 & P2=$!
  $B/efsd --node-id 3 --addr 127.0.0.1 --port 4103 --storage $D/s3 --join 127.0.0.1:4101 >$D/s3.log 2>&1 & P3=$!
  sleep 5
}
stop_cluster() { kill $P1 $P2 $P3 2>/dev/null; wait $P1 $P2 $P3 2>/dev/null; sleep 1; }
mount_efs() {    # $1 = bin dir
  $1/efs-fuse 127.0.0.1:4101 primary $D/mnt >$D/fuse.log 2>&1 & FP=$!
  sleep 5
  mountpoint -q $D/mnt || { echo MOUNT_FAIL; tail -8 $D/fuse.log; exit 1; }
}
unmount_efs() { kill $FP 2>/dev/null; wait $FP 2>/dev/null; fusermount3 -u $D/mnt 2>/dev/null; sleep 1; }

export EFS_SKIP_META_FLUSH=0

echo "== Phase A: v6 cluster, create $NA files =="
start_cluster "$V6"
$V6/efs-mgmt mkfs 127.0.0.1:4101 primary >$D/mkfs.log 2>&1 || { echo MKFS_FAIL; cat $D/mkfs.log; exit 1; }
sleep 2
mount_efs "$V6"
t0=$SECONDS
for i in $(seq 1 $NA); do
  printf 'v6-file-%d\n' $i | timeout 15 dd of=$D/mnt/a$i bs=4k conv=fdatasync status=none || echo "WFAIL a$i"
done
echo "Phase A done files=$(ls $D/mnt|wc -l) dt=$((SECONDS-t0))s"
unmount_efs
stop_cluster
echo "v6 pages on disk: $(find $D/s1 $D/s2 $D/s3 -type f | wc -l) files"

echo "== Phase B: restart with v7 binaries =="
start_cluster "$V7"
sleep 3
mount_efs "$V7"
echo "post-rebuild files=$(ls $D/mnt|wc -l) (expect $NA)"
# verify pre-migration data
bad=0
for i in 1 $((NA/2)) $NA; do
  c=$(cat $D/mnt/a$i 2>/dev/null)
  [ "$c" = "v6-file-$i" ] || { echo "READFAIL a$i got='$c'"; bad=1; }
done
[ $bad -eq 0 ] && echo "pre-migration reads OK"

echo "== Phase B: create $NB files post-migration =="
t0=$SECONDS
for i in $(seq 1 $NB); do
  printf 'v7-file-%d\n' $i | timeout 15 dd of=$D/mnt/b$i bs=4k conv=fdatasync status=none || echo "WFAIL b$i"
done
echo "Phase B creates done files=$(ls $D/mnt|wc -l) dt=$((SECONDS-t0))s"

echo "== verify all data =="
bad=0; n=0
for i in 1 $((NA/2)) $NA; do c=$(cat $D/mnt/a$i 2>/dev/null); [ "$c" = "v6-file-$i" ] || { echo "READFAIL a$i"; bad=1; }; n=$((n+1)); done
for i in 1 $((NB/2)) $NB; do c=$(cat $D/mnt/b$i 2>/dev/null); [ "$c" = "v7-file-$i" ] || { echo "READFAIL b$i"; bad=1; }; n=$((n+1)); done
echo "checked $n files, bad=$bad"
echo "total files=$(ls $D/mnt|wc -l) (expect $((NA+NB)))"

echo "== restart one v7 server, confirm v7 pages rebuild =="
kill $P2 2>/dev/null; wait $P2 2>/dev/null; sleep 2
$V7/efsd --node-id 2 --addr 127.0.0.1 --port 4102 --storage $D/s2 --join 127.0.0.1:4101 >>$D/s2.log 2>&1 & P2=$!
sleep 6
c=$(cat $D/mnt/b$NB 2>/dev/null); [ "$c" = "v7-file-$NB" ] && echo "post-v7-rebuild read OK" || echo "post-v7-rebuild READFAIL got='$c'"

echo "== markers =="
grep -iE "rebuild|resync|stale|rebased|version|v7|migrat" $D/s1.log $D/s2.log $D/s3.log $D/fuse.log 2>/dev/null | tail -20

[ $bad -eq 0 ] && echo "MIGRATE_OK" || echo "MIGRATE_DATA_FAIL"
unmount_efs
stop_cluster
pkill -x efs-fuse 2>/dev/null; pkill -x efsd 2>/dev/null; sleep 1
rm -rf $D
echo "MIGRATE_DONE"
