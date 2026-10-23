#!/bin/bash
# w125_p0_cluster.sh start|stop REL DIR [PORT]
#
# Private 3-daemon RDMA cluster for W125 P0 measurements on node9901 (or
# another node: W125_ADDR=<its address>):
# ports PORT+1..PORT+3 (default 17671-17673), stores under DIR/n{1,2,3},
# --direct-io, one efs-fuse mount at DIR/mnt (W125_TRANSPORT=tcp: TCP instead). Never touches orcd-mini
# (17432-17434) or w100 (27432/27433). REL holds efsd, efs-fuse, efs-mgmt
# and scripts/client.sh of one build.
#
# Every daemon runs under `run-daemon` (an internal subcommand): its pid is
# in DIR/logs/nI.pid and its exit status, once it exits, in DIR/logs/nI.rc.
#
# stop is checked. It unmounts through client.sh stop, verifies that the
# mount is gone, sends TERM to every owned daemon and waits up to
# W125_STOP_WAIT seconds (default 60) for all of them to exit with status
# 0. Success ("stopped clean", exit 0) means exactly that. Anything else
# is a forced stop, exit 1, with one line per reason: a client stop that
# failed or left the mount, a daemon that needed KILL (sent only after the
# wait), one that exited non-zero (EFS_SHUTDOWN_FORCED_RC marks a forced
# daemon shutdown), one that was already gone, or one whose status is
# unknown. Nothing is deleted here; callers keep the store after any
# non-zero status.
set -u
ADDR=${W125_ADDR:-10.1.99.1} # node address; node9900 is 10.1.99.0
cmd=$1
if [ "$cmd" = run-daemon ]; then
  # run-daemon PIDFILE RCFILE COMMAND...: record the pid, run, record the status.
  pidf=$2; rcf=$3; shift 3
  rm -f "$rcf"
  "$@" & pid=$!
  echo "$pid" > "$pidf"
  wait "$pid"; rc=$?
  echo "$rc" > "$rcf"
  exit "$rc"
fi
REL=$2; D=$3; PORT=${4:-17670}
# W125_TRANSPORT=tcp runs the same cluster on a host without RDMA.
export EFS_TRANSPORT=${W125_TRANSPORT:-rdma} EFS_MD_RAFT_N=3
[ "$EFS_TRANSPORT" = tcp ] || export EFS_RDMA_DEV=${EFS_RDMA_DEV:-mlx5_0}
FUSE_LIBDIR=${FUSE_LIBDIR:-/data1/efs/deps/libfuse/lib64}
[ -d "$FUSE_LIBDIR" ] && export LD_LIBRARY_PATH=$FUSE_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
mounted() {
  if command -v findmnt > /dev/null 2>&1; then findmnt -rn "$1" > /dev/null 2>&1
  else mount | grep -q " on $1 "; fi
}
alive() { kill -0 "$1" 2>/dev/null; }
case $cmd in
start)
  for i in 1 2 3; do
    ss -ltnH "sport = :$((PORT+i))" | grep -q . && { echo "port $((PORT+i)) in use"; exit 1; }
  done
  mkdir -p "$D"/{n1,n2,n3,logs,mnt}
  for i in 1 2 3; do
    join=""; [ $i -gt 1 ] && join="--join $ADDR:$((PORT+1))"
    nohup "$0" run-daemon "$D/logs/n$i.pid" "$D/logs/n$i.rc" \
      "$REL/efsd" --node-id $i --addr $ADDR --port $((PORT+i)) --storage "$D/n$i" \
      --quota 200G --direct-io $join >> "$D/logs/efsd-n$i.log" 2>&1 &
    for t in $(seq 60); do grep -q listening "$D/logs/efsd-n$i.log" 2>/dev/null && break; sleep 0.5; done
  done
  for t in $(seq 30); do "$REL/efs-mgmt" raft-mkfs $ADDR:$((PORT+1)) 2>&1 | grep -q 'raft-mkfs rc=0 ' && break; sleep 1; done
  EFS_FUSE_LOG="$D/logs/fuse.log" "$REL/scripts/client.sh" $ADDR:$((PORT+1)) "$D/mnt" > "$D/logs/client.out" 2>&1
  mounted "$D/mnt" && echo "mounted $D/mnt" || { echo "mount failed"; tail -5 "$D/logs/client.out"; exit 1; }
  ;;
stop)
  WAIT=${W125_STOP_WAIT:-60}
  forced=0
  if mounted "$D/mnt"; then
    "$REL/scripts/client.sh" stop "$D/mnt" > "$D/logs/client-stop.out" 2>&1
    crc=$?
    [ $crc -ne 0 ] && { echo "client stop rc=$crc (see $D/logs/client-stop.out)"; forced=1; }
    for t in $(seq 20); do mounted "$D/mnt" || break; sleep 0.5; done
    mounted "$D/mnt" && { echo "still mounted: $D/mnt"; forced=1; }
  fi
  pids=""
  for i in 1 2 3; do
    pid=$(cat "$D/logs/n$i.pid" 2>/dev/null)
    [ -z "$pid" ] && { echo "n$i: no pid recorded"; forced=1; continue; }
    if alive "$pid"; then kill -TERM "$pid" 2>/dev/null; pids="$pids $i:$pid"
    elif [ ! -s "$D/logs/n$i.rc" ]; then echo "n$i pid $pid: already gone, status unknown"; forced=1
    fi
  done
  # All TERMed daemons get the whole wait; a daemon that needs longer is
  # a finding, not something to hide with an early KILL.
  deadline=$(( $(date +%s) + WAIT ))
  for e in $pids; do
    pid=${e#*:}
    while alive "$pid" && [ "$(date +%s)" -lt "$deadline" ]; do sleep 0.5; done
  done
  killed=""
  for e in $pids; do
    i=${e%%:*}; pid=${e#*:}
    if alive "$pid"; then
      echo "n$i pid $pid: still running after ${WAIT}s, needed KILL"; forced=1; killed="$killed $i"
      kill -KILL "$pid" 2>/dev/null
      for t in $(seq 20); do alive "$pid" || break; sleep 0.5; done
      alive "$pid" && echo "n$i pid $pid: survives KILL"
    fi
  done
  for i in 1 2 3; do
    pid=$(cat "$D/logs/n$i.pid" 2>/dev/null); [ -z "$pid" ] && continue
    case " $killed " in *" $i "*) continue ;; esac
    # run-daemon writes the status right after the child exits.
    for t in $(seq 10); do [ -s "$D/logs/n$i.rc" ] && break; sleep 0.2; done
    rc=$(cat "$D/logs/n$i.rc" 2>/dev/null)
    case "$rc" in
      0) ;;
      "") echo "n$i pid $pid: exit status unknown"; forced=1 ;;
      *) echo "n$i pid $pid: exited rc=$rc"; forced=1 ;;
    esac
  done
  if [ $forced -ne 0 ]; then echo "stopped forced"; exit 1; fi
  echo "stopped clean"
  ;;
*) echo "usage: $0 start|stop REL DIR [PORT]"; exit 2 ;;
esac
