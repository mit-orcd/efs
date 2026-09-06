#!/bin/bash
# Scratch-cluster gate for the production Raft host (10.5c).
#
# Four efsd on fcstor003-006, port 19820, storage under /tmp — NEVER the
# live cluster (port 19810, /data1). Does not pkill -x efsd.
#
# Gate: elect, raft-mkfs lands ROOT on every voter, LOOKUP/GETATTR via
# ReadIndex + KV (leader serves ROOT, follower is NOT_PRIMARY, missing
# name is NOT_FOUND), file CREATE through Raft (lookup+getattr, duplicate
# is EXIST), SETATTR mode/owner, MKDIR as a 2-shard txn, last-link UNLINK
# of a file (lookup miss, second unlink NOT_FOUND), RMDIR as a 2-shard
# txn (empty LOCAL dir, lookup miss), LINK as a 2-shard txn (nlink=2,
# dest lookup, directory is INVAL), nlink>1 UNLINK of a dedicated extra
# name (lookup miss, surviving nlink=2), utimens mtime fence, same-dir
# LOCAL file RENAME (old name gone, new name present), READDIR of ROOT
# (created/renamed/link/mkdir names present, unlinked names absent),
# LOOKUP_PATH of those names, chunk-aligned SETATTR SIZE (truncate),
# chunk publish + GETCHUNKS (lane 0), unaligned truncate tail + trunc-0,
# kill -9 a follower
# then the leader, restart catch-up keeps ROOT, the created file (new
# mode, size=131072, nlink=2, utimens mtime), the published file truncated
# to size=0 with no chunk map,
# the mkdir, the extra link name, the
# unlinked name stays gone, the rmdir'd name stays gone, the nlink>1
# unlinked name stays gone, the renamed name stays, and READDIR /
# LOOKUP_PATH still match.
set -eu
SSH="${SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
PORT="${PORT:-19820}"
SEED="172.16.223.57:${PORT}"
SCRATCH=/tmp/efs-raft-scratch
SRC="$HOME/git/efs"
HOSTS=(fcstor003 fcstor004 fcstor005 fcstor006)
ADDRS=(172.16.223.57 172.16.223.58 172.16.223.59 172.16.223.60)
FAIL=0

ssh_to() {
    local t=$1 h=$2
    shift 2
    EFS_SSH_TIMEOUT=$t "$SSH" "${h}.ib" "$@"
}

say() { echo "[raft-host] $*"; }
bad() { echo "[raft-host] FAIL: $*"; FAIL=$((FAIL + 1)); }

# Kill only the scratch daemons (port 19820). Never pkill -x efsd.
kill_scratch() {
    local h
    for h in "${HOSTS[@]}"; do
        ssh_to 10 "$h" "
pids=\$(ps -eo pid,args | awk '/[e]fsd / && / --port ${PORT}( |\$)/ {print \$1}')
if [ -n \"\$pids\" ]; then kill -9 \$pids; fi
" >/dev/null 2>&1 || true
    done
}

cleanup() {
    kill_scratch
    local h
    for h in "${HOSTS[@]}"; do
        ssh_to 10 "$h" "rm -rf $SCRATCH" >/dev/null 2>&1 || true
    done
}
trap cleanup EXIT

build_one() {
    local h=$1
    ssh_to 60 "$h" "
set -e
mkdir -p /tmp/efs
rsync -a --delete --exclude='/mnt/' --exclude='*.log' '$SRC/' /tmp/efs/
cd /tmp/efs
make clean >/dev/null
make -j\"\$(nproc)\" efsd efs-mgmt tests/test_wire
./tests/test_wire
"
}

start_one() {
    local idx=$1
    local keep=${2:-}
    local h=${HOSTS[$idx]}
    local addr=${ADDRS[$idx]}
    local nid=$((idx + 1))
    local join=""
    if [ "$idx" -gt 0 ]; then
        join="--join $SEED"
    fi
    ssh_to 10 "$h" "
set -e
if [ '$keep' != keep ]; then
    rm -rf $SCRATCH
fi
mkdir -p $SCRATCH
rm -f /tmp/efs-raft-scratch.log
cd /tmp/efs
export EFS_MD_RAFT=1
setsid ./efsd --node-id $nid --addr $addr --port $PORT \
    --storage $SCRATCH --quota 1G --writers 0 --no-direct-io $join \
    >/tmp/efs-raft-scratch.log 2>&1 </dev/null &
"
}

wait_listen() {
    local h=$1
    local i
    for i in $(seq 1 40); do
        if ssh_to 5 "$h" "grep -q 'listening on' /tmp/efs-raft-scratch.log 2>/dev/null"; then
            return 0
        fi
    done
    return 1
}

g0_applied() {
    local idx=$1
    local st
    st=$(ssh_to 10 "${HOSTS[$idx]}" "cd /tmp/efs && ./efs-mgmt raft-status ${ADDRS[$idx]}:${PORT}" 2>/dev/null || true)
    echo "$st" | awk '/group 0 / {for(i=1;i<=NF;i++) if($i ~ /^applied=/) {split($i,a,"="); print a[2]; exit}}'
}

g0_commit() {
    local idx=$1
    local st
    st=$(ssh_to 10 "${HOSTS[$idx]}" "cd /tmp/efs && ./efs-mgmt raft-status ${ADDRS[$idx]}:${PORT}" 2>/dev/null || true)
    echo "$st" | awk '/group 0 / {for(i=1;i<=NF;i++) if($i ~ /^commit=/) {split($i,a,"="); print a[2]; exit}}'
}

# Wait until idx has applied through the leader's commit (log catch-up,
# not just ROOT in KV — CREATE/MKDIR sit after mkfs).
wait_g0_caught_up() {
    local idx=$1
    local tag=$2
    local lid want got i
    lid=$(g0_leader)
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader for catch-up"
        return 1
    fi
    want=$(g0_commit "$lid")
    for i in $(seq 1 50); do
        got=$(g0_applied "$idx")
        if [ -n "$got" ] && [ -n "$want" ] && [ "$got" -ge "$want" ] 2>/dev/null; then
            say "$tag caught up applied=$got"
            return 0
        fi
        lid=$(g0_leader)
        [ "$lid" != "-1" ] && want=$(g0_commit "$lid")
    done
    bad "$tag catch-up applied=$got want=$want"
    return 1
}

# Print group-0 leader raft id (-1 if none). Tries every voter; a just-
# restarted node may not know the leader yet even with ROOT in KV.
g0_leader() {
    local idx st l
    for idx in 0 1 2; do
        st=$(ssh_to 10 "${HOSTS[$idx]}" "cd /tmp/efs && ./efs-mgmt raft-status ${ADDRS[$idx]}:${PORT}" 2>/dev/null || true)
        l=$(echo "$st" | awk '/group 0 / {
            for (i = 1; i <= NF; i++)
                if ($i ~ /^leader=/) { sub(/^leader=/, "", $i); print $i; exit }
        }')
        if [ -n "$l" ] && [ "$l" != "-1" ]; then
            echo "$l"
            return 0
        fi
    done
    echo "-1"
}

# First group-0 voter that actually serves ROOT GETATTR (status=0).
# raft-status leader= can lag an election; inode RPCs are the truth.
g0_serving() {
    local idx out
    for idx in 0 1 2; do
        out=$(ssh_to 5 "${HOSTS[$idx]}" "cd /tmp/efs && ./efs-mgmt raft-getattr ${ADDRS[$idx]}:${PORT} 1" 2>/dev/null || true)
        if echo "$out" | grep -q 'status=0'; then
            echo "$idx"
            return 0
        fi
    done
    echo "-1"
}

root_on() {
    local idx=$1
    local st
    st=$(ssh_to 10 "${HOSTS[$idx]}" "cd /tmp/efs && ./efs-mgmt raft-status ${ADDRS[$idx]}:${PORT}" 2>/dev/null || true)
    echo "$st" | awk '/^node / {print $3}' | sed 's/root=//'
}

# efs-mgmt against the current group-0 leader. A restart can steal
# leadership between two RPCs (GETATTR OK, then LOOKUP NOT_PRIMARY);
# refresh and retry instead of treating 7 as a missing name.
g0_mgmt() {
    local cmd=$1
    local i out lid
    shift
    lid=${leader:-}
    for i in $(seq 1 8); do
        if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
            lid=$(g0_serving)
        fi
        if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
            lid=$(g0_leader)
        fi
        if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
            continue
        fi
        out=$(ssh_to 5 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt $cmd ${ADDRS[$lid]}:${PORT} $*" 2>/dev/null || true)
        if echo "$out" | grep -q 'status=7'; then
            lid=$(g0_serving)
            continue
        fi
        leader=$lid
        printf '%s\n' "$out"
        return 0
    done
    printf '%s\n' "${out:-}"
}

# ReadIndex GETATTR/LOOKUP against group-0. Leader must serve ROOT;
# a follower must refuse (NOT_PRIMARY=7); a missing name is NOT_FOUND=1.
# nlink is 2 at mkfs and 3 after a subdirectory (POSIX).
check_reads() {
    local lid=$1
    local tag=$2
    local nlink=${3:-2}
    local out fid rid
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    leader=$lid
    out=$(g0_mgmt raft-getattr 1)
    lid=$leader
    say "$tag getattr leader=$lid: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag getattr not OK"
    echo "$out" | grep -q "nlink=$nlink" || bad "$tag getattr nlink"
    echo "$out" | grep -q 'mode=040755' || bad "$tag getattr not dir"
    fid=""
    for rid in 0 1 2; do
        [ "$rid" = "$lid" ] && continue
        out=$(ssh_to 10 "${HOSTS[$rid]}" "cd /tmp/efs && ./efs-mgmt raft-getattr ${ADDRS[$rid]}:${PORT} 1" 2>/dev/null || true)
        say "$tag getattr follower=$rid: $out"
        if echo "$out" | grep -q 'status=7'; then
            fid=$rid
            break
        fi
    done
    [ -n "$fid" ] || bad "$tag no follower NOT_PRIMARY"
    out=$(g0_mgmt raft-lookup 1 no-such-efs-name)
    say "$tag lookup miss: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag lookup miss not NOT_FOUND"
}

# File CREATE on the group-0 leader.
check_create() {
    local lid=$1
    local tag=$2
    local out ino
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-create ${ADDRS[$lid]}:${PORT} 1 raft-smoke-f" 2>/dev/null || true)
    say "$tag create: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag create not OK"
    echo "$out" | grep -q 'mode=0100644' || bad "$tag create mode"
    echo "$out" | grep -q 'nlink=1' || bad "$tag create nlink"
    ino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$ino" ] && [ "$ino" != "0" ] || bad "$tag create ino"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-lookup ${ADDRS[$lid]}:${PORT} 1 raft-smoke-f" 2>/dev/null || true)
    say "$tag lookup created: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag lookup created not OK"
    echo "$out" | grep -q "ino=$ino" || bad "$tag lookup ino mismatch"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-getattr ${ADDRS[$lid]}:${PORT} $ino" 2>/dev/null || true)
    say "$tag getattr created: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag getattr created not OK"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-create ${ADDRS[$lid]}:${PORT} 1 raft-smoke-f" 2>/dev/null || true)
    say "$tag create dup: $out"
    echo "$out" | grep -q 'status=2' || bad "$tag create dup not EXIST"
}

# MKDIR is a 2-shard txn. The receiving leader must host every participant
# group; names that scatter onto the even group from a group-0-only leader
# are NOT_PRIMARY. Try several names until one lands.
check_mkdir() {
    local lid=$1
    local tag=$2
    local i out name got=""
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    for i in $(seq 0 31); do
        name="raft-smoke-d$i"
        out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-create ${ADDRS[$lid]}:${PORT} 1 $name 040755" 2>/dev/null || true)
        say "$tag mkdir $name: $out"
        if echo "$out" | grep -q 'status=0'; then
            got=$name
            echo "$out" | grep -q 'mode=040755' || bad "$tag mkdir mode"
            echo "$out" | grep -q 'nlink=2' || bad "$tag mkdir nlink"
            break
        fi
    done
    [ -n "$got" ] || bad "$tag mkdir none accepted"
    if [ -n "$got" ]; then
        out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-lookup ${ADDRS[$lid]}:${PORT} 1 $got" 2>/dev/null || true)
        say "$tag lookup mkdir: $out"
        echo "$out" | grep -q 'status=0' || bad "$tag lookup mkdir not OK"
        echo "$out" | grep -q 'mode=040755' || bad "$tag lookup mkdir mode"
        MKDIR_NAME=$got
    fi
}

# Last-link file UNLINK on the group-0 leader. Uses a dedicated name so
# raft-smoke-f still exists after crash.
check_unlink() {
    local lid=$1
    local tag=$2
    local out
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-create ${ADDRS[$lid]}:${PORT} 1 raft-smoke-u" 2>/dev/null || true)
    say "$tag unlink-prep create: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag unlink-prep create not OK"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-unlink ${ADDRS[$lid]}:${PORT} 1 raft-smoke-u" 2>/dev/null || true)
    say "$tag unlink: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag unlink not OK"
    echo "$out" | grep -q 'nlink=1' || bad "$tag unlink nlink"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-lookup ${ADDRS[$lid]}:${PORT} 1 raft-smoke-u" 2>/dev/null || true)
    say "$tag lookup unlinked: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag lookup unlinked not NOT_FOUND"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-unlink ${ADDRS[$lid]}:${PORT} 1 raft-smoke-u" 2>/dev/null || true)
    say "$tag unlink again: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag unlink again not NOT_FOUND"
}

# RMDIR is the same 2-shard txn as MKDIR. Dedicated name so MKDIR_NAME
# still exists after crash. SIZE-class leftovers: rmdir of a file is INVAL.
check_rmdir() {
    local lid=$1
    local tag=$2
    local i out name got=""
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    for i in $(seq 0 31); do
        name="raft-smoke-r$i"
        out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-create ${ADDRS[$lid]}:${PORT} 1 $name 040755" 2>/dev/null || true)
        say "$tag rmdir-prep mkdir $name: $out"
        if echo "$out" | grep -q 'status=0'; then
            got=$name
            break
        fi
    done
    [ -n "$got" ] || bad "$tag rmdir-prep mkdir none accepted"
    [ -n "$got" ] || return
    RMDIR_NAME=$got
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-rmdir ${ADDRS[$lid]}:${PORT} 1 $got" 2>/dev/null || true)
    say "$tag rmdir: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag rmdir not OK"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-lookup ${ADDRS[$lid]}:${PORT} 1 $got" 2>/dev/null || true)
    say "$tag lookup rmdir'd: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag lookup rmdir'd not NOT_FOUND"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-rmdir ${ADDRS[$lid]}:${PORT} 1 $got" 2>/dev/null || true)
    say "$tag rmdir again: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag rmdir again not NOT_FOUND"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-rmdir ${ADDRS[$lid]}:${PORT} 1 raft-smoke-f" 2>/dev/null || true)
    say "$tag rmdir file: $out"
    echo "$out" | grep -q 'status=6' || bad "$tag rmdir file not INVAL"
}

# LINK dest dentry + nlink++. Dedicated dest so raft-smoke-f stays.
# Directory src is INVAL. Duplicate dest is EXIST.
check_link() {
    local lid=$1
    local tag=$2
    local out ino dino
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-lookup ${ADDRS[$lid]}:${PORT} 1 raft-smoke-f" 2>/dev/null || true)
    say "$tag link lookup src: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag link src lookup not OK"
    ino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$ino" ] && [ "$ino" != "0" ] || bad "$tag link src ino"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-link ${ADDRS[$lid]}:${PORT} $ino 1 raft-smoke-l" 2>/dev/null || true)
    say "$tag link: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag link not OK"
    echo "$out" | grep -q 'nlink=2' || bad "$tag link nlink"
    if echo "$out" | grep -q 'status=0'; then
        LINK_NAME=raft-smoke-l
    fi
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-lookup ${ADDRS[$lid]}:${PORT} 1 raft-smoke-l" 2>/dev/null || true)
    say "$tag lookup linked: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag lookup linked not OK"
    echo "$out" | grep -q "ino=$ino" || bad "$tag lookup linked ino"
    echo "$out" | grep -q 'nlink=2' || bad "$tag lookup linked nlink"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-lookup ${ADDRS[$lid]}:${PORT} 1 raft-smoke-f" 2>/dev/null || true)
    say "$tag lookup src nlink: $out"
    echo "$out" | grep -q 'nlink=2' || bad "$tag src nlink after link"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-link ${ADDRS[$lid]}:${PORT} $ino 1 raft-smoke-l" 2>/dev/null || true)
    say "$tag link dup: $out"
    echo "$out" | grep -q 'status=2' || bad "$tag link dup not EXIST"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-link ${ADDRS[$lid]}:${PORT} 999999 1 raft-smoke-l2" 2>/dev/null || true)
    say "$tag link miss: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag link miss not NOT_FOUND"
    if [ -n "${MKDIR_NAME:-}" ]; then
        out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-lookup ${ADDRS[$lid]}:${PORT} 1 $MKDIR_NAME" 2>/dev/null || true)
        dino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
        if [ -n "$dino" ] && [ "$dino" != "0" ]; then
            out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-link ${ADDRS[$lid]}:${PORT} $dino 1 raft-smoke-ld" 2>/dev/null || true)
            say "$tag link dir: $out"
            echo "$out" | grep -q 'status=6' || bad "$tag link dir not INVAL"
        fi
    fi
}

# nlink>1 UNLINK: dedicated dest so raft-smoke-l still exists after crash.
# Surviving names keep nlink=2. Second unlink of the extra name is NOT_FOUND.
check_unlink_nlink() {
    local lid=$1
    local tag=$2
    local out ino
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-lookup ${ADDRS[$lid]}:${PORT} 1 raft-smoke-f" 2>/dev/null || true)
    say "$tag unlink-nlink lookup src: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag unlink-nlink src lookup not OK"
    ino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$ino" ] && [ "$ino" != "0" ] || bad "$tag unlink-nlink src ino"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-link ${ADDRS[$lid]}:${PORT} $ino 1 raft-smoke-h" 2>/dev/null || true)
    say "$tag unlink-nlink extra link: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag unlink-nlink extra link not OK"
    echo "$out" | grep -q 'nlink=3' || bad "$tag unlink-nlink extra nlink"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-unlink ${ADDRS[$lid]}:${PORT} 1 raft-smoke-h" 2>/dev/null || true)
    say "$tag unlink-nlink: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag unlink-nlink not OK"
    if echo "$out" | grep -q 'status=0'; then
        UNLINK_NLINK_NAME=raft-smoke-h
    fi
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-lookup ${ADDRS[$lid]}:${PORT} 1 raft-smoke-h" 2>/dev/null || true)
    say "$tag lookup unlinked extra: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag lookup unlinked extra not NOT_FOUND"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-lookup ${ADDRS[$lid]}:${PORT} 1 raft-smoke-f" 2>/dev/null || true)
    say "$tag lookup src after nlink unlink: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag src missing after nlink unlink"
    echo "$out" | grep -q 'nlink=2' || bad "$tag src nlink after nlink unlink"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-lookup ${ADDRS[$lid]}:${PORT} 1 raft-smoke-l" 2>/dev/null || true)
    say "$tag lookup linked after nlink unlink: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag linked missing after nlink unlink"
    echo "$out" | grep -q 'nlink=2' || bad "$tag linked nlink after nlink unlink"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-unlink ${ADDRS[$lid]}:${PORT} 1 raft-smoke-h" 2>/dev/null || true)
    say "$tag unlink-nlink again: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag unlink-nlink again not NOT_FOUND"
}

# Same-dir LOCAL file RENAME. Dedicated names so raft-smoke-f stays.
# Dest EXIST and directory src are INVAL/EXIST. Cross-dir is INVAL.
check_rename() {
    local lid=$1
    local tag=$2
    local out
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-create ${ADDRS[$lid]}:${PORT} 1 raft-smoke-n" 2>/dev/null || true)
    say "$tag rename-prep create: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag rename-prep create not OK"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-rename ${ADDRS[$lid]}:${PORT} 1 raft-smoke-n 1 raft-smoke-m" 2>/dev/null || true)
    say "$tag rename: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag rename not OK"
    if echo "$out" | grep -q 'status=0'; then
        RENAME_NAME=raft-smoke-m
        RENAME_OLD=raft-smoke-n
    fi
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-lookup ${ADDRS[$lid]}:${PORT} 1 raft-smoke-n" 2>/dev/null || true)
    say "$tag lookup rename old: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag lookup rename old not NOT_FOUND"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-lookup ${ADDRS[$lid]}:${PORT} 1 raft-smoke-m" 2>/dev/null || true)
    say "$tag lookup rename new: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag lookup rename new not OK"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-rename ${ADDRS[$lid]}:${PORT} 1 raft-smoke-n 1 raft-smoke-m2" 2>/dev/null || true)
    say "$tag rename miss: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag rename miss not NOT_FOUND"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-rename ${ADDRS[$lid]}:${PORT} 1 raft-smoke-f 1 raft-smoke-m" 2>/dev/null || true)
    say "$tag rename exist: $out"
    echo "$out" | grep -q 'status=2' || bad "$tag rename exist not EXIST"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-rename ${ADDRS[$lid]}:${PORT} 1 raft-smoke-m 2 raft-smoke-x" 2>/dev/null || true)
    say "$tag rename cross-dir: $out"
    echo "$out" | grep -q 'status=6' || bad "$tag rename cross-dir not INVAL"
    if [ -n "${MKDIR_NAME:-}" ]; then
        out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-rename ${ADDRS[$lid]}:${PORT} 1 $MKDIR_NAME 1 raft-smoke-md" 2>/dev/null || true)
        say "$tag rename dir: $out"
        echo "$out" | grep -q 'status=6' || bad "$tag rename dir not INVAL"
    fi
}

# Mode/owner SETATTR, chunk-aligned truncate, then utimens on
# raft-smoke-f (all stay after crash). Mixed SIZE+mode is INVAL.
# Unaligned SIZE (tail CAS) is on raft-smoke-p, not here.
check_setattr() {
    local lid=$1
    local tag=$2
    local out ino
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-lookup ${ADDRS[$lid]}:${PORT} 1 raft-smoke-f" 2>/dev/null || true)
    say "$tag setattr lookup: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag setattr lookup not OK"
    ino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$ino" ] && [ "$ino" != "0" ] || bad "$tag setattr ino"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-setattr ${ADDRS[$lid]}:${PORT} $ino 1 0600" 2>/dev/null || true)
    say "$tag setattr mode: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag setattr not OK"
    echo "$out" | grep -q 'mode=0100600' || bad "$tag setattr mode"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-getattr ${ADDRS[$lid]}:${PORT} $ino" 2>/dev/null || true)
    say "$tag getattr setattr: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag getattr setattr not OK"
    echo "$out" | grep -q 'mode=0100600' || bad "$tag getattr setattr mode"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-setattr ${ADDRS[$lid]}:${PORT} $ino 8 131072" 2>/dev/null || true)
    say "$tag setattr size: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag setattr size not OK"
    echo "$out" | grep -q 'size=131072' || bad "$tag setattr size"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-getattr ${ADDRS[$lid]}:${PORT} $ino" 2>/dev/null || true)
    say "$tag getattr size: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag getattr size not OK"
    echo "$out" | grep -q 'size=131072' || bad "$tag getattr size"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-setattr ${ADDRS[$lid]}:${PORT} $ino 9 0600" 2>/dev/null || true)
    say "$tag setattr mixed size: $out"
    echo "$out" | grep -q 'status=6' || bad "$tag setattr mixed size not INVAL"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-setattr ${ADDRS[$lid]}:${PORT} $ino 16 1000000000" 2>/dev/null || true)
    say "$tag setattr utimens: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag setattr utimens not OK"
    echo "$out" | grep -q 'mtime=1000000000' || bad "$tag setattr utimens mtime"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-getattr ${ADDRS[$lid]}:${PORT} $ino" 2>/dev/null || true)
    say "$tag getattr utimens: $out"
    echo "$out" | grep -q 'mtime=1000000000' || bad "$tag getattr utimens mtime"
    echo "$out" | grep -q 'size=131072' || bad "$tag getattr utimens size"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-setattr ${ADDRS[$lid]}:${PORT} 999999 1 0600" 2>/dev/null || true)
    say "$tag setattr miss: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag setattr miss not NOT_FOUND"
}

# Chunk publish (REPORT_CHUNKS → EFS_MD_CMD_PUBLISH) + GETCHUNKS. Dedicated
# file so write-mtime does not clobber raft-smoke-f's utimens fence.
check_publish() {
    local lid=$1
    local tag=$2
    local out ino
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-create ${ADDRS[$lid]}:${PORT} 1 raft-smoke-p" 2>/dev/null || true)
    say "$tag publish create: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag publish create not OK"
    ino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$ino" ] && [ "$ino" != "0" ] || bad "$tag publish ino"
    PUBLISH_NAME=raft-smoke-p
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-getchunks ${ADDRS[$lid]}:${PORT} $ino" 2>/dev/null || true)
    say "$tag getchunks empty: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag getchunks empty not OK"
    echo "$out" | grep -q 'count=0' || bad "$tag getchunks empty count"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-publish ${ADDRS[$lid]}:${PORT} $ino 0 131072" 2>/dev/null || true)
    say "$tag publish: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag publish not OK"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-getchunks ${ADDRS[$lid]}:${PORT} $ino" 2>/dev/null || true)
    say "$tag getchunks: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag getchunks not OK"
    echo "$out" | grep -q 'count=1' || bad "$tag getchunks count"
    echo "$out" | grep -q 'cis=0' || bad "$tag getchunks ci"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-getattr ${ADDRS[$lid]}:${PORT} $ino" 2>/dev/null || true)
    say "$tag getattr publish: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag getattr publish not OK"
    echo "$out" | grep -q 'size=131072' || bad "$tag getattr publish size"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-setattr ${ADDRS[$lid]}:${PORT} $ino 8 1000" 2>/dev/null || true)
    say "$tag setattr tail: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag setattr tail not OK"
    echo "$out" | grep -q 'size=1000' || bad "$tag setattr tail size"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-getattr ${ADDRS[$lid]}:${PORT} $ino" 2>/dev/null || true)
    say "$tag getattr tail: $out"
    echo "$out" | grep -q 'size=1000' || bad "$tag getattr tail size"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-getchunks ${ADDRS[$lid]}:${PORT} $ino" 2>/dev/null || true)
    say "$tag getchunks tail: $out"
    echo "$out" | grep -q 'count=1' || bad "$tag getchunks tail count"
    echo "$out" | grep -q 'cis=0' || bad "$tag getchunks tail ci"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-setattr ${ADDRS[$lid]}:${PORT} $ino 8 0" 2>/dev/null || true)
    say "$tag setattr trunc0: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag setattr trunc0 not OK"
    echo "$out" | grep -q 'size=0' || bad "$tag setattr trunc0 size"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-getchunks ${ADDRS[$lid]}:${PORT} $ino" 2>/dev/null || true)
    say "$tag getchunks trunc0: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag getchunks trunc0 not OK"
    echo "$out" | grep -q 'count=0' || bad "$tag getchunks trunc0 count"
}

# READDIR ROOT + LOOKUP_PATH. Run after rename so the listing is final.
check_readdir_path() {
    local lid=$1
    local tag=$2
    local out
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    leader=$lid
    out=$(g0_mgmt raft-readdir 1)
    lid=$leader
    say "$tag readdir: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag readdir not OK"
    echo "$out" | grep -q 'raft-smoke-f' || bad "$tag readdir missing f"
    if [ -n "${RENAME_NAME:-}" ]; then
        echo "$out" | grep -q "$RENAME_NAME" || bad "$tag readdir missing renamed"
    fi
    if [ -n "${RENAME_OLD:-}" ]; then
        echo "$out" | grep -q "$RENAME_OLD" && bad "$tag readdir still has rename-old"
    fi
    if [ -n "${LINK_NAME:-}" ]; then
        echo "$out" | grep -q "$LINK_NAME" || bad "$tag readdir missing link"
    fi
    if [ -n "${MKDIR_NAME:-}" ]; then
        echo "$out" | grep -q "$MKDIR_NAME" || bad "$tag readdir missing mkdir"
    fi
    if [ -n "${PUBLISH_NAME:-}" ]; then
        echo "$out" | grep -q "$PUBLISH_NAME" || bad "$tag readdir missing published"
    fi
    echo "$out" | grep -q 'raft-smoke-u' && bad "$tag readdir still has unlinked"
    if [ -n "${UNLINK_NLINK_NAME:-}" ]; then
        echo "$out" | grep -q "$UNLINK_NLINK_NAME" && bad "$tag readdir still has nlink-unlinked"
    fi
    out=$(g0_mgmt raft-lookup-path /raft-smoke-f)
    say "$tag lookup-path f: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag lookup-path f not OK"
    echo "$out" | grep -q 'name=raft-smoke-f' || bad "$tag lookup-path f name"
    if [ -n "${RENAME_NAME:-}" ]; then
        out=$(g0_mgmt raft-lookup-path "/$RENAME_NAME")
        say "$tag lookup-path renamed: $out"
        echo "$out" | grep -q 'status=0' || bad "$tag lookup-path renamed not OK"
        echo "$out" | grep -q "name=$RENAME_NAME" || bad "$tag lookup-path renamed name"
    fi
    if [ -n "${RENAME_OLD:-}" ]; then
        out=$(g0_mgmt raft-lookup-path "/$RENAME_OLD")
        say "$tag lookup-path rename-old: $out"
        echo "$out" | grep -q 'status=1' || bad "$tag lookup-path rename-old not NOT_FOUND"
    fi
    out=$(g0_mgmt raft-lookup-path /no-such-efs-name)
    say "$tag lookup-path miss: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag lookup-path miss not NOT_FOUND"
}

say "build 4 nodes (scratch, not live cluster)"
bfail=0
bpids=()
for h in "${HOSTS[@]}"; do
    build_one "$h" &
    bpids+=($!)
done
for p in "${bpids[@]}"; do
    wait "$p" || bfail=1
done
[ "$bfail" = 0 ] || { bad "build"; exit 1; }
say "build OK (test_wire gated on each node)"

cleanup
trap cleanup EXIT

say "start seed"
start_one 0
wait_listen fcstor003 || { bad "seed listen"; exit 1; }

say "start joiners"
start_one 1 &
start_one 2 &
start_one 3 &
wait
wait_listen fcstor004 || bad "004 listen"
wait_listen fcstor005 || bad "005 listen"
wait_listen fcstor006 || bad "006 listen"

say "wait for group-0 leader"
leader=""
for i in $(seq 1 50); do
    leader=$(g0_leader)
    if [ -n "$leader" ] && [ "$leader" != "-1" ]; then
        say "group-0 leader raft_id=$leader"
        break
    fi
done
if [ -z "$leader" ] || [ "$leader" = "-1" ]; then
    bad "no group-0 leader"
    ssh_to 10 fcstor003 "tail -40 /tmp/efs-raft-scratch.log" || true
    exit 1
fi

say "raft-mkfs (try voters until accepted)"
mkfs_ok=0
for idx in 0 1 2; do
    out=$(ssh_to 10 "${HOSTS[$idx]}" "cd /tmp/efs && ./efs-mgmt raft-mkfs ${ADDRS[$idx]}:${PORT}" 2>/dev/null || true)
    say "mkfs ${HOSTS[$idx]}: $out"
    echo "$out" | grep -q 'rc=0' && { mkfs_ok=1; break; }
done
[ "$mkfs_ok" = 1 ] || { bad "raft-mkfs not accepted"; exit 1; }

say "wait ROOT on voters 1,2,3"
for idx in 0 1 2; do
    got=0
    for i in $(seq 1 40); do
        r=$(root_on "$idx")
        if [ "$r" = "1" ]; then
            got=1
            break
        fi
    done
    [ "$got" = 1 ] || bad "no ROOT on ${HOSTS[$idx]}"
done

leader=$(g0_leader)
say "ReadIndex GETATTR/LOOKUP (fresh)"
check_reads "$leader" "fresh"
say "CREATE file through Raft (fresh)"
check_create "$leader" "fresh"
say "SETATTR mode through Raft (fresh)"
check_setattr "$leader" "fresh"
say "PUBLISH/GETCHUNKS through Raft (fresh)"
PUBLISH_NAME=""
check_publish "$leader" "fresh"
say "MKDIR through Raft (fresh)"
MKDIR_NAME=""
check_mkdir "$leader" "fresh"
say "UNLINK file through Raft (fresh)"
check_unlink "$leader" "fresh"
say "RMDIR through Raft (fresh)"
RMDIR_NAME=""
check_rmdir "$leader" "fresh"
say "LINK through Raft (fresh)"
LINK_NAME=""
check_link "$leader" "fresh"
say "UNLINK nlink>1 through Raft (fresh)"
UNLINK_NLINK_NAME=""
check_unlink_nlink "$leader" "fresh"
say "RENAME through Raft (fresh)"
RENAME_NAME=""
RENAME_OLD=""
check_rename "$leader" "fresh"
say "READDIR/LOOKUP_PATH through ReadIndex (fresh)"
check_readdir_path "$leader" "fresh"

# Pick a group-0 follower to kill (raft ids 0,1,2 minus leader).
crash_leader=$leader
follower_idx=""
for rid in 0 1 2; do
    if [ "$rid" != "$leader" ]; then
        follower_idx=$rid
        break
    fi
done
say "kill -9 follower node=$((follower_idx + 1)) (${HOSTS[$follower_idx]})"
ssh_to 10 "${HOSTS[$follower_idx]}" "
pids=\$(ps -eo pid,args | awk '/[e]fsd / && / --port ${PORT}( |\$)/ {print \$1}')
if [ -n \"\$pids\" ]; then kill -9 \$pids; fi
"

say "restart follower"
start_one "$follower_idx" keep
wait_listen "${HOSTS[$follower_idx]}" || bad "follower restart listen"
got=0
for i in $(seq 1 50); do
    r=$(root_on "$follower_idx")
    if [ "$r" = "1" ]; then
        got=1
        break
    fi
done
[ "$got" = 1 ] || bad "follower catch-up missed ROOT"
wait_g0_caught_up "$follower_idx" "follower" || true

# Kill the group-0 leader. Prefer a fresh status read; if the cluster is
# still settling after the follower restart, fall back to who led before
# that kill (HOSTS[-1] is fcstor006 — never a group-0 voter).
leader=$(g0_leader)
if [ -z "$leader" ] || [ "$leader" = "-1" ]; then
    leader=$crash_leader
    say "g0_leader missed after follower restart; using raft_id=$leader"
fi
if [ -z "$leader" ] || [ "$leader" = "-1" ]; then
    bad "no group-0 leader before leader kill"
    exit 1
fi
say "kill -9 leader raft_id=$leader"
leader_idx=$leader
ssh_to 10 "${HOSTS[$leader_idx]}" "
pids=\$(ps -eo pid,args | awk '/[e]fsd / && / --port ${PORT}( |\$)/ {print \$1}')
if [ -n \"\$pids\" ]; then kill -9 \$pids; fi
"

say "wait new group-0 leader"
new_leader=""
for i in $(seq 1 50); do
    for qidx in 0 1 2; do
        [ "$qidx" = "$leader_idx" ] && continue
        st=$(ssh_to 10 "${HOSTS[$qidx]}" "cd /tmp/efs && ./efs-mgmt raft-status ${ADDRS[$qidx]}:${PORT}" 2>/dev/null || true)
        cand=$(echo "$st" | awk '/group 0 / {
            for (i = 1; i <= NF; i++)
                if ($i ~ /^leader=/) { sub(/^leader=/, "", $i); print $i; exit }
        }')
        if [ -n "$cand" ] && [ "$cand" != "-1" ] && [ "$cand" != "$leader_idx" ]; then
            new_leader=$cand
            break 2
        fi
    done
done
if [ -z "$new_leader" ] || [ "$new_leader" = "-1" ]; then
    bad "no new leader after kill"
else
    say "new leader raft_id=$new_leader"
fi

surv_root=0
for idx in 0 1 2; do
    [ "$idx" = "$leader_idx" ] && continue
    r=$(root_on "$idx")
    [ "$r" = "1" ] && surv_root=$((surv_root + 1))
done
[ "$surv_root" -ge 1 ] || bad "ROOT lost on survivors after leader kill"

say "restart old leader"
start_one "$leader_idx" keep
wait_listen "${HOSTS[$leader_idx]}" || bad "old leader restart listen"
got=0
for i in $(seq 1 50); do
    r=$(root_on "$leader_idx")
    if [ "$r" = "1" ]; then
        got=1
        break
    fi
done
[ "$got" = 1 ] || bad "old leader catch-up missed ROOT"

say "wait group-0 leader after restart"
leader="-1"
for i in $(seq 1 40); do
    leader=$(g0_leader)
    if [ "$leader" != "-1" ]; then
        say "after-crash leader raft_id=$leader"
        break
    fi
done
after_lid=$leader
# Probe every voter for who actually serves ROOT GETATTR. The status
# line's leader= can name a node that already stepped down.
ready=0
for i in $(seq 1 40); do
    cur=$(g0_serving)
    if [ -z "$cur" ] || [ "$cur" = "-1" ]; then
        cur=$after_lid
    fi
    if [ -z "$cur" ] || [ "$cur" = "-1" ]; then
        continue
    fi
    out=$(ssh_to 5 "${HOSTS[$cur]}" "cd /tmp/efs && ./efs-mgmt raft-lookup ${ADDRS[$cur]}:${PORT} 1 no-such-efs-name" 2>/dev/null || true)
    if echo "$out" | grep -q 'status=1'; then
        leader=$cur
        ready=1
        say "after-crash serving raft_id=$leader"
        break
    fi
done
[ "$ready" = 1 ] || leader=$(g0_serving)
[ "$ready" = 1 ] || [ "$leader" != "-1" ] || leader=$after_lid
[ "$ready" = 1 ] || bad "after-crash leader not serving LOOKUP/GETATTR"
if [ -z "$leader" ] || [ "$leader" = "-1" ]; then
    bad "after-crash no group-0 leader"
    say "FAIL count=$FAIL"
    exit 1
fi
say "ReadIndex GETATTR/LOOKUP (after crash)"
root_nlink=2
[ -n "${MKDIR_NAME:-}" ] && root_nlink=3
check_reads "$leader" "after-crash" "$root_nlink"
out=$(g0_mgmt raft-lookup 1 raft-smoke-f)
say "after-crash lookup created: $out"
echo "$out" | grep -q 'status=0' || bad "after-crash created name missing"
ino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
if [ -n "$ino" ] && [ "$ino" != "0" ]; then
    out=$(g0_mgmt raft-getattr "$ino")
    say "after-crash getattr created: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash created getattr missing"
    echo "$out" | grep -q 'mode=0100600' || bad "after-crash setattr mode lost"
    echo "$out" | grep -q 'size=131072' || bad "after-crash truncate size lost"
    echo "$out" | grep -q 'mtime=1000000000' || bad "after-crash utimens mtime lost"
    if [ -n "${LINK_NAME:-}" ]; then
        echo "$out" | grep -q 'nlink=2' || bad "after-crash link nlink lost"
    fi
else
    bad "after-crash created ino missing"
fi
if [ -n "${PUBLISH_NAME:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$PUBLISH_NAME")
    say "after-crash lookup published: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash published name missing"
    pino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$pino" ] && [ "$pino" != "0" ] || bad "after-crash published ino"
    out=$(g0_mgmt raft-getchunks "$pino")
    say "after-crash getchunks: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash getchunks not OK"
    echo "$out" | grep -q 'count=0' || bad "after-crash getchunks count not cleared"
    out=$(g0_mgmt raft-getattr "$pino")
    say "after-crash getattr published: $out"
    echo "$out" | grep -q 'size=0' || bad "after-crash publish size not truncated"
fi
if [ -n "${MKDIR_NAME:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$MKDIR_NAME")
    say "after-crash lookup mkdir: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash mkdir name missing"
fi
if [ -n "${LINK_NAME:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$LINK_NAME")
    say "after-crash lookup linked: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash linked name missing"
    echo "$out" | grep -q 'nlink=2' || bad "after-crash linked nlink"
fi
out=$(g0_mgmt raft-lookup 1 raft-smoke-u)
say "after-crash lookup unlinked: $out"
echo "$out" | grep -q 'status=1' || bad "after-crash unlinked name came back"
if [ -n "${RMDIR_NAME:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$RMDIR_NAME")
    say "after-crash lookup rmdir: $out"
    echo "$out" | grep -q 'status=1' || bad "after-crash rmdir'd name came back"
fi
if [ -n "${UNLINK_NLINK_NAME:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$UNLINK_NLINK_NAME")
    say "after-crash lookup nlink-unlinked: $out"
    echo "$out" | grep -q 'status=1' || bad "after-crash nlink-unlinked name came back"
fi
if [ -n "${RENAME_NAME:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$RENAME_NAME")
    say "after-crash lookup renamed: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash renamed name missing"
fi
if [ -n "${RENAME_OLD:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$RENAME_OLD")
    say "after-crash lookup rename-old: $out"
    echo "$out" | grep -q 'status=1' || bad "after-crash rename old name came back"
fi
say "ReadIndex READDIR/LOOKUP_PATH (after crash)"
check_readdir_path "$leader" "after-crash"

if [ "$FAIL" -eq 0 ]; then
    say "PASS"
    exit 0
fi
say "FAIL count=$FAIL"
exit 1
