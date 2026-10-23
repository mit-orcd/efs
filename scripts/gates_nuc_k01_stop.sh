#!/bin/bash
# gates_nuc_k01_stop.sh
#
# K1 stop qualification (gates-nuc, Oct 22) on a private 3-daemon TCP
# cluster, ports 18910-18912, stores under $GATES/k01 (must not exist),
# REL = $GATES/src built with make and `make tests/shutdown_faults/efsd`.
# (a) three idle daemons started with scripts/server.sh, raft-mkfs, 20 s
#     idle, each stopped with `scripts/server.sh stop <storage>`: expects
#     script rc 0 and an exit record with every barrier done.
# (b) n1, n2 normal, n3 from tests/shutdown_faults/efsd with
#     EFS_FAULT_SHUTDOWN_PARK=packer EFS_FAULT_SHUTDOWN_TOTAL_MS=6000:
#     expects stop rc 3, record outcome=forced phase=packer, no store file
#     lost; then n3 restarts from the normal build on the preserved store,
#     serves an 8 MiB write+read through a FUSE mount, and its stop reports
#     rc 4 put_sync failed (W71 recovery-uncertain root after a forced stop).
# server.sh derives a node id from addr:port; EFS_MD_RAFT_N=3 needs 1..3,
# so --node-id is passed as an extra efsd argument (last one wins).
# Output on stdout; delete $GATES/k01 afterwards.
set -u
GATES=${GATES:-$HOME/efs-gates-nuc}
REL=$GATES/src; K=$GATES/k01; A=127.0.0.1
export EFS_TRANSPORT=tcp EFS_MD_RAFT_N=3 TMPDIR=$GATES/tmp EFS_CLIENT_SKIP_FOREIGN=1
cd "$REL" || exit 2
ms() { date +%s%3N; }
for p in 18910 18911 18912; do ss -ltnH "sport = :$p" | grep -q . && { echo "port $p in use"; exit 1; }; done
[ -e "$K" ] && { echo "$K exists"; exit 1; }
mkdir -p "$K"
start3() {
  ./scripts/server.sh $A:18910 $K/n1:2G --node-id 1 || return 1
  ./scripts/server.sh $A:18911 $K/n2:2G $A:18910 --node-id 2 || return 1
  ./scripts/server.sh $A:18912 $K/n3:2G $A:18910 --node-id 3 || return 1
}
waitlisten() { for t in $(seq 120); do grep -q "listening on $A:$2" "$1/log/efsd.log" 2>/dev/null && return 0; sleep 0.5; done; return 1; }
stopone() { # label storage
  local t0 rc t1; t0=$(ms); ./scripts/server.sh stop "$2" > "$K/stop-$1.out" 2>&1; rc=$?; t1=$(ms)
  echo "STOP $1 storage=$2 script_rc=$rc wall_ms=$((t1-t0))"; sed 's/^/  out: /' "$K/stop-$1.out"
  sed 's/^/  exit: /' "$2/log/efsd.exit" 2>/dev/null || echo "  exit: (no record)"
}
echo "== (a) orderly stop of idle daemons"
start3 || { echo "start failed"; exit 1; }
for i in 1 2 3; do waitlisten $K/n$i $((18909+i)) || echo "n$i not listening"; done
for t in $(seq 60); do ./efs-mgmt raft-mkfs $A:18910 2>&1 | grep -q 'raft-mkfs rc=0 ' && { echo "mkfs ok after ${t}s"; break; }; sleep 1; done
./efs-mgmt raft-status $A:18910 2>&1 | head -12
sleep 20   # idle
stopone a-n3 $K/n3; stopone a-n2 $K/n2; stopone a-n1 $K/n1
echo "== (b) forced stop of a daemon with a parked packer"
start3_b() {
  ./scripts/server.sh $A:18910 $K/n1:2G --node-id 1 || return 1
  ./scripts/server.sh $A:18911 $K/n2:2G $A:18910 --node-id 2 || return 1
  # n3 from the fault build, same node id and arguments as server.sh would use
  NODE=3
  EFS_FAULT_SHUTDOWN_PARK=packer EFS_FAULT_SHUTDOWN_TOTAL_MS=6000 \
    ./tests/shutdown_faults/efsd --node-id $NODE --addr $A --port 18912 --storage $K/n3 \
    --quota 2G --join $A:18910 >> $K/n3/log/efsd.log 2>&1 &
  echo $! > $K/n3/log/efsd.pid
  python3 ./scripts/server_processes.py record $K/n3/log/efsd.pid $K/n3
}
start3_b || { echo "start b failed"; exit 1; }
waitlisten $K/n3 18912 || echo "n3 (fault build) not listening"
sleep 15
./efs-mgmt raft-status $A:18910 2>&1 | head -12
find $K/n3 -type f ! -path '*/log/*' ! -name '*.new' ! -name '*.tmp*' | sort > $K/n3-before.txt
stopone b-n3-forced $K/n3
find $K/n3 -type f ! -path '*/log/*' ! -name '*.new' ! -name '*.tmp*' | sort > $K/n3-after.txt
echo "store files before=$(wc -l < $K/n3-before.txt) after=$(wc -l < $K/n3-after.txt) missing=$(comm -23 $K/n3-before.txt $K/n3-after.txt | wc -l)"
grep -n "deadline .* passed" $K/n3/log/efsd.log | tail -2
echo "== (b) restart n3 on the preserved store (normal build)"
./scripts/server.sh $A:18912 $K/n3:2G $A:18910 --node-id 3; echo "first start rc=$?"
if ! pgrep -f -- "--port 18912 " > /dev/null; then ./scripts/server.sh $A:18912 $K/n3:2G $A:18910 --node-id 3; echo "rerun start rc=$?"; fi
sleep 3; waitlisten $K/n3 18912 && echo "n3 listening again"
sleep 10
./efs-mgmt raft-status $A:18912 2>&1 | head -12
mkdir -p $K/mnt
EFS_FUSE_LOG=$K/fuse.log ./scripts/client.sh $A:18910 $K/mnt > $K/client.out 2>&1; echo "mount rc=$?"
head -c 8M /dev/urandom > $K/probe.bin
cp $K/probe.bin $K/mnt/probe.bin && sync && cmp $K/probe.bin $K/mnt/probe.bin && echo "serve check: 8 MiB write+read PASS"
./scripts/client.sh stop $K/mnt > $K/client-stop.out 2>&1; echo "client stop rc=$?"
findmnt -rn $K/mnt > /dev/null && echo "STILL MOUNTED $K/mnt"
stopone b-n3-after-restart $K/n3; stopone b-n2 $K/n2; stopone b-n1 $K/n1
pgrep -af -- "--port 1891[012] " && echo "LEFTOVER DAEMONS" || echo "no daemons left on 18910-18912"
