#!/bin/bash
# Task AR: validate the ar_over counter by forcing one lost PUT reply per
# fragment (EFS_FAULT_PUT_REPLY_COLLISION=1 on the mount, fault build).
# Usage: bash scripts/evidence/ar/ar_collision_check.sh FAULT_SRC WORK
set -euo pipefail
SRC=$(cd "$1" && pwd)
WORK=$2
PORTS=(19731 19732 19733)
RUN=$WORK/run
rm -rf "$RUN"
mkdir -p "$RUN"/{c,mnt}
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
  ok=1
  for i in 1 2 3; do grep -q 'formed: 3 members' "$RUN/c/n$i.log" || ok=0; done
  [ "$ok" -eq 1 ] && break
  sleep 1
done
[ "$ok" -eq 1 ] || { echo 'cluster never formed'; exit 1; }
EFS_FAULT_PUT_REPLY_COLLISION=1 "$SRC/efs-fuse" 127.0.0.1:${PORTS[0]} "$RUN/mnt" -f >"$RUN/mnt.log" 2>&1 &
for t in $(seq 1 60); do mountpoint -q "$RUN/mnt" && break; sleep 1; done
mountpoint -q "$RUN/mnt" || { echo 'mount failed'; tail -5 "$RUN/mnt.log"; exit 1; }
for k in 1 2 3 4; do
  dd if=/dev/urandom of="$RUN/mnt/f$k" bs=4096 count=1 2>/dev/null
  python3 -c "import os; fd=os.open('$RUN/mnt/f$k', os.O_RDONLY); os.fsync(fd); os.close(fd)"
done
sleep 6
grep -c 'put-reply fault' "$RUN/mnt.log" || true
for i in 1 2 3; do grep 'iostats:' "$RUN/c/n$i.log" | tail -1; done
