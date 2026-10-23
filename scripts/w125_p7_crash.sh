#!/bin/bash
# w125_p7_crash.sh FAULT_REL [RUN] [POINTS...]
#
# W125 P7 crash injection on a private node9901 cluster (ports 17681-17683;
# never orcd-mini or w100). FAULT_REL must be built with EFS_FAULTS=1. For
# each point (default: admitted uploaded switched gc_accepted
# retire_accepted) node 1, the leader of a fresh cluster, exits as if killed
# the first time it reaches that packer/reclaim step
# (EFS_PACK_CRASH, EFS_PACK_CRASH_NODE=1). The node is restarted without the
# fault; then every file must read back byte for byte, and after deleting
# everything no per-file fragment and no segment file may remain on any node
# (no orphan from the interrupted step).
set -u
ADDR=${W125_ADDR:-10.1.99.1} # node address; node9900 is 10.1.99.0
HERE=$(cd "$(dirname "$0")" && pwd); REL=$1; R=${2:-/tmp/w125-p7-crash}; shift 2 2>/dev/null
POINTS=${*:-admitted uploaded switched gc_accepted retire_accepted}
PORT=17680; FAIL=0
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0 LD_LIBRARY_PATH=/data1/efs/deps/libfuse/lib64
inv() { local t=0; for i in 1 2 3; do t=$((t + $(find $R/n$i/data/exports $R/n$i/data/segments -type f 2>/dev/null | wc -l))); done; echo $t; }
for point in $POINTS; do
  "$HERE/w125_p0_cluster.sh" stop "$REL" $R $PORT >/dev/null 2>&1; rm -rf $R
  EFS_PACK_MIN_AGE_MS=2000 EFS_PACK_CRASH=$point EFS_PACK_CRASH_NODE=1 \
    "$HERE/w125_p0_cluster.sh" start "$REL" $R $PORT >/dev/null 2>&1
  python3 "$HERE/w125_p3_readback.py" write $R/mnt/keep 600 >/dev/null
  python3 "$HERE/w125_p3_readback.py" write $R/mnt/drop 600 >/dev/null
  [ $point = retire_accepted ] && { sleep 20; rm -rf $R/mnt/drop; }
  pid=$(cat $R/logs/n1.pid)
  for t in $(seq 180); do kill -0 $pid 2>/dev/null || break; sleep 1; done
  if kill -0 $pid 2>/dev/null; then echo "FAIL $point: crash point not reached"; FAIL=1; continue; fi
  grep -q "injected crash at $point" $R/logs/efsd-n1.log || { echo "FAIL $point: n1 exited without the injected crash"; FAIL=1; }
  EFS_PACK_MIN_AGE_MS=2000 nohup "$REL/efsd" --node-id 1 --addr $ADDR --port $((PORT+1)) --storage $R/n1 \
    --quota 200G --direct-io >> $R/logs/efsd-n1.log 2>&1 &
  echo $! > $R/logs/n1.pid
  sleep 60
  v=$(timeout 300 python3 "$HERE/w125_p3_readback.py" verify $R/mnt/keep 600)
  [ $point != retire_accepted ] && rm -rf $R/mnt/drop
  rm -rf $R/mnt/keep
  left=; for t in $(seq 180); do left=$(inv); [ "$left" = 0 ] && break; sleep 1; done
  if echo "$v" | grep -q 'bad=0' && [ "$left" = 0 ]; then echo "PASS $point: 600 files verified after restart, no fragments or segments left"
  else echo "FAIL $point: $v; files left on nodes=$left"; FAIL=1; fi
done
"$HERE/w125_p0_cluster.sh" stop "$REL" $R $PORT >/dev/null 2>&1; rm -rf $R
[ $FAIL = 0 ] && echo "crash injection: PASS" || echo "crash injection: FAIL"
exit $FAIL
