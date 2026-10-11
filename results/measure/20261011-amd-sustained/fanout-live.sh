#!/bin/bash
set -euo pipefail
src=/data1/efs/devel-amd-gc-fanout-20261011
out=/data1/efs/amd-gc-fanout-20261011/live
mkdir -p "$out"
export EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_GC_BACKLOG_IDLE_MS=100
restore() {
 local rc=$? restored=0
 trap - EXIT
 for m in /data1/efs/mnt /data1/efs/mnt2; do
  if ! mountpoint -q "$m"; then
   /data1/efs/src/scripts/client.sh 127.0.0.1:17432 "$m" >> "$out/restore.log" 2>&1 || restored=1
  fi
  mountpoint -q "$m" && timeout 10 stat "$m" >> "$out/restore.log" 2>&1 || restored=1
 done
 echo "$rc" > "$out/round.exit"
 echo "$restored" > "$out/restore.exit"
 exit "$((rc || restored))"
}
trap restore EXIT
cd "$src"
./efsd --version > "$out/build-identity.txt"
sha256sum efsd efs-fuse efs-mgmt libefs.a > "$out/binary-sha256.txt"
for m in /data1/efs/mnt /data1/efs/mnt2; do
 /data1/efs/src/scripts/client.sh stop "$m" >> "$out/borrow.log" 2>&1
done
python3 -u tests/live/gc_reclamation.py --mode direct --transport tcp \
 --root "$out" --root "$out" --port 24190 --fixture-quota 4G \
 --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 \
 --guarded-leases --posix > "$out/direct-functional.log" 2>&1
echo 0 > "$out/direct-functional.exit"
failed=0
for mode in direct buffered; do
 set +e
 python3 -u tests/live/gc_reclamation.py --mode "$mode" --transport tcp \
  --root "$out" --root "$out" --port 24190 --fixture-quota 4G \
  --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 \
  --tiny-sustained-seconds 180 --tiny-sustained-large-mib-s 2 > "$out/$mode.log" 2>&1
 rc=$?
 set -e
 echo "$rc" > "$out/$mode.exit"
 [ "$rc" = 0 ] || failed=1
done
exit "$failed"
