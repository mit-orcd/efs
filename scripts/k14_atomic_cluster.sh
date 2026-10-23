#!/bin/bash
# k14_atomic_cluster.sh run|stop REL DIR [PORT] [FAULT_REL]
#
# K14 / W77 (I24) cluster gate: atomic multi-chunk publication and
# observation. Private 3-daemon TCP cluster on ONE host: ports
# PORT+1..PORT+3 (default 18801-18803), stores DIR/n{1,2,3},
# EFS_MD_RAFT_N=3, two efs-fuse mounts: DIR/mntA (writers) and DIR/mntB
# (readers, a different client with its own caches). REL is a build tree
# (efs-fuse, efs-mgmt, scripts/client.sh); efsd comes from FAULT_REL
# (default REL), which must be built with EFS_FAULTS=1 for phases 4-6.
#
# Two files, each rewritten as ONE 1 MiB O_DIRECT pwrite + fsync per
# version: aligned.dat [0, 1 MiB) = chunks 0-7, straddle.dat
# [64 KiB, 1 MiB + 64 KiB) = chunks 0-8. Every byte encodes its version, so
# a reader's 512 KiB O_DIRECT read (one FUSE read request) inside a region
# must be exactly one version (tests/live/k14_atomic_publication.c).
#   1 steady       writers on A, three readers per file on B
#   2 leaders      the same while every node is asked to step down for both
#                  metadata groups every 3 s
#   3 restart      the writer client is killed (SIGKILL) mid-write and
#                  remounted; writers resume
#   4 undecided    a unit parks after its PREPAREs (EFS_FAULT_PUB_BARRIER
#                  "prepare"): readers must still read the last published
#                  version, before and across a leader change; released, the
#                  new version becomes visible
#   5 committed    a unit parks after DECIDE COMMIT, before RESOLVE: readers
#                  read the old version or are refused (EBUSY), never a mix
#   6 aborted      units are aborted after their PREPAREs: nothing new is
#                  visible; released, writers progress again
# Final: both files read back as one version >= the last published one.
# K14_PHASES selects phases (default "1 2 3 4 5 6"); "1" alone runs on any
# build, so the same workload is a control against a build without W77.
# Prints PASS/FAIL per check; exit 1 on any FAIL. Raw logs stay in DIR/logs
# (outside Git). Cleanup: `stop`, then delete DIR.
set -u
cmd=$1; REL=$2; D=$3; PORT=${4:-18800}; FREL=${5:-$REL}
ADDR=127.0.0.1
export EFS_TRANSPORT=tcp EFS_MD_RAFT_N=3
export EFS_FAULT_PUB_BARRIER="$D/fault" EFS_RAFT_OBS=1
PHASES=" ${K14_PHASES:-1 2 3 4 5 6} "
on() { case "$PHASES" in *" $1 "*) return 0;; esac; return 1; }
HERE=$(cd "$(dirname "$0")/.." && pwd)
MIB=$((1<<20)); KIB64=65536; RLEN=$((512<<10))
WL="$D/k14wl"
FAILS=0
check() { if eval "$2"; then echo "PASS $1"; else echo "FAIL $1"; FAILS=$((FAILS+1)); fi; }

start_node() {
  local i=$1 join=""
  [ "$i" -gt 1 ] && join="--join $ADDR:$((PORT+1))"
  nohup "$FREL/efsd" --node-id "$i" --addr "$ADDR" --port $((PORT+i)) --storage "$D/n$i" \
    --quota 4G $join >> "$D/logs/efsd-n$i.log" 2>&1 &
  echo $! > "$D/logs/n$i.pid"
  for _ in $(seq 120); do grep -q listening "$D/logs/efsd-n$i.log" && return 0
    kill -0 "$(cat "$D/logs/n$i.pid")" 2>/dev/null || return 1; sleep 0.5; done
  return 1
}
stop_nodes() {
  for i in 1 2 3; do kill "$(cat "$D/logs/n$i.pid" 2>/dev/null)" 2>/dev/null; done
  for _ in $(seq 60); do
    alive=0; for i in 1 2 3; do kill -0 "$(cat "$D/logs/n$i.pid" 2>/dev/null)" 2>/dev/null && alive=1; done
    [ $alive = 0 ] && return; sleep 0.5; done
  for i in 1 2 3; do kill -9 "$(cat "$D/logs/n$i.pid" 2>/dev/null)" 2>/dev/null; done
}
mount_client() { # A|B
  mkdir -p "$D/mnt$1"
  (cd "$REL" && EFS_FUSE_LOG="$D/logs/fuse$1.log" ./scripts/client.sh "$ADDR:$((PORT+1))" "$D/mnt$1") \
    >> "$D/logs/client$1.out" 2>&1 && findmnt -rn "$D/mnt$1" > /dev/null
}
umount_client() { (cd "$REL" && ./scripts/client.sh stop "$@") >> "$D/logs/client.out" 2>&1; }
# The efs-fuse serving one of OUR mounts (owned by us, argv names the path).
fuse_pid() { pgrep -u "$(id -u)" -f "efs-fuse .* $D/mnt$1( |\$)" | head -1; }
step_down_all() {
  for g in 0 2; do for i in 1 2 3; do
    "$REL/efs-mgmt" raft-step-down "$ADDR:$((PORT+i))" $g >> "$D/logs/stepdown.out" 2>&1
  done; done
}
last_published() { grep '^published' "$1" 2>/dev/null | tail -1 | awk '{print $2}'; }
start_writers() { # SECONDS -> pids in WPIDS
  local first
  WPIDS=""
  first=$(( $(last_published "$D/logs/w-aligned.log" || echo 0) + 1000 ))
  "$WL" writer "$D/mntA/aligned.dat" 0 $MIB "$1" "$D/logs/w-aligned.log" $first &
  WPIDS="$WPIDS $!"
  first=$(( $(last_published "$D/logs/w-straddle.log" || echo 0) + 1000 ))
  "$WL" writer "$D/mntA/straddle.dat" $KIB64 $MIB "$1" "$D/logs/w-straddle.log" $first &
  WPIDS="$WPIDS $!"
}
start_readers() { # PHASE SECONDS [FILES...] -> pids in RPIDS
  local phase=$1 secs=$2 n=0; shift 2
  [ $# = 0 ] && set -- aligned straddle
  RPIDS=""
  for f in "$@"; do
    local off=0; [ "$f" = straddle ] && off=$KIB64
    for r in 1 2 3; do n=$((n+1))
      "$WL" reader "$D/mntB/$f.dat" $off $MIB $RLEN "$secs" $((n*7919+${#phase})) \
        > "$D/logs/$phase-reader-$f-$r.out" 2> "$D/logs/$phase-reader-$f-$r.err" &
      RPIDS="$RPIDS $!"
    done
  done
}
wait_all() { local rc=0; for p in $1; do wait "$p" || rc=1; done; return $rc; }
torn_total() { cat "$D"/logs/"$1"-reader-*.out 2>/dev/null | sed -n 's/.* torn=\([0-9]*\).*/\1/p' | awk '{s+=$1} END {print s+0}'; }
reads_total() { cat "$D"/logs/"$1"-reader-*.out 2>/dev/null | sed -n 's/.* reads=\([0-9]*\).*/\1/p' | awk '{s+=$1} END {print s+0}'; }
changes_total() { cat "$D"/logs/"$1"-reader-*.out 2>/dev/null | sed -n 's/.* changes=\([0-9]*\).*/\1/p' | awk '{s+=$1} END {print s+0}'; }
versions_of() { cat "$D"/logs/"$1"-reader-"$2"-*.out | sed -n 's/.* versions=\([0-9]*\)\.\.\([0-9]*\).*/\1 \2/p'; }
summary() { echo "  $1: reads=$(reads_total "$1") torn=$(torn_total "$1") version_changes=$(changes_total "$1")"; }

case $cmd in
run)
  for i in 1 2 3; do ss -ltnH "sport = :$((PORT+i))" | grep -q . && { echo "port $((PORT+i)) in use"; exit 1; }; done
  [ -e "$D/n1" ] && { echo "$D not fresh"; exit 1; }
  mkdir -p "$D"/{n1,n2,n3,logs,mntA,mntB}
  cc -O2 -Wall -Wextra -o "$WL" "$HERE/tests/live/k14_atomic_publication.c" || { echo "FAIL build workload"; exit 1; }
  echo "efsd $("$FREL/efsd" --version 2>&1 | head -1) | efs-fuse $("$REL/efs-fuse" --version 2>&1 | head -1)"
  for i in 1 2 3; do start_node $i || { echo "FAIL start n$i"; exit 1; }; done
  for _ in $(seq 30); do "$REL/efs-mgmt" raft-mkfs "$ADDR:$((PORT+1))" 2>&1 | grep -q 'raft-mkfs rc=0 ' && break; sleep 1; done
  mount_client A || { echo "FAIL mount A"; tail -5 "$D/logs/clientA.out"; exit 1; }
  mount_client B || { echo "FAIL mount B"; tail -5 "$D/logs/clientB.out"; exit 1; }
  # Version 1 of both files before any reader starts.
  "$WL" writer "$D/mntA/aligned.dat" 0 $MIB 0.01 "$D/logs/w-aligned.log" 1
  "$WL" writer "$D/mntA/straddle.dat" $KIB64 $MIB 0.01 "$D/logs/w-straddle.log" 1
  check "initial versions published" '[ -n "$(last_published "$D/logs/w-aligned.log")" ] && [ -n "$(last_published "$D/logs/w-straddle.log")" ]'

  if on 1; then
  echo "phase 1: steady rewrites"
  start_writers 40; start_readers p1 40; wait_all "$WPIDS"; wait_all "$RPIDS"; summary p1
  check "p1 no torn read" '[ "$(torn_total p1)" = 0 ]'
  check "p1 readers observed new versions" '[ "$(changes_total p1)" -gt 10 ]'
  fi

  if on 2; then
  echo "phase 2: leader changes"
  start_writers 40; start_readers p2 40
  for _ in $(seq 12); do sleep 3; step_down_all; done
  wait_all "$WPIDS"; wait_all "$RPIDS"; summary p2
  echo "  step-downs accepted: $(grep -c '(stepped down)' "$D/logs/stepdown.out")"
  check "p2 leaders changed" '[ "$(grep -c "(stepped down)" "$D/logs/stepdown.out")" -gt 4 ]'
  check "p2 no torn read" '[ "$(torn_total p2)" = 0 ]'
  check "p2 readers observed new versions" '[ "$(changes_total p2)" -gt 5 ]'
  fi

  if on 3; then
  echo "phase 3: writer client killed mid-write and restarted"
  start_writers 15; start_readers p3 40
  sleep 6
  B3=$(last_published "$D/logs/w-aligned.log")
  pid=$(fuse_pid A); echo "  killing efs-fuse $pid serving $D/mntA"
  [ -n "$pid" ] && kill -9 "$pid"
  wait_all "$WPIDS"
  fusermount3 -uz "$D/mntA" 2>/dev/null
  mount_client A || { echo "FAIL remount A"; tail -5 "$D/logs/clientA.out"; }
  start_writers 15; wait_all "$WPIDS"; wait_all "$RPIDS"; summary p3
  check "p3 no torn read across the client restart" '[ "$(torn_total p3)" = 0 ]'
  check "p3 writers published after the restart" \
    '[ "$(last_published "$D/logs/w-aligned.log")" -gt "$B3" ]'
  fi

  if on 4; then
  echo "phase 4: undecided publication (parked after PREPARE)"
  L=$(last_published "$D/logs/w-aligned.log")
  echo prepare > "$D/fault"
  "$WL" writer "$D/mntA/aligned.dat" 0 $MIB 0.01 "$D/logs/w-aligned.log" $((L+1)) &
  WP=$!
  for _ in $(seq 50); do [ -e "$D/fault.ready" ] && break; sleep 0.1; done
  check "p4 a unit is parked undecided" '[ -e "$D/fault.ready" ]'
  start_readers p4 4 aligned; sleep 2; step_down_all; wait_all "$RPIDS"
  rm -f "$D/fault"; wait "$WP"; summary p4
  versions_of p4 aligned | sed 's/^/  undecided reads saw versions /'
  check "p4 no torn read" '[ "$(torn_total p4)" = 0 ]'
  check "p4 undecided: only the last published version $L was visible" \
    '! versions_of p4 aligned | awk -v l=$L "\$1!=l || \$2!=l {bad=1} END {exit !bad}"'
  sleep 1
  check "p4 released: the new version is the file" \
    '[ "$("$WL" check "$D/mntB/aligned.dat" 0 $MIB)" = "version $(last_published "$D/logs/w-aligned.log")" ]'
  fi

  if on 5; then
  echo "phase 5: committed, not yet resolved (parked after DECIDE)"
  L=$(last_published "$D/logs/w-aligned.log")
  echo commit > "$D/fault"
  "$WL" writer "$D/mntA/aligned.dat" 0 $MIB 0.01 "$D/logs/w-aligned.log" $((L+1)) &
  WP=$!
  for _ in $(seq 50); do [ -e "$D/fault.ready" ] && break; sleep 0.1; done
  check "p5 a unit is parked committed" '[ -e "$D/fault.ready" ]'
  start_readers p5 3 aligned; wait_all "$RPIDS"
  rm -f "$D/fault"; wait "$WP"; summary p5
  versions_of p5 aligned | sed 's/^/  committed-unresolved reads saw versions /'
  check "p5 no torn read" '[ "$(torn_total p5)" = 0 ]'
  check "p5 never the uncommitted mix: only versions $L or $((L+1))" \
    '! versions_of p5 aligned | awk -v l=$L "(\$1!=0 && \$1<l) || \$2>l+1 {bad=1} END {exit !bad}"'
  sleep 1
  check "p5 released: the committed version is the file" \
    '[ "$("$WL" check "$D/mntB/aligned.dat" 0 $MIB)" = "version $((L+1))" ]'
  fi

  if on 6; then
  echo "phase 6: aborted publications"
  L=$(last_published "$D/logs/w-aligned.log")
  echo abort > "$D/fault"
  "$WL" writer "$D/mntA/aligned.dat" 0 $MIB 4 "$D/logs/w-aligned.log" $((L+1)) &
  WP=$!
  start_readers p6 4 aligned; wait_all "$RPIDS"; wait "$WP"
  rm -f "$D/fault"; summary p6
  versions_of p6 aligned | sed 's/^/  reads during aborts saw versions /'
  check "p6 no torn read" '[ "$(torn_total p6)" = 0 ]'
  check "p6 nothing past the last published version $L became visible" \
    '! versions_of p6 aligned | awk -v l=$L "\$2>l {bad=1} END {exit !bad}"'
  check "p6 servers aborted units" 'grep -q "W77 fault: publication unit aborted" "$D"/logs/efsd-n*.log'
  "$WL" writer "$D/mntA/aligned.dat" 0 $MIB 2 "$D/logs/w-aligned.log" $((L+1000))
  check "p6 released: writers publish again" '[ "$(last_published "$D/logs/w-aligned.log")" -ge $((L+1000)) ]'
  fi

  echo "final"
  sleep 1
  for f in aligned straddle; do
    off=0; [ $f = straddle ] && off=$KIB64
    got=$("$WL" check "$D/mntB/$f.dat" $off $MIB); want=$(last_published "$D/logs/w-$f.log")
    echo "  $f: $got (last published $want)"
    check "final $f is one version, at least the last published" \
      '[ "${got%% *}" = version ] && [ "${got#version }" -ge "$want" ]'
  done
  grep -h "raft-obs-w77" "$D"/logs/efsd-n*.log | tail -3 | sed 's/^/  /'
  echo "  stage-busy refusals seen by readers: $(cat "$D"/logs/p5-reader-*.out | sed -n 's/.* errors=\([0-9]*\).*/\1/p' | awk '{s+=$1} END {print s+0}')"
  echo "RESULT fails=$FAILS"
  [ $FAILS = 0 ]
  ;;
stop)
  rm -f "$D/fault"
  umount_client "$D/mntB"; umount_client "$D/mntA"
  for m in A B; do mountpoint -q "$D/mnt$m" && umount_client --force-discard "$D/mnt$m"; done
  stop_nodes; echo stopped
  ;;
*) echo "usage: $0 run|stop REL DIR [PORT] [FAULT_REL]"; exit 2 ;;
esac
