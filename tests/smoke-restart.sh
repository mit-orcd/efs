#!/bin/bash
# Self-contained 3-server + FUSE smoke test on one host (node-local storage).
# Exercises: sequential fsync flush, concurrent-writer election, and a server
# restart (rebuild staging). Throwaway cluster on 127.0.0.1:4101-4103.
set -u
B="${EFS_BIN:-/tmp/efs-build}"
D=/tmp/efs-smoke
pkill -x efs-fuse 2>/dev/null; pkill -x efsd 2>/dev/null; sleep 1
fusermount3 -u $D/mnt 2>/dev/null
rm -rf $D; mkdir -p $D/s1 $D/s2 $D/s3 $D/mnt

$B/efsd --node-id 1 --addr 127.0.0.1 --port 4101 --storage $D/s1 >$D/s1.log 2>&1 & P1=$!
$B/efsd --node-id 2 --addr 127.0.0.1 --port 4102 --storage $D/s2 --join 127.0.0.1:4101 >$D/s2.log 2>&1 & P2=$!
$B/efsd --node-id 3 --addr 127.0.0.1 --port 4103 --storage $D/s3 --join 127.0.0.1:4101 >$D/s3.log 2>&1 & P3=$!
sleep 5

$B/efs-mgmt mkfs 127.0.0.1:4101 primary >$D/mkfs.log 2>&1 || { echo MKFS_FAIL; cat $D/mkfs.log; exit 1; }
sleep 2

$B/efs-fuse 127.0.0.1:4101 primary $D/mnt >$D/fuse.log 2>&1 & FP=$!
sleep 5
if ! mountpoint -q $D/mnt; then echo MOUNT_FAIL; tail -8 $D/fuse.log; exit 1; fi
echo "MOUNTED"

export EFS_SKIP_META_FLUSH=0
echo "== P1: 60 seq create+fdatasync =="
t0=$SECONDS; f=0
for i in $(seq 1 60); do
  timeout 15 dd if=/dev/zero of=$D/mnt/f$i bs=4k count=1 conv=fdatasync status=none || { echo "WFAIL f$i"; f=1; }
done
echo "P1 done files=$(ls $D/mnt|wc -l) dt=$((SECONDS-t0))s fail=$f"

echo "== P2: 3 concurrent writers x25 (election contention) =="
t0=$SECONDS
wpids=""
for w in 1 2 3; do
  ( for i in $(seq 1 25); do timeout 15 dd if=/dev/zero of=$D/mnt/w$w-$i bs=4k count=1 conv=fdatasync status=none; done ) &
  wpids="$wpids $!"
done
wait $wpids
echo "P2 done files=$(ls $D/mnt|wc -l) dt=$((SECONDS-t0))s"

echo "== P3: restart server2 (rebuild staging) =="
kill $P2 2>/dev/null; sleep 2
$B/efsd --node-id 2 --addr 127.0.0.1 --port 4102 --storage $D/s2 --join 127.0.0.1:4101 >>$D/s2.log 2>&1 & P2=$!
sleep 6
t0=$SECONDS
for i in $(seq 1 20); do timeout 15 dd if=/dev/zero of=$D/mnt/r$i bs=4k count=1 conv=fdatasync status=none; done
echo "P3 done files=$(ls $D/mnt|wc -l) dt=$((SECONDS-t0))s"

echo "== verify =="
echo "total files=$(ls $D/mnt|wc -l) (expect 155)"
head -c4 $D/mnt/f1 >/dev/null 2>&1 && echo "read f1 ok" || echo "read f1 FAIL"
head -c4 $D/mnt/w2-7 >/dev/null 2>&1 && echo "read w2-7 ok" || echo "read w2-7 FAIL"
df $D/mnt 2>/dev/null | tail -1

echo "== markers (rebuild/resync/stale/yield/fence/rebased) =="
grep -iE "rebuild|resync|stale|yield|fence|rebased" $D/s1.log $D/s2.log $D/s3.log $D/fuse.log 2>/dev/null | tail -25

kill $FP $P1 $P2 $P3 2>/dev/null; sleep 1
fusermount3 -u $D/mnt 2>/dev/null
pkill -x efs-fuse 2>/dev/null; pkill -x efsd 2>/dev/null; sleep 1
rm -rf $D
echo "SMOKE_DONE"
