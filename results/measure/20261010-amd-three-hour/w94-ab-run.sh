#!/bin/bash
set -euo pipefail
root=/data1/efs/amd-round-20261010
out=$root/w94-ab
mkdir -p "$out"
export EFS_TRANSPORT=tcp EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0
while [ ! -f "$root/candidate/round.exit" ]; do sleep 3; done
[ "$(cat "$root/candidate/round.exit")" = 0 ]
[ "$(cat "$root/candidate/restore.exit")" = 0 ]
[ "$(cat "$root/ab-baseline-build.exit")" = 0 ]
restore() {
    local old_rc=$? restore_rc=0
    trap - EXIT
    if ! mountpoint -q /data1/efs/mnt; then
        /data1/efs/src/scripts/client.sh 127.0.0.1:17432 /data1/efs/mnt >> "$out/restore.log" 2>&1 || restore_rc=1
    fi
    mountpoint -q /data1/efs/mnt && timeout 10 stat /data1/efs/mnt >> "$out/restore.log" 2>&1 || restore_rc=1
    echo "$restore_rc" > "$out/restore.exit"
    echo "$old_rc" > "$out/round.exit"
    exit "$((old_rc || restore_rc))"
}
trap restore EXIT
/data1/efs/src/scripts/client.sh stop /data1/efs/mnt > "$out/borrow.log" 2>&1
for variant in separate combined; do
    src=/data1/efs/devel-amd
    [ "$variant" != combined ] || src=/data1/efs/devel-amd-adopt
    cd "$src"
    ./efsd --version > "$out/$variant-identity.txt"
    sha256sum efsd efs-fuse libefs.a > "$out/$variant-sha256.txt"
    mkdir -p "$out/trace-$variant"
    EFS_OPID_TRACE=1 python3 -u tests/live/gc_reclamation.py --mode direct --transport tcp --root "$out/trace-$variant" --root "$out/trace-$variant" --port 24190 --mount /data1/efs/mnt --tiny-create-only --tiny-mixed-files 64 > "$out/trace-$variant.log" 2>&1
    echo 0 > "$out/trace-$variant.exit"
done
for round in 1 2 3 4; do
    order='separate combined'
    [ $((round%2)) != 0 ] || order='combined separate'
    for mode in direct buffered; do
        for variant in $order; do
            src=/data1/efs/devel-amd
            [ "$variant" != combined ] || src=/data1/efs/devel-amd-adopt
            cd "$src"
            case_dir="$out/r$round-$mode-$variant"
            mkdir -p "$case_dir"
            echo "$(date -u +%FT%TZ) start r$round $mode $variant" >> "$out/progress.log"
            python3 -u tests/live/gc_reclamation.py --mode "$mode" --transport tcp --root "$case_dir" --root "$case_dir" --port 24190 --mount /data1/efs/mnt --tiny-create-only --tiny-mixed-files 8192 > "$case_dir/run.log" 2>&1
            echo 0 > "$case_dir/exit-code"
            echo "$(date -u +%FT%TZ) pass r$round $mode $variant" >> "$out/progress.log"
        done
    done
done
