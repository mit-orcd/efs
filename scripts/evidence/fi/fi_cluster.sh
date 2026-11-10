#!/bin/bash
# fi_cluster.sh start|stop TREE DIR PORT — Task FI: a private three-member
# efsd cluster on fresh stores plus one efs-fuse mount, on one host, for the
# same shape as orcd-mini (seed + two joiners, RDMA, one scripts/client.sh
# mount). TREE is a built source tree; its own source decides the command
# line, so one script starts both an old (--node-id/--storage/--direct-io,
# raft-mkfs) and a current (positional endpoint) build. Ports PORT+1..PORT+3,
# stores DIR/n{1,2,3}, logs DIR/logs, mount DIR/mnt, pids DIR/logs/pids.
# Writes nothing outside DIR; never touches 17xxx or /data1/efs.
set -u
cmd=${1:?}; TREE=${2:?}; D=${3:?}; P=${4:?}
for v in $(env | sed -n 's/^\(EFS_[A-Za-z0-9_]*\)=.*/\1/p'); do unset "$v"; done
export EFS_TRANSPORT=${FI_TRANSPORT:-rdma} EFS_RDMA_DEV=${FI_RDMA_DEV:-mlx5_0}
PFX=${EFS_FUSE_PREFIX:-/data1/efs/deps/libfuse}
export LD_LIBRARY_PATH=$PFX/lib64
H=127.0.0.1; QUOTA=${FI_QUOTA:-200G}; L=$D/logs
SRC=$TREE/src/server/efsd.c
# a build that refuses the flag says "was removed (K33)" / "(D42)" in its source
old=1; grep -q 'was removed (K33)' "$SRC" && old=0
dio=--direct-io; grep -q 'was removed (D42)' "$SRC" && dio=""
case $cmd in
start)
  for i in 1 2 3; do
    ss -ltnH "sport = :$((P+i))" | grep -q . && { echo "port $((P+i)) in use"; exit 1; }
  done
  mkdir -p "$D"/{n1,n2,n3,mnt} "$L"; : > "$L/pids"
  # before W153 the member count is an env var; W153 and later refuse it
  grep -rqE --include=*.c 'EFS_MD_RAFT_N was removed|W153\): the member count' "$TREE/src" || export EFS_MD_RAFT_N=3
  for i in 1 2 3; do
    if [ $old = 1 ]; then
      j=""; [ $i -gt 1 ] && j="--join $H:$((P+1))"
      a=("$TREE/efsd" --node-id $i --addr $H --port $((P+i)) --storage "$D/n$i" --quota "$QUOTA" $dio $j)
    elif [ $i = 1 ]; then
      a=("$TREE/efsd" $H:$((P+1)) "$D/n1" --quota "$QUOTA" $dio)
    else
      a=("$TREE/efsd" --join $H:$((P+1)) --listen $H:$((P+i)) "$D/n$i" --quota "$QUOTA" $dio)
    fi
    echo "start ${a[*]}" >> "$L/efsd-n$i.log"
    setsid nohup "${a[@]}" >> "$L/efsd-n$i.log" 2>&1 < /dev/null &
    echo "efsd$i $!" >> "$L/pids"
    for t in $(seq 120); do grep -q listening "$L/efsd-n$i.log" && break; sleep 0.5; done
  done
  if [ $old = 1 ]; then
    for t in $(seq 60); do "$TREE/efs-mgmt" raft-mkfs $H:$((P+1)) 2>&1 | grep -q 'raft-mkfs rc=0 ' && break; sleep 1; done
  else
    for i in 1 2 3; do
      for t in $(seq 180); do grep -q 'raft-host: formed: 3 members' "$L/efsd-n$i.log" && break; sleep 0.5; done
    done
  fi
  (cd "$TREE" && EFS_FUSE_LOG="$L/fuse.log" scripts/client.sh $H:$((P+1)) "$D/mnt" > "$L/client.out" 2>&1)
  findmnt -rn "$D/mnt" > /dev/null || { echo "mount failed"; tail -5 "$L/client.out"; exit 1; }
  echo "fuse $(sed -n 's/.*efs-fuse daemon pid=\([0-9]*\).*/\1/p' "$L/fuse.log" | tail -1)" >> "$L/pids"
  echo "started old=$old dio=${dio:-none} transport=$EFS_TRANSPORT dev=$EFS_RDMA_DEV ports=$((P+1))-$((P+3))"
  ;;
stop)
  (cd "$TREE" && scripts/client.sh stop "$D/mnt") > "$L/client-stop.out" 2>&1 || tail -3 "$L/client-stop.out"
  while read -r n p; do case $n in efsd*) kill "$p" 2>/dev/null;; esac; done < "$L/pids"
  for t in $(seq 120); do
    live=0; while read -r n p; do case $n in efsd*) kill -0 "$p" 2>/dev/null && live=1;; esac; done < "$L/pids"
    [ $live = 0 ] && break; sleep 0.5
  done
  [ $live = 0 ] && echo stopped || echo "efsd still running after SIGTERM: $(cat "$L/pids")"
  ;;
*) echo "usage: $0 start|stop TREE DIR PORT"; exit 2 ;;
esac
