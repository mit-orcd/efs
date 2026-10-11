#!/bin/bash
set -euo pipefail
out=/data1/efs/amd-round-20261010
src=/data1/efs/devel-amd-fault
export EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0
while [ ! -f "$out/round.exit" ] || [ ! -f "$out/fault-native.exit" ]; do sleep 2; done
[ "$(cat "$out/restore.exit")" = 0 ] # Independent gate; preserve failed acceptance separately.
[ "$(cat "$out/fault-native.exit")" = 0 ]
[ "$(cat "$out/pressure-current-probe.exit")" = 0 ]
restore() {
    local old_rc=$? restore_rc=0
    trap - EXIT
    if ! mountpoint -q /data1/efs/mnt2; then
        /data1/efs/src/scripts/client.sh 127.0.0.1:17432 /data1/efs/mnt2 >> "$out/pressure-restore.log" 2>&1 || restore_rc=1
    fi
    mountpoint -q /data1/efs/mnt2 && timeout 10 stat /data1/efs/mnt2 >> "$out/pressure-restore.log" 2>&1 || restore_rc=1
    echo "$restore_rc" > "$out/pressure-restore.exit"
    echo "$old_rc" > "$out/pressure.exit"
    exit "$((old_rc || restore_rc))"
}
trap restore EXIT
/data1/efs/src/scripts/client.sh stop /data1/efs/mnt2 > "$out/pressure-borrow.log" 2>&1
cd "$src"
./efsd --version > "$out/pressure-build-identity.txt"
sha256sum efsd efs-fuse efs-mgmt > "$out/pressure-binary-sha256.txt"
python3 -u tests/live/compaction_pressure.py "$out/pressure-current" --probe "$out/pressure-current-probe" --mount /data1/efs/mnt2 > "$out/pressure.log" 2>&1
