#!/bin/bash
# w125_mac_cluster.sh start|stop|status [DIR] [PORT]
#
# Small private 3-daemon efs cluster on the Mac's Tart VMs (~/git/cluster
# "mac" cluster), driven from the Mac over ssh: n1 on efs2, n2 and n3 on
# efs3 (efs1 is left to other work), TCP, ports PORT+1..PORT+3 (default
# 33433-33435; the mac cluster's own efsd uses 17432, other sessions'
# private clusters 32432-32434). One efs-fuse mount on efs2 at DIR/mnt.
# Binaries come from /data1/w125/src on both VMs (built there with
# `make efsd efs-fuse efs-mgmt`). Packer settings pass through from the
# caller's EFS_PACK_* environment. Quota per daemon: QUOTA (default 4G).
set -u
cmd=$1; D=${2:-/data1/w125/run}; PORT=${3:-33432}; QUOTA=${QUOTA:-4G}
SRC=/data1/w125/src
H=(efs2 efs3 efs3); A=(192.168.2.12 192.168.2.13 192.168.2.13)
PACKENV=$(env | grep -E '^EFS_PACK_[A-Z_]+=' | tr '\n' ' ')
case $cmd in
start)
  # A mount left from an earlier run would be reused silently (and talk to
  # this new cluster with stale client state): refuse instead.
  for h in efs2 efs3; do
    ssh $h "grep -q ' $D/mnt' /proc/mounts" && { echo "$h: an efs mount under $D is still present; run stop first"; exit 1; }
  done
  for i in 0 1 2; do
    id=$((i + 1)); join=""; [ $id -gt 1 ] && join="--join ${A[0]}:$((PORT + 1))"
    ssh "${H[$i]}" "mkdir -p $D/n$id $D/logs && cd $SRC && env $PACKENV setsid -f ./efsd --node-id $id \
      --addr ${A[$i]} --port $((PORT + id)) --storage $D/n$id --quota $QUOTA $join \
      >> $D/logs/efsd-n$id.log 2>&1 < /dev/null" || exit 1
    ssh "${H[$i]}" "for t in \$(seq 60); do grep -q listening $D/logs/efsd-n$id.log && break; sleep 0.5; done;
      grep -q listening $D/logs/efsd-n$id.log || { echo 'n$id did not start'; tail -5 $D/logs/efsd-n$id.log; exit 1; }" || exit 1
  done
  ssh "${H[0]}" "cd $SRC && for t in \$(seq 30); do ./efs-mgmt raft-mkfs ${A[0]}:$((PORT + 1)) 2>&1 | grep -q 'raft-mkfs rc=0 ' && break; sleep 1; done;
    mkdir -p $D/mnt && EFS_FUSE_LOG=$D/logs/fuse.log scripts/client.sh ${A[0]}:$((PORT + 1)) $D/mnt > $D/logs/client.out 2>&1;
    findmnt -rn $D/mnt > /dev/null && echo mounted $D/mnt || { echo mount failed; tail -5 $D/logs/client.out; exit 1; }"
  ;;
stop)
  # Clients by mount path, not through their log (the log may be gone):
  # stop efs-fuse for any mount under $D, then detach what is left.
  for h in efs2 efs3; do
    ssh $h "for m in \$(awk '\$2 ~ \"^$D/mnt\" {print \$2}' /proc/mounts); do
      pkill -TERM -f \"^./efs-fuse .* \$m\$\"; sleep 2; pkill -KILL -f \"^./efs-fuse .* \$m\$\";
      fusermount3 -u -z \$m 2>/dev/null; done; true"
  done
  for sig in TERM KILL; do
    for i in 0 1 2; do
      ssh "${H[$i]}" "pkill -$sig -f '^./efsd .*--storage $D/n$((i + 1))( |\$)'; true"
    done
    sleep 3
  done
  echo stopped
  ;;
status)
  ssh "${H[0]}" "cd $SRC && ./efs-mgmt status ${A[0]}:$((PORT + 1)) 2>&1 | head -12"
  ;;
*) echo "usage: $0 start|stop|status [DIR] [PORT]"; exit 2 ;;
esac
