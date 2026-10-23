#!/bin/bash
# gate_k9_append_load.sh run|cold|stop REL DIR PORT [MINUTES]
#
# K9/W87 oracle under background load, around append_oracle_cluster.sh
# (read its header). REL is a build tree with efsd, efs-fuse, efs-mgmt,
# scripts/ and tests/; DIR an absolute scratch root; PORT the base (daemons
# on PORT+1..PORT+3).
#
#   run REL DIR PORT MINUTES
#       starts the cluster with a 4 MiB client cache (EFS_DCACHE_BYTES),
#       a churn writer (create, rewrite and delete 128 KiB-1 MiB files on
#       the mount, driving physical GC) and a POSIX loop (posix_suite.py
#       --filter concurrent_appends, every fifth pass the full core suite
#       with --jobs 4), then runs oracle rounds until MINUTES pass,
#       rotating the shapes below, stopping at the first failure (the file
#       is kept under DIR/mnt, round JSON under DIR/logs). Every fourth
#       round's cold step restarts all three daemons, the others remount.
#   cold REL DIR PORT remount|restart
#       the oracle's --cold-cmd: pauses churn and POSIX at their next
#       iteration boundary, runs the cold step, resumes them.
#   stop REL DIR PORT
#       stops the background loops, the mount and the three daemons.
set -u
cmd=$1; REL=$2; D=$3; PORT=$4
HERE=$(cd "$(dirname "$0")" && pwd)
CL="$HERE/append_oracle_cluster.sh"
export EFS_DCACHE_BYTES=${EFS_DCACHE_BYTES:-4194304}
say() { echo "[k9 $(date -u +%FT%TZ)] $*" | tee -a "$D/logs/gate.log"; }

churn() {
  local i=0 d="$D/mnt/churn"
  mkdir -p "$d"
  while [ ! -f "$D/bg.stop" ]; do
    if [ -f "$D/bg.pause" ]; then touch "$D/churn.paused"; sleep 1; continue; fi
    rm -f "$D/churn.paused"
    i=$((i + 1))
    python3 - "$d" "$i" <<'PY' 2>> "$D/logs/churn.err" || echo "$(date -u +%T) iteration $i rc=$?" >> "$D/logs/churn.err"
import os, random, sys
d, i = sys.argv[1], int(sys.argv[2]); r = random.Random(i)
for k in range(8):
    p = os.path.join(d, "c%02d" % r.randrange(32))
    with open(p, "wb") as f: f.write(os.urandom(r.randrange(128, 1025) * 1024))
for k in range(4):
    p = os.path.join(d, "c%02d" % r.randrange(32))
    try: os.unlink(p)
    except FileNotFoundError: pass
PY
  done
  touch "$D/churn.paused"
}
posix() {
  local i=0 rc
  while [ ! -f "$D/bg.stop" ]; do
    if [ -f "$D/bg.pause" ]; then touch "$D/posix.paused"; sleep 1; continue; fi
    rm -f "$D/posix.paused"
    i=$((i + 1)); mkdir -p "$D/mnt/posix"
    if [ $((i % 5)) = 0 ]; then
      timeout 900 python3 "$REL/tests/posix/posix_suite.py" "$D/mnt/posix" --jobs 4 \
        --results "$D/logs/posix-$i.json" > "$D/logs/posix-$i.log" 2>&1
    else
      timeout 600 python3 "$REL/tests/posix/posix_suite.py" "$D/mnt/posix" --filter concurrent_appends \
        --results "$D/logs/posix-$i.json" > "$D/logs/posix-$i.log" 2>&1
    fi
    rc=$?; echo "$(date -u +%T) posix $i rc=$rc" >> "$D/logs/posix.rc"
  done
  touch "$D/posix.paused"
}

case $cmd in
run)
  MIN=$5
  mkdir -p "$D/logs"; rm -f "$D"/bg.* "$D"/*.paused
  bash "$CL" start "$REL" "$D" "$PORT" || exit 1
  churn & echo $! > "$D/logs/churn.pid"
  posix & echo $! > "$D/logs/posix.pid"
  end=$(( $(date +%s) + MIN * 60 )); r=0; recs=0
  shapes=("--writers 8 --records 40 --mode perrecord"
          "--writers 12 --records 60 --mode mixed --fsync-every 7"
          "--writers 16 --records 60 --mode persist"
          "--writers 4 --records 60 --mode persist --pad-max 3000"
          "--writers 16 --records 100 --mode mixed --gap-us 200")
  while [ "$(date +%s)" -lt "$end" ]; do
    r=$((r + 1)); s=${shapes[$(( (r - 1) % ${#shapes[@]} ))]}
    step=remount; [ $((r % 4)) = 0 ] && step=restart
    say "round $r shape: $s cold=$step"
    if ! python3 "$REL/tests/live/append_oracle.py" --mount "$D/mnt" --name "k09-r$r" --keep \
         --out "$D/logs/round-$r.json" --cold-cmd "bash $0 cold $REL $D $PORT $step" $s \
         >> "$D/logs/rounds.log" 2>&1; then
      touch "$D/bg.stop"; say "FAIL at round $r (file kept, see logs/round-$r.json and rounds.log)"; exit 1
    fi
    n=$(python3 -c "import json,sys; j=json.load(open(sys.argv[1])); print(j.get('acked', j.get('acked_records', 0)))" "$D/logs/round-$r.json" 2>/dev/null)
    recs=$((recs + ${n:-0})); say "round $r PASS acked=$n total_acked=$recs"
    rm -f "$D/mnt/k09-r$r"
  done
  touch "$D/bg.stop"; say "DONE rounds=$r acked=$recs (cluster left running: run stop)"
  ;;
cold)
  rm -f "$D/churn.paused" "$D/posix.paused"; touch "$D/bg.pause"
  for t in $(seq 900); do [ -f "$D/churn.paused" ] && [ -f "$D/posix.paused" ] && break; sleep 1; done
  rc=0; bash "$CL" "$5" "$REL" "$D" "$PORT" || rc=$?
  rm -f "$D/bg.pause"
  exit $rc
  ;;
stop)
  rm -f "$D/churn.paused" "$D/posix.paused"; touch "$D/bg.stop"
  for t in $(seq 900); do   # loops that already exited (run finished) leave no flag
    alive=0
    for f in churn posix; do
      p=$(cat "$D/logs/$f.pid" 2>/dev/null)
      [ -n "$p" ] && kill -0 "$p" 2>/dev/null && [ ! -f "$D/$f.paused" ] && alive=1
    done
    [ $alive = 0 ] && break; sleep 1
  done
  bash "$CL" stop "$REL" "$D" "$PORT"
  ;;
*) echo "usage: $0 run|cold|stop REL DIR PORT [MINUTES|remount|restart]"; exit 2 ;;
esac
