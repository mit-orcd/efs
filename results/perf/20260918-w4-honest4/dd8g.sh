#!/bin/bash
# W4: 8 GiB dd+fsync from 4 then 9 clients, each to its own file.
# Non-zero source (/tmp/src8g all-0x5a). Flush is in the clock.
set -u
SSH="${EFS_SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
MNT=/tmp/efs-mount
OUT=/home/erbmi1/git/efs/results/perf/20260918-w4-honest4
H4=(fcstor007 fcstor008 fcstor009 fcstor010)
H9=(fcstor007 fcstor008 fcstor009 fcstor010 fcstor011 fcstor012 fcstor013 fcstor014 fcstor015)
mkdir -p "$OUT"

ssh_to() {
    local t=$1 h=$2; shift 2
    EFS_SSH_TIMEOUT=$t "$SSH" "${h}.ib" "$@"
}

remount_one() {
    local h=$1
    ssh_to 25 "$h" "killall -9 efs-fuse 2>/dev/null || true
        timeout 3 fusermount3 -uz $MNT 2>/dev/null || true
        cd /tmp/efs && mkdir -p $MNT && rm -f fuse.log
        EFS_TRANSPORT=tcp setsid ./efs-fuse 172.16.223.57:19810 efs-test $MNT >fuse.log 2>&1 </dev/null &
        for i in \$(seq 1 40); do
            grep -q \"efs-fuse $MNT \" /proc/mounts && break
            sleep 0.15
        done
        grep -q \"efs-fuse $MNT \" /proc/mounts || { echo remount-fail; tail -5 fuse.log; exit 1; }
        for i in \$(seq 1 40); do
            ls -d $MNT >/dev/null 2>&1 && exit 0
            sleep 0.15
        done
        echo lookup-fail; exit 1"
}

ensure_src() {
    local h=$1
    ssh_to 60 "$h" 'python3 - <<"PY"
import os
p="/tmp/src8g"
want=8589934592
if os.path.exists(p) and os.path.getsize(p)==want:
    print("SRC8G_OK")
    raise SystemExit(0)
b=b"\x5a"*1024*1024
with open(p,"wb") as f:
    for _ in range(8192):
        f.write(b)
print("SRC8G_MADE")
PY'
}

run_dd() {
    local label=$1; shift
    local hosts=("$@")
    echo "=== ${label}-client 8g dd+fsync ==="
    local h pids=()
    for h in "${hosts[@]}"; do
        remount_one "$h" >/dev/null || { echo "remount failed $h"; return 1; }
        ensure_src "$h" >/dev/null || { echo "src failed $h"; return 1; }
    done
    for h in "${hosts[@]}"; do
        ssh_to 400 "$h" "
            fst=\$(findmnt -n -o FSTYPE $MNT 2>/dev/null || true)
            echo FUSE_CHECK fstype=\$fst
            echo \$fst | grep -q '^fuse\\.efs-fuse\$' || { echo NOT_FUSE; exit 1; }
            echo FUSE_OK
            rm -f $MNT/w4-dd-${h}.bin
            TIMEFORMAT='WALL %R'
            { time dd if=/tmp/src8g of=$MNT/w4-dd-${h}.bin bs=1M conv=fsync status=none; } 2>&1
            ls -l $MNT/w4-dd-${h}.bin
        " >"$OUT/dd8g-${#hosts[@]}-${h}.txt" &
        pids+=($!)
    done
    local p
    for p in "${pids[@]}"; do wait "$p"; done
    for h in "${hosts[@]}"; do
        echo "--- $h ---"
        grep -E 'FUSE_OK|NOT_FUSE|WALL | copied|w4-dd-' "$OUT/dd8g-${#hosts[@]}-${h}.txt" | head -8
    done
}

run_dd 4 "${H4[@]}"
run_dd 9 "${H9[@]}"
echo DONE_DD "$OUT"
