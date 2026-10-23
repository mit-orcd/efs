#!/bin/bash
# append_oracle_cluster.sh start|stop|remount|rounds REL DIR [PORT] [-- oracle args]
#
# W87 live appender gate on a private 3-daemon TCP cluster (loopback,
# ports PORT+1..PORT+3, default 18201-18203, stores under DIR/n{1,2,3},
# server page cache (--no-direct-io) unless EFS_K09_DIRECT=1, one efs-fuse
# mount at DIR/mnt). REL holds efsd, efs-fuse, efs-mgmt and scripts/ of one
# build. Never touches orcd-mini (17432-17434) or w100 (27432/27433).
#
#   rounds REL DIR PORT N [-- oracle args]
#       runs tests/live/append_oracle.py N times against DIR/mnt, each with a
#       cold remount between the warm and cold reads, and stops at the first
#       failure (its file is kept under DIR/mnt, verdict under DIR/logs).
#       K09_COLD_CMD replaces the cold step (default: remount; "... restart"
#       restarts all three daemons too). Background load on the mount must
#       be quiesced by that command, or client stop refuses (busy mount).
set -u
cmd=$1; REL=$2; D=$3; PORT=${4:-18200}
ADDR=${EFS_K09_ADDR:-127.0.0.1}
export EFS_TRANSPORT=tcp
IO=--no-direct-io; [ "${EFS_K09_DIRECT:-0}" = 1 ] && IO=--direct-io
HERE=$(cd "$(dirname "$0")" && pwd)

start_daemons() {
  for i in 1 2 3; do
    ss -ltnH "sport = :$((PORT+i))" | grep -q . && { echo "port $((PORT+i)) in use"; exit 1; }
  done
  mkdir -p "$D"/{n1,n2,n3,logs,mnt}
  for i in 1 2 3; do
    join=""; [ $i -gt 1 ] && join="--join $ADDR:$((PORT+1))"
    nohup "$REL/efsd" --node-id $i --addr $ADDR --port $((PORT+i)) --storage "$D/n$i" \
      --quota "${EFS_K09_QUOTA:-4G}" $IO $join >> "$D/logs/efsd-n$i.log" 2>&1 &
    echo $! > "$D/logs/n$i.pid"
    for t in $(seq 60); do grep -q listening "$D/logs/efsd-n$i.log" && break; sleep 0.5; done
  done
}
mount_client() {
  EFS_FUSE_LOG="$D/logs/fuse.log" "$REL/scripts/client.sh" $ADDR:$((PORT+1)) "$D/mnt" > "$D/logs/client.out" 2>&1
  findmnt -rn "$D/mnt" > /dev/null && echo "mounted $D/mnt" || { echo "mount failed"; tail -5 "$D/logs/client.out"; exit 1; }
}
stop_daemons() {
  for i in 1 2 3; do kill "$(cat "$D/logs/n$i.pid" 2>/dev/null)" 2>/dev/null; done
  sleep 3
  for i in 1 2 3; do kill -9 "$(cat "$D/logs/n$i.pid" 2>/dev/null)" 2>/dev/null; done
}
case $cmd in
start)
  start_daemons
  for t in $(seq 30); do "$REL/efs-mgmt" raft-mkfs $ADDR:$((PORT+1)) 2>&1 | grep -q 'raft-mkfs rc=0 ' && break; sleep 1; done
  mount_client
  ;;
remount)
  "$REL/scripts/client.sh" stop "$D/mnt" > /dev/null 2>&1 || { echo "client stop failed"; exit 1; }
  mount_client
  ;;
restart)
  "$REL/scripts/client.sh" stop "$D/mnt" > /dev/null 2>&1 || { echo "client stop failed"; exit 1; }
  stop_daemons; start_daemons; sleep 2; mount_client
  ;;
stop)
  "$REL/scripts/client.sh" stop "$D/mnt" > /dev/null 2>&1
  stop_daemons
  echo stopped
  ;;
rounds)
  N=$5; shift 5; [ "${1:-}" = "--" ] && shift
  for r in $(seq "$N"); do
    echo "=== round $r/$N $(date -u +%FT%TZ)"
    python3 "$HERE/append_oracle.py" --mount "$D/mnt" --keep --out "$D/logs/round-$r.json" \
      --cold-cmd "${K09_COLD_CMD:-$0 remount $REL $D $PORT}" "$@" || { echo "FAIL at round $r"; exit 1; }
  done
  echo "PASS $N rounds"
  ;;
*) echo "usage: $0 start|stop|remount|restart|rounds REL DIR [PORT] [N] [-- oracle args]"; exit 2 ;;
esac
