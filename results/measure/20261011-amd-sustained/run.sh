#!/bin/bash
set -euo pipefail
src=/data1/efs/devel-amd-final-integration-20261011
out=/data1/efs/amd-sustained-20261011
mkdir -p "$out"
export EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0
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
for mode in direct buffered; do
 set +e
 python3 -u tests/live/gc_reclamation.py --mode "$mode" --transport tcp \
  --root "$out" --port 24190 --fixture-quota 4G \
  --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 \
  --tiny-sustained-seconds 600 > "$out/$mode.log" 2>&1
 rc=$?
 set -e
 echo "$rc" > "$out/$mode.exit"
 [ "$rc" = 0 ] || exit "$rc"
done
