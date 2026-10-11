#!/bin/bash
set -euo pipefail
root=/data1/efs/amd-round-20261010
out=$root/adopt-fault
src=/data1/efs/devel-amd-fault
mkdir -p "$out"
export EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0
while [ ! -f "$root/w94-ab/round.exit" ]; do sleep 3; done
[ "$(cat "$root/w94-ab/restore.exit")" = 0 ]
[ "$(cat "$root/adopt-fault-build.exit")" = 0 ]
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
set +e
make -j8 EFS_FAULTS=1 EFS_GIT_ID=92fd2d62-combined-fault EFS_GIT_BRANCH=devel-amd EFS_GIT_DESCRIBE=92fd2d62-combined-fault all test > "$out/native.log" 2>&1
native_rc=$?
set -e
echo "$native_rc" > "$out/native.exit"
[ "$native_rc" = 0 ]

./efsd --version > "$out/build-identity.txt"
sha256sum efsd efs-fuse efs-mgmt libefs.a > "$out/binary-sha256.txt"
for m in /data1/efs/mnt /data1/efs/mnt2; do
    /data1/efs/src/scripts/client.sh stop "$m" >> "$out/borrow.log" 2>&1
done
for mode in direct buffered; do
    python3 -u tests/live/gc_reclamation.py --mode "$mode" --transport tcp --root "$out" --root "$out" --port 24190 --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 --fuse-reply-failure --create-live-retry --create-adopt-reply-loss --unique-create --guarded-leases --create-lease --create-negative-replay --posix --continue-regression-failure > "$out/$mode.log" 2>&1
    echo 0 > "$out/$mode.exit"
done
