#!/bin/bash
set -euo pipefail
src=/data1/efs/devel-amd-adopt
out=/data1/efs/amd-round-20261010/candidate
mkdir -p "$out"
export EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0
cd "$src"
[ "$(cat /data1/efs/amd-round-20261010/adopt-native.exit)" = 0 ]
[ "$(cat /data1/efs/amd-round-20261010/mkdir-restore.exit)" = 0 ]
sha256sum efsd efs-fuse efs-mgmt > "$out/binary-sha256.txt"
./efsd --version > "$out/build-identity.txt"
# Keep the installed stores and binaries. Only the two policy-allowed mounts
# are borrowed, using safe drain; restoration is attempted even on gate failure.
restore() {
    local old_rc=$? restore_rc=0
    trap - EXIT
    for m in /data1/efs/mnt /data1/efs/mnt2; do
        if ! mountpoint -q "$m"; then
            EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 /data1/efs/src/scripts/client.sh 127.0.0.1:17432 "$m" >> "$out/restore.log" 2>&1 || restore_rc=1
        fi
        mountpoint -q "$m" && timeout 10 stat "$m" >> "$out/restore.log" 2>&1 || restore_rc=1
    done
    echo "$restore_rc" > "$out/restore.exit"
    echo "$old_rc" > "$out/round.exit"
    exit "$((old_rc || restore_rc))"
}
trap restore EXIT
for m in /data1/efs/mnt /data1/efs/mnt2; do
    /data1/efs/src/scripts/client.sh stop "$m" >> "$out/borrow-mounts.log" 2>&1
done
for mode in buffered direct; do
    python3 -u tests/live/gc_reclamation.py --mode "$mode" --transport tcp --root "$out" --root "$out" --port 24190 --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 --posix --continue-regression-failure --sync-writes --append-audit --append-replay-race --guarded-leases --typed-writeback --create-negative-replay --create-lease > "$out/$mode-full.log" 2>&1
    echo 0 > "$out/$mode-full.exit"
done
for mode in buffered direct; do
    python3 -u tests/live/gc_reclamation.py --mode "$mode" --transport tcp --root "$out" --root "$out" --port 24190 --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 --mount-sync > "$out/$mode-mount-sync.log" 2>&1
    echo 0 > "$out/$mode-mount-sync.exit"
done
