#!/bin/bash
# w125_p0_cluster.sh start|stop REL DIR [PORT]
#
# Private 3-daemon RDMA cluster for W125 P0 measurements on node9901 (or
# another node: W125_ADDR=<its address>):
# ports PORT+1..PORT+3 (default 17671-17673), stores under DIR/n{1,2,3},
# --direct-io, one efs-fuse mount at DIR/mnt. Never touches orcd-mini
# (17432-17434) or w100 (27432/27433). REL holds efsd, efs-fuse, efs-mgmt
# and scripts/client.sh of one build.
set -u
ADDR=${W125_ADDR:-10.1.99.1} # node address; node9900 is 10.1.99.0
cmd=$1; REL=$2; D=$3; PORT=${4:-17670}
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=mlx5_0 EFS_MD_RAFT_N=3
FUSE_LIBDIR=${FUSE_LIBDIR:-/data1/efs/deps/libfuse/lib64}
[ -d "$FUSE_LIBDIR" ] && export LD_LIBRARY_PATH=$FUSE_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
case $cmd in
start)
  for i in 1 2 3; do
    ss -ltnH "sport = :$((PORT+i))" | grep -q . && { echo "port $((PORT+i)) in use"; exit 1; }
  done
  mkdir -p "$D"/{n1,n2,n3,logs,mnt}
  for i in 1 2 3; do
    join=""; [ $i -gt 1 ] && join="--join $ADDR:$((PORT+1))"
    nohup "$REL/efsd" --node-id $i --addr $ADDR --port $((PORT+i)) --storage "$D/n$i" \
      --quota 200G --direct-io $join >> "$D/logs/efsd-n$i.log" 2>&1 &
    echo $! > "$D/logs/n$i.pid"
    for t in $(seq 60); do grep -q listening "$D/logs/efsd-n$i.log" && break; sleep 0.5; done
  done
  for t in $(seq 30); do "$REL/efs-mgmt" raft-mkfs $ADDR:$((PORT+1)) 2>&1 | grep -q 'raft-mkfs rc=0 ' && break; sleep 1; done
  EFS_FUSE_LOG="$D/logs/fuse.log" "$REL/scripts/client.sh" $ADDR:$((PORT+1)) "$D/mnt" > "$D/logs/client.out" 2>&1
  findmnt -rn "$D/mnt" > /dev/null && echo "mounted $D/mnt" || { echo "mount failed"; tail -5 "$D/logs/client.out"; exit 1; }
  ;;
stop)
  "$REL/scripts/client.sh" stop "$D/mnt" > /dev/null 2>&1
  for i in 1 2 3; do kill "$(cat "$D/logs/n$i.pid" 2>/dev/null)" 2>/dev/null; done
  sleep 3
  for i in 1 2 3; do kill -9 "$(cat "$D/logs/n$i.pid" 2>/dev/null)" 2>/dev/null; done
  echo stopped
  ;;
*) echo "usage: $0 start|stop REL DIR [PORT]"; exit 2 ;;
esac
