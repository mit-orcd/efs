#!/bin/bash
# w125_p3_compat.sh NEW_REL OLD_REL [RUN]
#
# W125 P3 checks on a private node9901 cluster (ports 17681-17683; never
# orcd-mini or w100): (1) new daemons + new client: 1,000 files of 1 B to
# 16 KiB written, packed (2 s minimum age) and read back byte for byte;
# (2) old client (OLD_REL efs-fuse) on new daemons: GETCHUNKS refused and
# logged, reads fail with EIO, writes still work; (3) new client on old
# daemons: the declaration is refused/unanswered and logged.
# Case 3 leaves operations hanging until their deadline; stop that mount
# with fusermount3 -uz if client.sh stop hangs.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
NEW=$1; OLD=$2; R=${3:-/tmp/w125-p3-run}
export LD_LIBRARY_PATH=/data1/efs/deps/libfuse/lib64 EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0
$NEW/scripts/w125_p0_cluster.sh stop $NEW $R 17680 >/dev/null 2>&1; rm -rf $R
echo "== 1. new daemons, new client"
EFS_PACK_MIN_AGE_MS=2000 $NEW/scripts/w125_p0_cluster.sh start $NEW $R 17680 | tail -1
python3 "$HERE/w125_p3_readback.py" write $R/mnt/v 1000; sleep 25; python3 "$HERE/w125_p3_readback.py" verify $R/mnt/v 1000
echo "== 2. old (base) client on new daemons"
$NEW/scripts/client.sh stop $R/mnt >/dev/null 2>&1
mkdir -p $R/mnt2; EFS_FUSE_LOG=$R/logs/fuse-old.log $OLD/scripts/client.sh 10.1.99.1:17681 $R/mnt2 > $R/logs/client-old.out 2>&1
findmnt -rn $R/mnt2 >/dev/null && echo "old client mounted" || { echo "old client mount failed"; tail -3 $R/logs/client-old.out; }
timeout 60 head -c 20 $R/mnt2/v/d000/f000003 >/dev/null 2>$R/logs/old-read.err; echo "old read rc=$? $(head -1 $R/logs/old-read.err)"
timeout 60 sh -c "echo hi > $R/mnt2/oldwrite" ; echo "old write rc=$?"
grep -h 'refused message' $R/logs/efsd-n*.log | head -2
$OLD/scripts/client.sh stop $R/mnt2 >/dev/null 2>&1 || fusermount3 -u $R/mnt2
$NEW/scripts/w125_p0_cluster.sh stop $NEW $R 17680 >/dev/null 2>&1; rm -rf $R
echo "== 3. new client on old (base) daemons"
$NEW/scripts/w125_p0_cluster.sh start $OLD $R 17680 > /dev/null 2>&1; findmnt -rn $R/mnt >/dev/null && echo "old cluster up (old client)"
$OLD/scripts/client.sh stop $R/mnt >/dev/null 2>&1
mkdir -p $R/mnt3; EFS_FUSE_LOG=$R/logs/fuse-new.log timeout 120 $NEW/scripts/client.sh 10.1.99.1:17681 $R/mnt3 > $R/logs/client-new.out 2>&1
findmnt -rn $R/mnt3 >/dev/null && echo "new client mounted" || echo "new client mount failed"
timeout 60 ls $R/mnt3 >/dev/null 2>&1; echo "new client ls rc=$?"
grep -h 'protocol' $R/logs/fuse-new.log $R/logs/client-new.out 2>/dev/null | head -2
$NEW/scripts/client.sh stop $R/mnt3 >/dev/null 2>&1 || fusermount3 -u $R/mnt3 2>/dev/null
$NEW/scripts/w125_p0_cluster.sh stop $OLD $R 17680 >/dev/null 2>&1; rm -rf $R
