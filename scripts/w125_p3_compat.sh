#!/bin/bash
# w125_p3_compat.sh NEW_REL OLD_REL [RUN]
#
# W125 P3 / W133 (K4) protocol-declaration gate on a private node9901
# cluster (ports 17681-17683; never orcd-mini or w100). EFS has no backward
# compatibility: a client and its daemons must run the same protocol
# (EFS_PROTOCOL_VERSION), and a daemon serves nothing but HEARTBEAT on a
# connection without an accepted CLIENT_HELLO. Expected:
#   1. new daemons + new client: 1,000 files of 1 B to 16 KiB written,
#      packed (2 s minimum age) and read back byte for byte;
#   2. old client (OLD_REL efs-fuse) on new daemons: REFUSED. The mount
#      fails (or every operation on it fails) and every daemon logs
#      "handler: refused ..." for the undeclared or mismatched connection;
#      nothing is served, writes included;
#   3. new client on new-protocol-unaware old daemons: REFUSED by the
#      client ("Cluster does not support client protocol"); no mount.
# Prints PASS/FAIL per check and exits nonzero on any failure. Case 3 may
# leave a stale mount if the old client.sh stop hangs: fusermount3 -uz.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
NEW=$1; OLD=$2; R=${3:-/tmp/w125-p3-run}
export LD_LIBRARY_PATH=/data1/efs/deps/libfuse/lib64 EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0
fail=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; fail=1; fi; }
$NEW/scripts/w125_p0_cluster.sh stop $NEW $R 17680 >/dev/null 2>&1; rm -rf $R

echo "== 1. new daemons, new client: packed files read back"
EFS_PACK_MIN_AGE_MS=2000 $NEW/scripts/w125_p0_cluster.sh start $NEW $R 17680 | tail -1
check "1 write 1000 files" "python3 '$HERE/w125_p3_readback.py' write $R/mnt/v 1000"
sleep 25
check "1 verify 1000 files byte for byte" "python3 '$HERE/w125_p3_readback.py' verify $R/mnt/v 1000"
$NEW/scripts/client.sh stop $R/mnt >/dev/null 2>&1

echo "== 2. old (base) client on new daemons: refused, nothing served"
mkdir -p $R/mnt2; EFS_FUSE_LOG=$R/logs/fuse-old.log timeout 120 $OLD/scripts/client.sh 10.1.99.1:17681 $R/mnt2 > $R/logs/client-old.out 2>&1
if findmnt -rn $R/mnt2 >/dev/null; then
  echo "old client mounted (its daemons answered something); every operation must fail"
  timeout 60 head -c 20 $R/mnt2/v/d000/f000003 >/dev/null 2>$R/logs/old-read.err; rrc=$?
  timeout 60 sh -c "echo hi > $R/mnt2/oldwrite" 2>/dev/null; wrc=$?
  check "2 old client read fails (rc=$rrc)" "[ $rrc -ne 0 ]"
  check "2 old client write fails (rc=$wrc)" "[ $wrc -ne 0 ]"
  check "2 no file written through the old client" "! [ -e $R/mnt/v/oldwrite ] && ! [ -e $R/mnt2/oldwrite ]"
else
  echo "PASS 2 old client did not mount: $(grep -h -m1 'protocol\|refused\|Cannot' $R/logs/client-old.out $R/logs/fuse-old.log 2>/dev/null | head -1)"
fi
check "2 daemons logged the refusal" "grep -qh 'handler: refused' $R/logs/efsd-n*.log"
grep -h 'handler: refused' $R/logs/efsd-n*.log | head -2
$OLD/scripts/client.sh stop $R/mnt2 >/dev/null 2>&1 || fusermount3 -uz $R/mnt2 2>/dev/null
$NEW/scripts/w125_p0_cluster.sh stop $NEW $R 17680 >/dev/null 2>&1; rm -rf $R

echo "== 3. new client on old (base) daemons: client refuses the cluster"
$NEW/scripts/w125_p0_cluster.sh start $OLD $R 17680 > /dev/null 2>&1; findmnt -rn $R/mnt >/dev/null && echo "old cluster up (old client)"
$OLD/scripts/client.sh stop $R/mnt >/dev/null 2>&1
mkdir -p $R/mnt3; EFS_FUSE_LOG=$R/logs/fuse-new.log timeout 120 $NEW/scripts/client.sh 10.1.99.1:17681 $R/mnt3 > $R/logs/client-new.out 2>&1
check "3 new client did not mount" "! findmnt -rn $R/mnt3 >/dev/null"
check "3 new client named the protocol refusal" "grep -qh 'does not support client protocol\|refused protocol\|did not answer the protocol' $R/logs/fuse-new.log $R/logs/client-new.out"
grep -h 'protocol' $R/logs/fuse-new.log $R/logs/client-new.out 2>/dev/null | head -2
$NEW/scripts/client.sh stop $R/mnt3 >/dev/null 2>&1 || fusermount3 -uz $R/mnt3 2>/dev/null
$NEW/scripts/w125_p0_cluster.sh stop $OLD $R 17680 >/dev/null 2>&1; rm -rf $R
[ $fail = 0 ] && echo "w125_p3_compat: PASS" || echo "w125_p3_compat: FAIL"
exit $fail
