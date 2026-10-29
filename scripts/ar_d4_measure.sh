#!/bin/bash
# Task AR measurement: D4 small-file load against a private loopback cluster
# of the TEMP-AR instrumented production build (ar_over/ar_bytes in the
# daemon iostats lines). Usage: bash scripts/ar_d4_measure.sh SRC WORK [LOAD_S]
set -euo pipefail
SRC=$(cd "$1" && pwd)
WORK=$2
LOAD_S=${3:-120}
PORTS=(19711 19712 19713)
RUN=$WORK/run
rm -rf "$RUN"
mkdir -p "$RUN"/{c,mnt,out}
for i in 1 2 3; do
  mkdir -p "$RUN/c/n$i"
  if [ "$i" -eq 1 ]; then
    "$SRC/efsd" 127.0.0.1:${PORTS[0]} "$RUN/c/n$i" >"$RUN/c/n$i.log" 2>&1 &
  else
    "$SRC/efsd" --join 127.0.0.1:${PORTS[0]} --listen 127.0.0.1:${PORTS[$((i-1))]} "$RUN/c/n$i" >"$RUN/c/n$i.log" 2>&1 &
  fi
done
cleanup() {
  fusermount3 -u "$RUN/mnt" 2>/dev/null || true
  sleep 1
  pkill -f "$SRC/efsd" 2>/dev/null || true
  wait 2>/dev/null || true
}
trap cleanup EXIT
for t in $(seq 1 60); do
  if grep -q 'formed: 3 members' "$RUN/c/n1.log" && grep -q 'formed: 3 members' "$RUN/c/n2.log" && grep -q 'formed: 3 members' "$RUN/c/n3.log"; then break; fi
  sleep 1
done
grep -q 'formed: 3 members' "$RUN/c/n1.log" || { echo 'cluster never formed'; exit 1; }
"$SRC/efs-fuse" 127.0.0.1:${PORTS[0]} "$RUN/mnt" -f >"$RUN/mnt.log" 2>&1 &
FUSE_PID=$!
for t in $(seq 1 60); do mountpoint -q "$RUN/mnt" && break; sleep 1; done
mountpoint -q "$RUN/mnt" || { echo 'mount failed'; tail -5 "$RUN/mnt.log"; exit 1; }
# iostats counters are cumulative per daemon; snapshot the last line before
# the load and after it, per node.
for i in 1 2 3; do grep 'iostats:' "$RUN/c/n$i.log" | tail -1 >"$RUN/c/n$i.before" || true; done
cd "$SRC"
python3 scripts/d4_progress.py --mount "$RUN/mnt" --seed 127.0.0.1:${PORTS[0]} \
  --load-s "$LOAD_S" --threads 8 --out "$RUN/out" >"$RUN/d4.log" 2>&1 || true
sleep 6  # let one more iostats line land
for i in 1 2 3; do grep 'iostats:' "$RUN/c/n$i.log" | tail -1 >"$RUN/c/n$i.after" || true; done
echo '[ar-d4] verdict:'; tail -3 "$RUN/d4.log" | cut -c1-200
for i in 1 2 3; do
  echo "[ar-d4] n$i before: $(cat "$RUN/c/n$i.before")"
  echo "[ar-d4] n$i after:  $(cat "$RUN/c/n$i.after")"
done
ls "$RUN/out" | head -5
