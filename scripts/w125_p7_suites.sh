#!/bin/bash
# w125_p7_suites.sh REL OUT
#
# W125 P7 regression suites with the packer active, on node9901 private
# fixtures only (never orcd-mini or w100):
#   two-client  posix_2client.py --local on two efs-fuse mounts of one
#               private 3-daemon RDMA cluster (17681-17683), minimum age 0;
#   persist     posix_persist.py prepare, 30 s for packing, remount, verify;
#   gc          tests/live/gc_reclamation.py --posix (owned
#               four-node TCP fixture), minimum age 2 s;
#   expiry      posix_suite.py --suite expiry (owned four-node fixture),
#               minimum age 2 s.
# Results land in OUT/<suite>.txt; the script prints one line per suite.
set -u
HERE=$(cd "$(dirname "$0")" && pwd); REL=$1; OUT=$2; R=/tmp/w125-p7-suites
mkdir -p "$OUT"; T=$(dirname "$HERE")/tests
export LD_LIBRARY_PATH=/data1/efs/deps/libfuse/lib64
"$HERE/w125_p0_cluster.sh" stop "$REL" $R 17680 >/dev/null 2>&1; rm -rf $R
EFS_PACK_MIN_AGE_MS=0 "$HERE/w125_p0_cluster.sh" start "$REL" $R 17680 >/dev/null 2>&1
mkdir -p $R/mntb
EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0 EFS_FUSE_LOG=$R/logs/fuse-b.log "$REL/scripts/client.sh" 10.1.99.1:17681 $R/mntb >/dev/null 2>&1
python3 "$T/posix/posix_2client.py" --local $R/mnt $R/mntb --results "$OUT/two-client.txt" > "$OUT/two-client.out" 2>&1
echo "two-client rc=$? $(tail -1 "$OUT/two-client.out")"
"$REL/scripts/client.sh" stop $R/mntb >/dev/null 2>&1
python3 "$T/posix/posix_persist.py" $R/mnt --phase prepare > "$OUT/persist-prepare.out" 2>&1
sleep 30
"$REL/scripts/client.sh" stop $R/mnt >/dev/null 2>&1
EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0 EFS_FUSE_LOG=$R/logs/fuse2.log "$REL/scripts/client.sh" 10.1.99.1:17681 $R/mnt >/dev/null 2>&1
python3 "$T/posix/posix_persist.py" $R/mnt --phase verify > "$OUT/persist.out" 2>&1
echo "persist rc=$? $(tail -1 "$OUT/persist.out") packed=$(grep -h 'packed_total=' $R/logs/efsd-n*.log | tail -1 | sed 's/.*packed_total=//')"
"$HERE/w125_p0_cluster.sh" stop "$REL" $R 17680 >/dev/null 2>&1; rm -rf $R
rm -rf /tmp/w125-gc-a /tmp/w125-gc-b; mkdir -p /tmp/w125-gc-a /tmp/w125-gc-b
(cd "$(dirname "$HERE")" && EFS_PACK_MIN_AGE_MS=2000 python3 tests/live/gc_reclamation.py --posix \
   --root /tmp/w125-gc-a --root /tmp/w125-gc-b > "$OUT/gc.out" 2>&1)
echo "gc rc=$? $(tail -1 "$OUT/gc.out")"
rm -rf /tmp/w125-exp-*; mkdir -p /tmp/w125-exp-a /tmp/w125-exp-b /tmp/w125-exp-mnt /tmp/w125-exp-peer
(cd "$(dirname "$HERE")" && EFS_PACK_MIN_AGE_MS=2000 python3 tests/posix/posix_suite.py /tmp/w125-exp-mnt --suite expiry \
   --root /tmp/w125-exp-a --root /tmp/w125-exp-b --peer-mount /tmp/w125-exp-peer > "$OUT/expiry.out" 2>&1)
echo "expiry rc=$? $(tail -1 "$OUT/expiry.out")"
