#!/bin/bash
# k01_restart_elections.sh BUILD_DIR WORK_DIR [PORT]
#
# K1 / W145 part 3: count Raft elections across a staggered restart of a
# private 3-daemon TCP cluster (ports PORT+1..PORT+3, default 18101-18103;
# stores under WORK_DIR/n{1,2,3}; no FUSE mount). BUILD_DIR holds efsd and
# efs-mgmt of one build.
#
# 1. Start n1..n3, raft-mkfs, wait SETTLE s; record each group's term.
# 2. Stop all three (SIGTERM, then SIGKILL after STOP_WAIT s).
# 3. Restart n1, wait GAP s, n2, wait GAP s, n3 (orcd-mini's start.sh
#    staggered the three by 10-17 s while each ran its storage inventory).
# 4. Wait SETTLE s; record each group's term and leader, and the vote and
#    election log lines.
# Each election raises a group's term by one. PASS when every group's
# term rose by at most MAX_ELECTIONS (default 3) and all groups have one
# leader agreed by all three nodes.
# Env: GAP (15), SETTLE (10), STOP_WAIT (20), MAX_ELECTIONS (3), KEEP (0).
# Cleanup: the script stops its daemons on exit (KEEP=1 leaves them);
# delete WORK_DIR afterwards.
set -u
B=$(cd "$1" && pwd); D=$2; PORT=${3:-18100}
GAP=${GAP:-15}; SETTLE=${SETTLE:-10}; STOP_WAIT=${STOP_WAIT:-20}
MAX=${MAX_ELECTIONS:-3}; KEEP=${KEEP:-0}
ADDR=127.0.0.1
export EFS_TRANSPORT=tcp
for i in 1 2 3; do
  ss -ltnH "sport = :$((PORT+i))" | grep -q . && { echo "port $((PORT+i)) in use"; exit 2; }
done
mkdir -p "$D"/n1 "$D"/n2 "$D"/n3 "$D"/logs
# D39: the configured member list; the endpoint selects each member and
# efs-mgmt declares as management with it (EFS_CLUSTER_CONF).
CONF="$D/cluster.conf"; : > "$CONF"
for i in 1 2 3; do echo "$i $ADDR:$((PORT+i))" >> "$CONF"; done
export EFS_CLUSTER_CONF="$CONF"
start() {
  local i=$1
  nohup "$B/efsd" --cluster "$CONF" --addr $ADDR --port $((PORT+i)) --storage "$D/n$i" \
    --quota 2G >> "$D/logs/efsd-n$i.log" 2>&1 &
  echo $! > "$D/logs/n$i.pid"
}
stop_all() {
  for i in 1 2 3; do kill "$(cat "$D/logs/n$i.pid" 2>/dev/null)" 2>/dev/null; done
  for t in $(seq $((STOP_WAIT * 2))); do
    alive=0
    for i in 1 2 3; do kill -0 "$(cat "$D/logs/n$i.pid" 2>/dev/null)" 2>/dev/null && alive=1; done
    [ $alive = 0 ] && return 0
    sleep 0.5
  done
  echo "WARN: daemons outlived ${STOP_WAIT}s; SIGKILL"
  for i in 1 2 3; do kill -9 "$(cat "$D/logs/n$i.pid" 2>/dev/null)" 2>/dev/null; done
  return 1
}
[ "$KEEP" = 1 ] || trap stop_all EXIT
wait_listen() {
  for t in $(seq 120); do
    [ "$(grep -c listening "$D/logs/efsd-n$1.log" 2>/dev/null)" -ge "$2" ] && return 0
    sleep 0.5
  done
  echo "n$1 did not listen"; return 1
}
# status FILE: one line per node and group: node group role leader term
status() {
  : > "$1"
  for i in 1 2 3; do
    "$B/efs-mgmt" raft-status $ADDR:$((PORT+i)) 2>/dev/null |
      awk -v n="$i" '/^  group/ { for (k = 1; k <= NF; k++) { split($k, a, "=");
        v[a[1]] = a[2] } print n, $2, v["role"], v["leader"], v["term"] }' >> "$1"
  done
}
for i in 1 2 3; do start "$i"; wait_listen "$i" 1 || exit 2; done
for t in $(seq 30); do "$B/efs-mgmt" raft-mkfs $ADDR:$((PORT+1)) 2>&1 | grep -q 'raft-mkfs rc=0 ' && break; sleep 1; done
sleep "$SETTLE"
status "$D/before.txt"
stop_all
for i in 1 2 3; do echo "=== restart" >> "$D/logs/efsd-n$i.log"; done
start 1; wait_listen 1 2 || exit 2; sleep "$GAP"
start 2; wait_listen 2 2 || exit 2; sleep "$GAP"
start 3; wait_listen 3 2 || exit 2; sleep "$SETTLE"
status "$D/after.txt"
echo "before (node group role leader term):"; cat "$D/before.txt"
echo "after:"; cat "$D/after.txt"
for i in 1 2 3; do
  echo "n$i after restart: $(sed -n '/=== restart/,$p' "$D/logs/efsd-n$i.log" | grep -c 'raft: group=.* vote ') vote lines," \
       "$(sed -n '/=== restart/,$p' "$D/logs/efsd-n$i.log" | grep -c 'became leader') became-leader lines"
done
verdict=PASS
for g in $(awk '{print $2}' "$D/before.txt" | sort -u); do
  t0=$(awk -v g="$g" '$2 == g { if ($5 > m) m = $5 } END { print m + 0 }' "$D/before.txt")
  t1=$(awk -v g="$g" '$2 == g { if ($5 > m) m = $5 } END { print m + 0 }' "$D/after.txt")
  nl=$(awk -v g="$g" '$2 == g && $3 == "LEADER"' "$D/after.txt" | wc -l)
  nr=$(awk -v g="$g" '$2 == g' "$D/after.txt" | wc -l)
  agree=$(awk -v g="$g" '$2 == g { print $4 }' "$D/after.txt" | sort -u | wc -l)
  echo "group $g: term $t0 -> $t1 (elections $((t1 - t0))), leaders=$nl replicas=$nr leader-ids=$agree"
  [ $((t1 - t0)) -le "$MAX" ] && [ "$nl" = 1 ] && [ "$agree" = 1 ] || verdict=FAIL
done
echo "verdict: $verdict"
[ $verdict = PASS ]
