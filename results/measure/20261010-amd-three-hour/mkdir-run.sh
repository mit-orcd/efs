#!/bin/bash
set -euo pipefail
src=/data1/efs/devel-amd
out=/data1/efs/amd-round-20261010
export EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0
cd "$src"
while [ ! -f "$out/pressure.exit" ]; do sleep 2; done
[ "$(cat "$out/pressure-restore.exit")" = 0 ]
export EFS_OPID_TRACE=1 EFS_RPC_PROF=1
# Keep the installed stores and binaries. Only the two policy-allowed mounts
# are borrowed, using safe drain; restoration is attempted even on gate failure.
restore() {
    local old_rc=$? restore_rc=0
    trap - EXIT
    for m in /data1/efs/mnt /data1/efs/mnt2; do
        if ! mountpoint -q "$m"; then
            EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 /data1/efs/src/scripts/client.sh 127.0.0.1:17432 "$m" >> "$out/mkdir-restore.log" 2>&1 || restore_rc=1
        fi
        mountpoint -q "$m" && timeout 10 stat "$m" >> "$out/mkdir-restore.log" 2>&1 || restore_rc=1
    done
    echo "$restore_rc" > "$out/mkdir-restore.exit"
    echo "$old_rc" > "$out/mkdir-round.exit"
    exit "$((old_rc || restore_rc))"
}
trap restore EXIT
for m in /data1/efs/mnt /data1/efs/mnt2; do
    /data1/efs/src/scripts/client.sh stop "$m" >> "$out/borrow-mounts.log" 2>&1
done
for mode in buffered direct; do
    python3 -u tests/live/gc_reclamation.py --mode "$mode" --transport tcp --root "$out" --root "$out" --port 24190 --mount /data1/efs/mnt --peer-mount /data1/efs/mnt2 --peer-mkdir-race-only > "$out/$mode-mkdir-race.log" 2>&1
    echo 0 > "$out/$mode-mkdir-race.exit"
done
