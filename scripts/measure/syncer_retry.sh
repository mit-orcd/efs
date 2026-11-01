#!/bin/bash
# Root-syncer retry measurement (K11 case, node 1's syncfs(root) refused).
# usage: syncer_retry.sh <built source tree> <out dir> <port> [seconds]
# Runs gc_reclamation.py --put-barrier-fault, freezes the driver once node 1's
# first barrier failed (the daemons and the marker keep running), then records
# for <seconds> each: top -H CPU of the daemon, refused-syncfs attempts per second
# (strace -f counts the shim's faccessat on .efs-durable-epoch, one per attempt;
# ptrace slows the thread, so the rate is a lower bound), and node 1's log growth.
set -u
SRC=$1 OUT=$2 PORT=$3 SECS=${4:-30}
W=$OUT/roots
mkdir -p "$W/p1" "$W/p2"
cd "$SRC" || exit 2
python3 tests/live/gc_reclamation.py --root "$W/p1" --root "$W/p2" --port "$PORT" \
    --put-barrier-fault > "$OUT/case.log" 2>&1 &
CASE=$!
trap 'kill -CONT $CASE 2>/dev/null' EXIT
LOG=
for _ in $(seq 1 3000); do
    LOG=$(ls "$W"/p1/gc-*/n1.log 2>/dev/null | head -1)
    [ -n "$LOG" ] && grep -q 'barrier failed' "$LOG" && break
    sleep 0.1
done
kill -STOP $CASE
grep -q 'barrier failed' "$LOG" || { echo "no barrier failure seen"; exit 1; }
PID=
for p in $(pgrep -x efsd); do
    tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null | grep -q -- "$W" &&
        tr '\0' '\n' < /proc/$p/environ | grep -qx EFS_TEST_STORE_NODE=1 && PID=$p
done
[ -n "$PID" ] || { echo "node 1 efsd not found"; exit 1; }
size0=$(stat -c %s "$LOG"); lines0=$(wc -l < "$LOG")
top -H -b -d "$SECS" -n 2 -p "$PID" > "$OUT/top.txt"
size1=$(stat -c %s "$LOG"); lines1=$(wc -l < "$LOG")
timeout -s INT $((SECS + 2)) strace -f -tt -e trace=faccessat,faccessat2 -p "$PID" -o "$OUT/strace.txt" &
sleep 1; sleep "$SECS"; pkill -INT -f "strace -f -tt -e trace=faccessat,faccessat2 -p $PID"; wait %2 2>/dev/null
calls=$(grep -c '.efs-durable-epoch' "$OUT/strace.txt")
{
  echo "pid=$PID window=${SECS}s"
  echo "log: $((lines1 - lines0)) lines, $((size1 - size0)) bytes in the top window ($(( (lines1 - lines0) / SECS )) lines/s, $(( (size1 - size0) / SECS )) B/s)"
  echo "refused syncfs attempts under strace: $calls in ${SECS}s ($((calls / SECS))/s)"
  echo "busiest thread, last top sample:"
  awk '/^top - /{n++} n==2' "$OUT/top.txt" | sed -n '7,10p'
} | tee "$OUT/summary.txt"
kill -CONT $CASE
wait $CASE; echo "case rc=$?" | tee -a "$OUT/summary.txt"
grep -c 'PASS' "$OUT/case.log" | sed 's/^/case PASS lines: /' | tee -a "$OUT/summary.txt"
