#!/bin/bash
set -euo pipefail
src=/data1/efs/devel-amd-merged-20261011
out=/data1/efs/amd-integration-20261011/live
mkdir -p "$out"
export EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0
[ "$(cat /data1/efs/amd-integration-20261011/fault-native.exit)" = 0 ]
restore() {
    local old_rc=$? restore_rc=0
    trap - EXIT
    for m in /data1/efs/mnt /data1/efs/mnt2; do
        if ! mountpoint -q "$m"; then
            /data1/efs/src/scripts/client.sh 127.0.0.1:17432 "$m" >> "$out/restore.log" 2>&1 || restore_rc=1
        fi
        mountpoint -q "$m" && timeout 10 stat "$m" >> "$out/restore.log" 2>&1 || restore_rc=1
    done
    echo "$restore_rc" > "$out/restore.exit"
    echo "$old_rc" > "$out/round.exit"
    exit "$((old_rc || restore_rc))"
}
trap restore EXIT
cd "$src"
./efsd --version > "$out/build-identity.txt"
sha256sum efsd efs-fuse efs-mgmt libefs.a > "$out/binary-sha256.txt"
for m in /data1/efs/mnt /data1/efs/mnt2; do
    /data1/efs/src/scripts/client.sh stop "$m" >> "$out/borrow.log" 2>&1
done
for mode in direct buffered; do
    python3 -u tests/live/gc_reclamation.py --mode "$mode" --transport tcp --root "$out" --root "$out" --port 24190 --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 --fuse-reply-failure --create-live-retry --create-adopt-reply-loss --create-adopt-negative --unique-create --guarded-leases --create-lease --create-negative-replay --posix --continue-regression-failure > "$out/$mode.log" 2>&1
    echo 0 > "$out/$mode.exit"
done
