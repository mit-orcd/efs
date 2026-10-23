#!/bin/bash
# gate_k28_five_node.sh start|stop|node-stop|node-start|counts|status REL DIR [ID]
#
# K28/W151 live gate helper: a private five-daemon TCP cluster on loopback,
# ports 19001-19005 (EFS_K28_PORT base, default 19000), five committed members,
# stores DIR/n1..n5, node 1 the seed and nodes 2-5 --join, one efs-fuse
# mount at DIR/mnt. REL holds efsd, efs-fuse, efs-mgmt and scripts/ of one
# build. Stops only the PIDs it recorded under DIR/logs and unmounts only
# DIR/mnt. EFS_PACK_MIN_AGE_MS (default here 2000) lets the packer take
# small files quickly; it is a documented runtime tunable.
#
#   start            start five daemons, raft-mkfs, mount the client
#   node-stop ID     SIGTERM one recorded daemon (SIGKILL after 10 s)
#   node-start ID    restart that daemon on its store (node 1 rejoins via 2)
#   counts           fragment files (data/exports) and segment fragments
#                    (data/segments) per store
#   status           membership, raft-status, packer summary
#   stop             unmount the client and stop all five daemons
set -u
cmd=$1; REL=$2; D=$3; ID=${4:-}
PORT=${EFS_K28_PORT:-19000}; ADDR=127.0.0.1; N=5
export EFS_TRANSPORT=tcp EFS_PACK_MIN_AGE_MS=${EFS_PACK_MIN_AGE_MS:-2000}

start_node() {
  local i=$1 join=""
  [ "$i" -gt 1 ] && join="--join $ADDR:$((PORT+1))"
  ss -ltnH "sport = :$((PORT+i))" | grep -q . && { echo "port $((PORT+i)) in use"; return 1; }
  nohup "$REL/efsd" --node-id "$i" --addr $ADDR --port $((PORT+i)) --storage "$D/n$i" \
    --quota "${EFS_K28_QUOTA:-8G}" --no-direct-io $join >> "$D/logs/efsd-n$i.log" 2>&1 &
  echo $! > "$D/logs/n$i.pid"; touch "$D/logs/started-$i"
  for t in $(seq 120); do ss -ltnH "sport = :$((PORT+i))" | grep -q . && return 0; sleep 0.5; done
  echo "node $i not listening"; return 1
}
stop_node() {
  local p; p=$(cat "$D/logs/n$1.pid" 2>/dev/null) || return 0
  kill "$p" 2>/dev/null
  for t in $(seq 20); do kill -0 "$p" 2>/dev/null || return 0; sleep 0.5; done
  kill -9 "$p" 2>/dev/null; echo "node $1 SIGKILL after 10 s"
}
case $cmd in
start)
  mkdir -p "$D"/{n1,n2,n3,n4,n5,logs,mnt}
  for i in $(seq $N); do start_node "$i" || exit 1; done
  # W153: formation commits the members joined at that moment; wait for all N.
  for t in $(seq 120); do
    "$REL/efs-mgmt" status $ADDR:$((PORT+1)) 2>/dev/null | grep -q "Cluster nodes ($N)" && break; sleep 0.5
  done
  for t in $(seq 60); do
    "$REL/efs-mgmt" raft-mkfs $ADDR:$((PORT+1)) > "$D/logs/mkfs.out" 2>&1
    grep -q 'raft-mkfs rc=0 ' "$D/logs/mkfs.out" && break; sleep 1
  done
  cat "$D/logs/mkfs.out"
  EFS_FUSE_LOG="$D/logs/fuse.log" "$REL/scripts/client.sh" $ADDR:$((PORT+1)) "$D/mnt" > "$D/logs/client.out" 2>&1
  findmnt -rn "$D/mnt" > /dev/null && echo "mounted $D/mnt" || { echo "mount failed"; tail -5 "$D/logs/client.out"; exit 1; }
  ;;
node-stop) stop_node "$ID"; echo "node $ID stopped" ;;
node-start) start_node "$ID" && echo "node $ID started" ;;
counts)
  for i in $(seq $N); do
    f=$(find "$D/n$i/data/exports" -type f 2>/dev/null | wc -l)
    s=$(find "$D/n$i/data/segments" -type f 2>/dev/null | wc -l)
    echo "n$i fragments=$f segment_fragments=$s"
  done
  ;;
status)
  "$REL/efs-mgmt" status $ADDR:$((PORT+${ID:-1}))
  "$REL/efs-mgmt" raft-status $ADDR:$((PORT+${ID:-1}))
  "$REL/efs-mgmt" pack-compact $ADDR:$((PORT+${ID:-1})) 0
  ;;
stop)
  "$REL/scripts/client.sh" stop "$D/mnt" > /dev/null 2>&1 || echo "client stop failed"
  for i in $(seq $N); do stop_node "$i"; done
  echo stopped
  ;;
*) echo "usage: $0 start|stop|node-stop|node-start|counts|status REL DIR [ID]"; exit 2 ;;
esac
