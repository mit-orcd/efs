#!/bin/bash
# w145_deposed_leader.sh RELEASE_DIR LABEL
#
# W145 reproduction: does a deposed Raft group leader catch up as a
# follower? Private 3-daemon RDMA cluster on node9901: ports PORT+1..PORT+3
# (default 17761-17763), stores under /tmp/efs-w145/LABEL. Never touches
# orcd-mini (17432-17434).
#
# Under a create load (16 writer threads, 256 B files), each round
# SIGSTOPs the current leader of group GROUP for FREEZE seconds and then
# SIGCONTs it. Another node takes over, so the frozen node returns as a
# deposed leader. The round passes when that node's applied index for
# GROUP is within LAG entries of the new leader's commit index within
# CATCHUP seconds. On a failure the new leader's raft-status (with W145
# per-peer progress) and the efsd log lines from W145 are printed.
# KEEP=1 leaves a failed cluster running for inspection.
#
# FREEZE_MODE=stop sends SIGSTOP/SIGCONT; FREEZE_MODE=perf instead slows
# the leader with `perf record -e cs --call-graph dwarf,16384` for FREEZE
# seconds, the capture that deposed orcd-mini's n2 (it keeps running, slowly).
# RESYNC_CHECK=1 first checks efs-mgmt raft-resync: refused on the group
# leader, accepted on a follower, and a healthy follower keeps following.
# Env: PORT (17760), GROUP (2), ROUNDS (3), FREEZE (3), FREEZE_MODE (stop),
# WARMUP (45), CATCHUP (90), LAG (2000), KEEP (0), RESYNC_CHECK (0).
# Exit 0 = every round caught up (PASS), 1 = a round did not (FAIL).
set -u
REL=$(cd "$1" && pwd); L=$2
PORT=${PORT:-17760}; GROUP=${GROUP:-2}; ROUNDS=${ROUNDS:-3}; FREEZE=${FREEZE:-3}
WARMUP=${WARMUP:-45}; CATCHUP=${CATCHUP:-90}; LAG=${LAG:-2000}; KEEP=${KEEP:-0}
FREEZE_MODE=${FREEZE_MODE:-stop}
AB=/tmp/efs-w145/$L
FUSE_LIBDIR=/data1/efs/deps/libfuse/lib64
for p in 1 2 3; do
  ss -ltn | grep -q ":$((PORT+p)) " && { echo "port $((PORT+p)) in use"; exit 2; }
done
rm -rf "$AB"; mkdir -p "$AB"/n1 "$AB"/n2 "$AB"/n3 "$AB"/logs "$AB"/mnt
# D39: the configured member list; the endpoint selects each member and
# efs-mgmt declares as management with it (EFS_CLUSTER_CONF).
CONF="$AB/cluster.conf"; : > "$CONF"
for i in 1 2 3; do echo "$i 10.1.99.1:$((PORT+i))" >> "$CONF"; done
export EFS_CLUSTER_CONF="$CONF"
[ -d "$FUSE_LIBDIR" ] && export LD_LIBRARY_PATH=$FUSE_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export EFS_TRANSPORT=rdma EFS_RDMA_DEV=${EFS_RDMA_DEV:-mlx5_0}
for i in 1 2 3; do
  nohup "$REL/efsd" --cluster "$CONF" --addr 10.1.99.1 --port $((PORT+i)) --storage "$AB/n$i" \
    --quota 30G --direct-io > "$AB/logs/efsd-n$i.log" 2>&1 &
  echo $! > "$AB/logs/n$i.pid"
  for t in $(seq 60); do grep -q listening "$AB/logs/efsd-n$i.log" && break; sleep 0.5; done
done
pid() { cat "$AB/logs/n$1.pid"; }
FAILED=0
cleanup() {
  for i in 1 2 3; do kill -CONT "$(pid $i)" 2>/dev/null; done
  if [ "$FAILED" = 1 ] && [ "$KEEP" = 1 ]; then
    echo "KEEP=1: cluster left running under $AB (ports $((PORT+1))-$((PORT+3)))"; return
  fi
  "$REL/scripts/client.sh" stop "$AB/mnt" > /dev/null 2>&1
  for i in 1 2 3; do kill "$(pid $i)" 2>/dev/null; done
  sleep 3; for i in 1 2 3; do kill -9 "$(pid $i)" 2>/dev/null; done
  rm -rf "$AB"/n1 "$AB"/n2 "$AB"/n3
}
trap cleanup EXIT
for t in $(seq 30); do
  up=$("$REL/efs-mgmt" status 10.1.99.1:$((PORT+1)) 2>&1 | grep -cE '^  node [0-9]+: .*  up ')
  [ "$up" -eq 3 ] && break; sleep 1
done
for t in $(seq 30); do "$REL/efs-mgmt" raft-mkfs 10.1.99.1:$((PORT+1)) 2>&1 | grep -q 'raft-mkfs rc=0 ' && break; sleep 1; done
EFS_FUSE_LOG="$AB/logs/fuse.log" "$REL/scripts/client.sh" 10.1.99.1:$((PORT+1)) "$AB/mnt" > "$AB/logs/client.out" 2>&1
findmnt -rn "$AB/mnt" > /dev/null || { echo "$L: mount failed"; tail -5 "$AB/logs/client.out"; exit 2; }
echo "== $L $("$REL/efsd" --version | head -1); group $GROUP, $ROUNDS rounds, $FREEZE_MODE ${FREEZE}s"

cat > "$AB/load.py" <<'EOF'
import os,sys,threading
d=sys.argv[1]; stop=sys.argv[2]; W=16; blk=b"x"*256; n=[0]*W; err=[0]*W
def writer(t):
    p=f"{d}/w{t}"; os.makedirs(p); i=0
    while not os.path.exists(stop):
        try:
            with open(f"{p}/f{i}","xb") as f: f.write(blk)
            n[t]+=1
        except OSError: err[t]+=1
        i+=1
th=[threading.Thread(target=writer,args=(t,)) for t in range(W)]
[x.start() for x in th]; [x.join() for x in th]
print("LOAD creates=%d errors=%d"%(sum(n),sum(err)))
EOF
python3 "$AB/load.py" "$AB/mnt/load" "$AB/logs/stop" > "$AB/logs/load.out" 2>&1 &
LP=$!

status() { "$REL/efs-mgmt" raft-status 10.1.99.1:$((PORT+$1)) 2>/dev/null; }
field() { # node group key -> value from that node's group line
  status "$1" | grep "^  group $2 " | grep -o " $3=[-0-9a-fx]*" | head -1 | cut -d= -f2; }
leader_node() { for i in 1 2 3; do status $i | grep -q "^  group $1 .*role=LEADER" && { echo $i; return; }; done; }

sleep "$WARMUP"
if [ "${RESYNC_CHECK:-0}" = 1 ]; then
  ld=$(leader_node "$GROUP"); fo=$(( ld % 3 + 1 ))
  out=$("$REL/efs-mgmt" raft-resync 10.1.99.1:$((PORT+ld)) "$GROUP" 2>&1)
  echo "-- resync on leader n$ld: $out"
  echo "$out" | grep -q "rc=-" || { echo "   FAIL: leader accepted a resync"; FAILED=1; }
  out=$("$REL/efs-mgmt" raft-resync 10.1.99.1:$((PORT+fo)) "$GROUP" 2>&1)
  echo "-- resync on follower n$fo: $out"
  echo "$out" | grep -q "rc=0" || { echo "   FAIL: follower refused a resync"; FAILED=1; }
  sleep 20
  status "$fo" | grep "^  group $GROUP " | grep -q " resync=1" || { echo "   FAIL: resync flag not shown"; FAILED=1; }
  lc=$(field "$(leader_node "$GROUP")" "$GROUP" commit); fa=$(field "$fo" "$GROUP" applied)
  echo "   healthy follower n$fo applied=$fa leader commit=$lc"
  [ $(( lc - fa )) -le "$LAG" ] || { echo "   FAIL: healthy follower fell behind after resync request"; FAILED=1; }
fi
for r in $(seq "$ROUNDS"); do
  old=$(leader_node "$GROUP")
  [ -z "$old" ] && { echo "round $r: no leader for group $GROUP"; FAILED=1; break; }
  before=$(field "$old" "$GROUP" commit)
  echo "-- round $r: freeze group-$GROUP leader n$old (commit=$before) for ${FREEZE}s"
  if [ "$FREEZE_MODE" = perf ]; then
    perf record -e cs --call-graph dwarf,16384 -p "$(pid "$old")" -o "$AB/logs/stall-$r.data" \
      -- sleep "$FREEZE" > /dev/null 2>&1 < /dev/null
    rm -f "$AB/logs/stall-$r.data"
  else
    kill -STOP "$(pid "$old")"; sleep "$FREEZE"; kill -CONT "$(pid "$old")"
  fi
  ok=0; t0=$(date +%s)
  while [ $(( $(date +%s) - t0 )) -lt "$CATCHUP" ]; do
    sleep 2
    new=$(leader_node "$GROUP"); [ -z "$new" ] && continue
    lc=$(field "$new" "$GROUP" commit); oa=$(field "$old" "$GROUP" applied)
    [ -z "$lc" ] || [ -z "$oa" ] && continue
    if [ "$new" != "$old" ] && [ $(( lc - oa )) -le "$LAG" ] && [ "$oa" -gt "$before" ]; then
      echo "   caught up: leader n$new commit=$lc, n$old applied=$oa after $(( $(date +%s) - t0 ))s"; ok=1; break
    fi
    if [ "$new" = "$old" ] && [ $(( $(date +%s) - t0 )) -gt 20 ]; then
      echo "   n$old kept or regained leadership; no deposition this round"; ok=1; break
    fi
  done
  if [ "$ok" != 1 ]; then
    FAILED=1
    echo "   FAIL: n$old did not catch up within ${CATCHUP}s (leader n${new:-?} commit=${lc:-?}, n$old applied=${oa:-?})"
    for i in 1 2 3; do echo "   -- n$i raft-status"; status $i | sed 's/^/      /'; done
    for i in 1 2 3; do
      echo "   -- n$i W145 log lines"
      grep -E "AE rejected|rejecting AE|AE conflict|inbox refused|became leader|steps down|vote |commit by non-leader" "$AB/logs/efsd-n$i.log" | tail -14 | sed 's/^/      /'
    done
    break
  fi
  sleep 20
done
touch "$AB/logs/stop"; wait $LP; cat "$AB/logs/load.out"
for i in 1 2 3; do echo "n$i: rejects/conflict lines $(grep -cE 'AE rejected|rejecting AE|AE conflict|inbox refused' "$AB/logs/efsd-n$i.log")"; done
[ "$FAILED" = 0 ] && { echo "W145 $L: PASS"; exit 0; }
echo "W145 $L: FAIL"; exit 1
