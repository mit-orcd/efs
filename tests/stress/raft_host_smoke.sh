#!/bin/bash
# Scratch-cluster gate for the production Raft host (10.5c).
#
# The standing 19820 /tmp scratch is RETIRED (Sep 17). Live gates run on
# 19810 /data1. This script still *can* spin a disposable 19820, but it
# refuses if 19810 efsd is up so it cannot split the cluster. Does not
# pkill -x efsd.
#
# Gate: elect, raft-mkfs lands ROOT on every voter, LOOKUP/GETATTR via
# ReadIndex + KV (a group-0 leader or caught-up replica serves ROOT;
# missing name is NOT_FOUND), file CREATE through Raft (lookup+getattr, duplicate
# is EXIST), SETATTR mode/owner, MKDIR as a 2-shard txn (including a
# dest whose inode shard is on group 2, submitted from raft_id 0 which
# does not host that group), last-link UNLINK
# of a file (lookup miss, second unlink NOT_FOUND), RMDIR as a 2-shard
# txn (empty LOCAL dir, lookup miss), LINK as a 2-shard txn (nlink=2,
# dest lookup, directory is INVAL), nlink>1 UNLINK of a dedicated extra
# name (lookup miss, surviving nlink=2), utimens mtime fence, same-dir
# LOCAL file RENAME (old name gone, new name present), same-dir LOCAL
# directory RENAME (raft-smoke-rd → raft-smoke-re, pver bump), HASHED dest
# CREATE + same-dir HASHED file RENAME (dedicated raft-smoke-hd split empty
# then a file whose hashed dentry shard is on the other Raft group, then
# renamed onto a second hashed name) + HASHED last-link UNLINK of a
# dedicated extra name, HASHED dest LINK onto a third hashed name
# (nlink=2), HASHED empty-dir RMDIR (raft-smoke-he), HASHED dest-dir
# overwrite (raft-smoke-hs → emptied HASHED raft-smoke-hx; nonempty dest
# is NOT_EMPTY), SPLITTING dest CREATE + READDIR + UNLINK/RMDIR + LINK
# + RENAME (raft-smoke-sp stays SPLITTING: local `pre` plus leftover `q`
# plus a hashed-name file plus empty child `e`; unlink/rmdir write
# hashed tombstones (I8); dest LINK writes hashed; dest RENAME writes
# hashed, leftover dest is POSIX replace, src drop is I8; migrate of a
# populated local range is a 2-shard txn when the leftover hashes onto
# the other Raft group), HOLD open-unlinked lease
# (dedicated raft-smoke-k: open, unlink name, getattr nlink=0, leave
# held through crash, close reclaims), FLOCK grant/release (dedicated
# raft-smoke-w: EX owner=1, EX owner=2 BUSY, UN owner=1, EX owner=2,
# leave owner=2 held through crash), FCNTL grant/release (dedicated
# raft-smoke-c: EX owner=1, EX owner=2 BUSY, flock EX owner=2 OK on
# the other domain, UN fcntl, EX owner=1 left through crash), FCNTL
# byte ranges (dedicated raft-smoke-t: EX [0,100) owner=1, adjacent
# [100,200) owner=2 OK, F_GETLK overlap reports owner=1 [0,100),
# own-range GETLK is UNLCK, overlap [50,150) BUSY, inverted range INVAL,
# leave [100,200) owner=2 through crash), FCNTL blocking waits
# (dedicated raft-smoke-q: wex owner=2 pends behind owner=1's EX,
# wsh owner=3 queues behind the waiter, release grants owner=2 only
# (FIFO), owner=3 grants after owner=2's release, inverted range and
# flock-domain range INVAL, leave owner=5 EX [500,600) through crash), READDIR of ROOT
# (created/renamed/link/mkdir names present, unlinked names absent),
# LOOKUP_PATH of those names, chunk-aligned SETATTR SIZE (truncate),
# chunk publish + GETCHUNKS (lane 0), unaligned truncate tail + trunc-0,
# O_APPEND reserve (reply size=watermark, getattr still 0 until publish
# resolves), SYMLINK as CREATE S_IFLNK + publish of the target bytes
# (no SYMLINK opcode, no target column), kill -9 a follower
# then the leader, restart catch-up keeps ROOT, the created file (new
# mode, size=131072, nlink=2, utimens mtime), the published file truncated
# to size=0 with no chunk map, the O_APPEND file at size=131072,
# the symlink (mode=0120777, size=11),
# the mkdir, the extra link name,
# the unlinked name stays gone, the rmdir'd name stays gone, the nlink>1
# unlinked name stays gone, the renamed file stays, the renamed dir stays,
# the HASHED dest file stays under its post-rename name (old hashed name
# stays gone), the HASHED dest link name stays with nlink=2, the HASHED
# unlinked extra name stays gone, the HASHED
# empty dir stays gone, the HASHED dest-dir overwrite keeps hx (src ino)
# and hs stays gone, the SPLITTING dir keeps local `pre` and the hashed
# dest file (dir stays SPLITTING), the held-unlinked inode stays at nlink=0
# until close reclaims it, the flock file stays and owner=2's EX still
# BUSYs owner=1 until UN, the fcntl file stays and owner=1's EX still
# BUSYs owner=2 until UN, the ranged fcntl file stays and owner=2's
# [100,200) still BUSYs that range (F_GETLK still reports it),
# the wait file stays and owner=5's [500,600) still BUSYs owner=9
# while the waiter that pended across the leader kill re-issues on the
# new leader and grants after owner=5's release,
# the session record stays ACTIVE with its registered shard bit,
# a HOLD/FLOCK carrying that uuid is accepted and a wrong epoch is BUSY,
# fencing that session dequeues a waiter of the old epoch, resolves a
# pending O_APPEND reservation as a FENCED_HOLE, and the new epoch is
# accepted after establish,
# and READDIR / LOOKUP_PATH still match.
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

# Refuse to stand up 19820 while the live 19810 cluster is running.
live_19810() {
    local h
    for h in "${HOSTS[@]}"; do
        if ssh_to 8 "$h" "ps -eo args | awk '/[e]fsd / && / --port 19810( |\$)/ {found=1} END{exit !found}'"; then
            return 0
        fi
    done
    return 1
}
if live_19810; then
    echo "[raft-host] REFUSE: 19810 efsd is up. 19820 scratch is retired." >&2
    echo "[raft-host] Gates now run on 19810. Do not start a second cluster." >&2
    exit 2
fi

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
    # Background waiters live on this login-node process, not on the
    # scratch hosts. A leftover 400s raft-fcntl can hit the NEXT smoke.
    kill ${WAIT8_PID:-} ${WAIT2_PID:-} ${WAIT3_PID:-} 2>/dev/null || true
    kill_scratch
    local h
    for h in "${HOSTS[@]}"; do
        ssh_to 10 "$h" "rm -rf $SCRATCH" >/dev/null 2>&1 || true
    done
}
trap cleanup EXIT

build_one() {
    local h=$1
    ssh_to 120 "$h" "
set -e
killall -9 make gcc 2>/dev/null || true
sleep 3
mkdir -p /tmp/efs
rsync -a --checksum --delete --delete-excluded --exclude='/mnt/' --exclude='/.git/' --exclude='*.log' --exclude='*.o' --exclude='*.a' --exclude='tests/test_wire' --exclude='tests/test_meta_apply' --exclude='efsd' --exclude='efs-mgmt' --exclude='efs-fuse' '$SRC/' /tmp/efs/ || { rc=\$?; [ \"\$rc\" -eq 24 ]; }
cd /tmp/efs
make clean >/dev/null
make -j\"\$(nproc)\" efsd efs-mgmt tests/test_wire tests/test_meta_apply
chmod +x tests/test_wire tests/test_meta_apply
./tests/test_wire
./tests/test_meta_apply
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
export EFS_TRANSPORT=tcp
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
        say "$tag: no leader for catch-up yet"
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
    say "$tag catch-up applied=$got want=$want"
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
    local i out lid to tries
    shift
    lid=${leader:-}
    # G0_TO / G0_TRIES override the per-try SSH timeout / try count. Blocking
    # lock waiters need a per-try window longer than the time they pend
    # server-side, or the client-side timeout orphans the queued wait and the
    # retry re-queues behind it (harmless — same owner is not a conflict —
    # but needlessly). Discovery probes (g0_serving/g0_leader) stay short.
    to=${G0_TO:-5}
    tries=${G0_TRIES:-8}
    for i in $(seq 1 $tries); do
        if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
            lid=$(g0_serving)
        fi
        if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
            lid=$(g0_leader)
        fi
        if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
            continue
        fi
        out=$(ssh_to $to "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt $cmd ${ADDRS[$lid]}:${PORT} $*" 2>/dev/null || true)
        # Empty is a 5s SSH miss, not a status. Same 8-try budget as
        # NOT_PRIMARY — do not treat it as NOT_FOUND / missing name.
        if [ -z "$out" ]; then
            lid=""
            continue
        fi
        if echo "$out" | grep -q 'status=7'; then
            # NOT_PRIMARY: follow the reply's leader hint (primary= is
            # 1-based; 0 = no hint), then the status view. Blocking
            # waits run at the leader only, so any-serving is wrong.
            lid=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^primary=/) {split($i,a,"="); if (a[2] >= 1) print a[2]-1}}')
            if [ -z "$lid" ]; then
                lid=$(g0_leader)
            fi
            continue
        fi
        leader=$lid
        printf '%s\n' "$out"
        return 0
    done
    printf '%s\n' "${out:-}"
}

# ReadIndex GETATTR/LOOKUP against group-0. Leader must serve ROOT;
# LOOKUP/GETATTR of ROOT on a serving replica (ReadIndex). A missing
# name is NOT_FOUND=1. nlink is 2 at mkfs and grows with live subdirs.
check_reads() {
    local lid=$1
    local tag=$2
    local nlink=${3:-2}
    local out
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

# raft_id 0 (fcstor003) never hosts group 2. MKDIR of a dest whose inode
# shard is even must bounce the CREATE to a dual-host and still succeed.
check_cross_group() {
    local i out name ino got=""
    for i in $(seq 0 31); do
        name="raft-smoke-xg$i"
        out=$(ssh_to 10 "${HOSTS[0]}" "cd /tmp/efs && ./efs-mgmt raft-create ${ADDRS[0]}:${PORT} 1 $name 040755" 2>/dev/null || true)
        say "cross-group mkdir $name: $out"
        if echo "$out" | grep -q 'status=0'; then
            ino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
            if [ -n "$ino" ] && [ "$((ino & 1))" = "0" ]; then
                got=$name
                break
            fi
            out=$(ssh_to 10 "${HOSTS[0]}" "cd /tmp/efs && ./efs-mgmt raft-rmdir ${ADDRS[0]}:${PORT} 1 $name" 2>/dev/null || true)
            say "cross-group rmdir odd $name: $out"
        fi
    done
    [ -n "$got" ] || bad "cross-group mkdir none even-shard"
    if [ -n "$got" ]; then
        out=$(ssh_to 10 "${HOSTS[0]}" "cd /tmp/efs && ./efs-mgmt raft-lookup ${ADDRS[0]}:${PORT} 1 $got" 2>/dev/null || true)
        say "cross-group lookup: $out"
        echo "$out" | grep -q 'status=0' || bad "cross-group lookup not OK"
        echo "$out" | grep -q 'mode=040755' || bad "cross-group lookup mode"
        XG_NAME=$got
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
# Dest EXIST. Cross-dir is INVAL. Directory src is a later check.
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
    # Dedicated names so raft-smoke-f / raft-smoke-m stay put.
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-create ${ADDRS[$lid]}:${PORT} 1 raft-smoke-ne" 2>/dev/null || true)
    echo "$out" | grep -q 'status=0' || bad "$tag rename-exist dest create"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-create ${ADDRS[$lid]}:${PORT} 1 raft-smoke-ns" 2>/dev/null || true)
    echo "$out" | grep -q 'status=0' || bad "$tag rename-exist src create"
    ns_ino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-rename ${ADDRS[$lid]}:${PORT} 1 raft-smoke-ns 1 raft-smoke-ne" 2>/dev/null || true)
    say "$tag rename exist: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag rename exist not OK"
    echo "$out" | grep -q "ino=$ns_ino" || bad "$tag rename exist ino"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-create ${ADDRS[$lid]}:${PORT} 1 raft-smoke-xd 040755" 2>/dev/null || true)
    echo "$out" | grep -q 'status=0' || bad "$tag rename cross-dir dest mkdir"
    xd_ino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-create ${ADDRS[$lid]}:${PORT} 1 raft-smoke-xc" 2>/dev/null || true)
    echo "$out" | grep -q 'status=0' || bad "$tag rename cross-dir src create"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-rename ${ADDRS[$lid]}:${PORT} 1 raft-smoke-xc $xd_ino raft-smoke-x" 2>/dev/null || true)
    say "$tag rename cross-dir: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag rename cross-dir not OK"
}

# Same-dir LOCAL directory rename. Dedicated names so MKDIR_NAME stays.
# Dest file overwrite is INVAL (ENOTDIR). Cross-dir into a dest dir is OK.
check_dir_rename() {
    local lid=$1
    local tag=$2
    local out
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-create ${ADDRS[$lid]}:${PORT} 1 raft-smoke-rd 040755" 2>/dev/null || true)
    say "$tag dir-rename-prep mkdir: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag dir-rename-prep mkdir not OK"
    echo "$out" | grep -q 'mode=040755' || bad "$tag dir-rename-prep mode"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-rename ${ADDRS[$lid]}:${PORT} 1 raft-smoke-rd 1 raft-smoke-re" 2>/dev/null || true)
    say "$tag dir-rename: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag dir-rename not OK"
    if echo "$out" | grep -q 'status=0'; then
        DIR_RENAME_NAME=raft-smoke-re
        DIR_RENAME_OLD=raft-smoke-rd
    fi
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-lookup ${ADDRS[$lid]}:${PORT} 1 raft-smoke-rd" 2>/dev/null || true)
    say "$tag lookup dir-rename old: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag lookup dir-rename old not NOT_FOUND"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-lookup ${ADDRS[$lid]}:${PORT} 1 raft-smoke-re" 2>/dev/null || true)
    say "$tag lookup dir-rename new: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag lookup dir-rename new not OK"
    echo "$out" | grep -q 'mode=040755' || bad "$tag lookup dir-rename mode"
    echo "$out" | grep -q 'nlink=2' || bad "$tag lookup dir-rename nlink"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-rename ${ADDRS[$lid]}:${PORT} 1 raft-smoke-rd 1 raft-smoke-re2" 2>/dev/null || true)
    say "$tag dir-rename miss: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag dir-rename miss not NOT_FOUND"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-create ${ADDRS[$lid]}:${PORT} 1 raft-smoke-df" 2>/dev/null || true)
    echo "$out" | grep -q 'status=0' || bad "$tag dir-rename exist dest create"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-rename ${ADDRS[$lid]}:${PORT} 1 raft-smoke-re 1 raft-smoke-df" 2>/dev/null || true)
    say "$tag dir-rename exist: $out"
    echo "$out" | grep -qE 'status=2|status=6' || bad "$tag dir-rename exist not EXIST/INVAL"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-create ${ADDRS[$lid]}:${PORT} 1 raft-smoke-dd 040755" 2>/dev/null || true)
    echo "$out" | grep -q 'status=0' || bad "$tag dir-rename cross-dir dest mkdir"
    dd_ino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-create ${ADDRS[$lid]}:${PORT} 1 raft-smoke-dc 040755" 2>/dev/null || true)
    echo "$out" | grep -q 'status=0' || bad "$tag dir-rename cross-dir src mkdir"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-rename ${ADDRS[$lid]}:${PORT} 1 raft-smoke-dc $dd_ino raft-smoke-dx" 2>/dev/null || true)
    say "$tag dir-rename cross-dir: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag dir-rename cross-dir not OK"
}

# Empty dedicated dir → HASHED, then file CREATE whose dir lane lives on
# the other Raft group (first-use 2-shard txn). Do not HASHED ROOT.
# A second name on the same lane is a single CREATE (bit already set).
hashed_spread_names() {
    local parent=$1
    python3 -c "
parent=int('$parent')
MASK=0xFFF
def dir_lane(name):
    h=2166136261
    for c in name.encode():
        h ^= c
        h = (h * 16777619) & 0xffffffff
    return h % 64
def lane_shard(ino, lane):
    h = ((ino & 0xffffffff) * 2654435761) & 0xffffffff
    stride = 2 * (h & 0x7FF) + 1
    return ((ino & MASK) + lane * stride) & MASK
psh = parent & MASK
first=second=''
want=None
for i in range(8192):
    nm='n%d' % i
    lane=dir_lane(nm)
    dsh=lane_shard(parent, lane)
    if lane==0 or dsh==psh or (dsh & 1)==(psh & 1):
        continue
    if not first:
        first=nm
        want=lane
        continue
    if lane==want:
        second=nm
        break
print(first, second)
"
}

check_hashed_create() {
    local lid=$1
    local tag=$2
    local out ino hd_ino names nm nm2 lnm psh ish hs_ino hx_ino sp_ino
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    leader=$lid
    out=$(g0_mgmt raft-create 1 raft-smoke-hd 040755)
    lid=$leader
    say "$tag hashed-dir mkdir: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag hashed-dir mkdir not OK"
    echo "$out" | grep -q 'mode=040755' || bad "$tag hashed-dir mode"
    hd_ino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$hd_ino" ] && [ "$hd_ino" != "0" ] || { bad "$tag hashed-dir ino"; return; }
    HASHED_DIR=raft-smoke-hd
    HASHED_DIR_INO=$hd_ino
    out=$(g0_mgmt raft-dir "$hd_ino" begin)
    say "$tag hashed-dir begin: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag hashed-dir begin not OK"
    out=$(g0_mgmt raft-dir "$hd_ino" migrate)
    say "$tag hashed-dir migrate: $out"
    echo "$out" | grep -qE 'status=0|status=1' || bad "$tag hashed-dir migrate not OK"
    out=$(g0_mgmt raft-dir "$hd_ino" finish)
    say "$tag hashed-dir finish: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag hashed-dir finish not OK"
    names=$(hashed_spread_names "$hd_ino")
    nm=$(echo "$names" | awk '{print $1}')
    nm2=$(echo "$names" | awk '{print $2}')
    [ -n "$nm" ] || { bad "$tag hashed spread name"; return; }
    out=$(g0_mgmt raft-create "$hd_ino" "$nm")
    say "$tag hashed-create $nm: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag hashed-create not OK"
    echo "$out" | grep -q 'mode=0100644' || bad "$tag hashed-create mode"
    echo "$out" | grep -q 'nlink=1' || bad "$tag hashed-create nlink"
    ino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$ino" ] && [ "$ino" != "0" ] || bad "$tag hashed-create ino"
    psh=$((hd_ino & 4095))
    ish=$((ino & 4095))
    if [ "$psh" -eq "$ish" ]; then
        bad "$tag hashed-create ino shard=$ish still parent shard (not HASHED dest)"
    fi
    HASHED_FILE=$nm
    out=$(g0_mgmt raft-lookup "$hd_ino" "$nm")
    say "$tag hashed-lookup: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag hashed-lookup not OK"
    echo "$out" | grep -q "ino=$ino" || bad "$tag hashed-lookup ino"
    out=$(g0_mgmt raft-readdir "$hd_ino")
    say "$tag hashed-readdir: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag hashed-readdir not OK"
    echo "$out" | grep -q "$nm" || bad "$tag hashed-readdir missing file"
    out=$(g0_mgmt raft-create "$hd_ino" "$nm")
    say "$tag hashed-create dup: $out"
    echo "$out" | grep -q 'status=2' || bad "$tag hashed-create dup not EXIST"
    if [ -n "$nm2" ]; then
        out=$(g0_mgmt raft-create "$hd_ino" "$nm2")
        say "$tag hashed-create-lane $nm2: $out"
        echo "$out" | grep -q 'status=0' || bad "$tag hashed-create-lane not OK"
        HASHED_FILE2=$nm2
        out=$(g0_mgmt raft-lookup "$hd_ino" "$nm2")
        echo "$out" | grep -q 'status=0' || bad "$tag hashed-lookup-lane not OK"
    fi
    out=$(g0_mgmt raft-lookup-path "/raft-smoke-hd/$nm")
    say "$tag hashed lookup-path: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag hashed lookup-path not OK"
    echo "$out" | grep -q "name=$nm" || bad "$tag hashed lookup-path name"
    rnm=$(python3 -c "
parent=int('$hd_ino')
MASK=0xFFF
skip=set('$nm $nm2'.split())
def dir_lane(name):
    h=2166136261
    for c in name.encode():
        h ^= c
        h = (h * 16777619) & 0xffffffff
    return h % 64
def lane_shard(ino, lane):
    h = ((ino & 0xffffffff) * 2654435761) & 0xffffffff
    stride = 2 * (h & 0x7FF) + 1
    return ((ino & MASK) + lane * stride) & MASK
psh = parent & MASK
for i in range(8192):
    n='r%d' % i
    lane=dir_lane(n)
    dsh=lane_shard(parent, lane)
    if lane==0 or dsh==psh or n in skip:
        continue
    print(n)
    break
")
    if [ -n "$rnm" ]; then
        out=$(g0_mgmt raft-rename "$hd_ino" "$nm" "$hd_ino" "$rnm")
        say "$tag hashed-rename $nm -> $rnm: $out"
        echo "$out" | grep -q 'status=0' || bad "$tag hashed-rename not OK"
        if echo "$out" | grep -q 'status=0'; then
            HASHED_FILE_OLD=$nm
            HASHED_FILE=$rnm
        fi
        out=$(g0_mgmt raft-lookup "$hd_ino" "$nm")
        echo "$out" | grep -q 'status=1' || bad "$tag hashed-rename old not NOT_FOUND"
        out=$(g0_mgmt raft-lookup "$hd_ino" "$rnm")
        echo "$out" | grep -q 'status=0' || bad "$tag hashed-rename new not OK"
    else
        bad "$tag hashed-rename dest name"
    fi
    unm=$(python3 -c "
parent=int('$hd_ino')
MASK=0xFFF
skip=set('$nm $nm2 ${rnm:-} ${HASHED_FILE:-}'.split())
def dir_lane(name):
    h=2166136261
    for c in name.encode():
        h ^= c
        h = (h * 16777619) & 0xffffffff
    return h % 64
def lane_shard(ino, lane):
    h = ((ino & 0xffffffff) * 2654435761) & 0xffffffff
    stride = 2 * (h & 0x7FF) + 1
    return ((ino & MASK) + lane * stride) & MASK
psh = parent & MASK
for i in range(8192):
    n='u%d' % i
    lane=dir_lane(n)
    dsh=lane_shard(parent, lane)
    if lane==0 or dsh==psh or n in skip:
        continue
    print(n)
    break
")
    if [ -n "$unm" ]; then
        out=$(g0_mgmt raft-create "$hd_ino" "$unm")
        say "$tag hashed-unlink-prep $unm: $out"
        echo "$out" | grep -q 'status=0' || bad "$tag hashed-unlink-prep not OK"
        out=$(g0_mgmt raft-unlink "$hd_ino" "$unm")
        say "$tag hashed-unlink $unm: $out"
        echo "$out" | grep -q 'status=0' || bad "$tag hashed-unlink not OK"
        if echo "$out" | grep -q 'status=0'; then
            HASHED_UNLINK_OLD=$unm
        fi
        out=$(g0_mgmt raft-lookup "$hd_ino" "$unm")
        echo "$out" | grep -q 'status=1' || bad "$tag hashed-unlink not NOT_FOUND"
    else
        bad "$tag hashed-unlink dest name"
    fi
    lnm=$(python3 -c "
parent=int('$hd_ino')
MASK=0xFFF
skip=set('$nm $nm2 ${rnm:-} ${HASHED_FILE:-} ${unm:-}'.split())
def dir_lane(name):
    h=2166136261
    for c in name.encode():
        h ^= c
        h = (h * 16777619) & 0xffffffff
    return h % 64
def lane_shard(ino, lane):
    h = ((ino & 0xffffffff) * 2654435761) & 0xffffffff
    stride = 2 * (h & 0x7FF) + 1
    return ((ino & MASK) + lane * stride) & MASK
psh = parent & MASK
for i in range(8192):
    n='l%d' % i
    lane=dir_lane(n)
    dsh=lane_shard(parent, lane)
    if lane==0 or dsh==psh or (dsh & 1)==(psh & 1) or n in skip:
        continue
    print(n)
    break
")
    if [ -n "$lnm" ] && [ -n "$ino" ] && [ "$ino" != "0" ]; then
        out=$(g0_mgmt raft-link "$ino" "$hd_ino" "$lnm")
        say "$tag hashed-link $lnm: $out"
        echo "$out" | grep -q 'status=0' || bad "$tag hashed-link not OK"
        echo "$out" | grep -q 'nlink=2' || bad "$tag hashed-link nlink"
        if echo "$out" | grep -q 'status=0'; then
            HASHED_LINK_NAME=$lnm
        fi
        out=$(g0_mgmt raft-lookup "$hd_ino" "$lnm")
        echo "$out" | grep -q 'status=0' || bad "$tag hashed-link lookup not OK"
        echo "$out" | grep -q "ino=$ino" || bad "$tag hashed-link lookup ino"
        echo "$out" | grep -q 'nlink=2' || bad "$tag hashed-link lookup nlink"
        out=$(g0_mgmt raft-lookup "$hd_ino" "$HASHED_FILE")
        echo "$out" | grep -q 'nlink=2' || bad "$tag hashed-link src nlink"
        out=$(g0_mgmt raft-link "$ino" "$hd_ino" "$lnm")
        say "$tag hashed-link dup: $out"
        echo "$out" | grep -q 'status=2' || bad "$tag hashed-link dup not EXIST"
        out=$(g0_mgmt raft-readdir "$hd_ino")
        echo "$out" | grep -q "$lnm" || bad "$tag hashed-link readdir missing"
        out=$(g0_mgmt raft-lookup-path "/raft-smoke-hd/$lnm")
        echo "$out" | grep -q 'status=0' || bad "$tag hashed-link lookup-path not OK"
        echo "$out" | grep -q "name=$lnm" || bad "$tag hashed-link lookup-path name"
    else
        bad "$tag hashed-link dest name"
    fi
    out=$(g0_mgmt raft-create 1 raft-smoke-he 040755)
    say "$tag hashed-rmdir-prep mkdir: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag hashed-rmdir-prep mkdir not OK"
    he_ino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$he_ino" ] && [ "$he_ino" != "0" ] || bad "$tag hashed-rmdir-prep ino"
    out=$(g0_mgmt raft-dir "$he_ino" begin)
    echo "$out" | grep -q 'status=0' || bad "$tag hashed-rmdir begin not OK"
    out=$(g0_mgmt raft-dir "$he_ino" migrate)
    echo "$out" | grep -qE 'status=0|status=1' || bad "$tag hashed-rmdir migrate not OK"
    out=$(g0_mgmt raft-dir "$he_ino" finish)
    echo "$out" | grep -q 'status=0' || bad "$tag hashed-rmdir finish not OK"
    out=$(g0_mgmt raft-rmdir 1 raft-smoke-he)
    say "$tag hashed-rmdir: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag hashed-rmdir not OK"
    if echo "$out" | grep -q 'status=0'; then
        HASHED_RMDIR_OLD=raft-smoke-he
    fi
    out=$(g0_mgmt raft-lookup 1 raft-smoke-he)
    echo "$out" | grep -q 'status=1' || bad "$tag hashed-rmdir not NOT_FOUND"

    out=$(g0_mgmt raft-create 1 raft-smoke-hs 040755)
    say "$tag hashed-dow-src mkdir: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag hashed-dow-src mkdir not OK"
    hs_ino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$hs_ino" ] && [ "$hs_ino" != "0" ] || bad "$tag hashed-dow-src ino"
    out=$(g0_mgmt raft-create 1 raft-smoke-hx 040755)
    say "$tag hashed-dow-dst mkdir: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag hashed-dow-dst mkdir not OK"
    hx_ino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$hx_ino" ] && [ "$hx_ino" != "0" ] || bad "$tag hashed-dow-dst ino"
    out=$(g0_mgmt raft-dir "$hx_ino" begin)
    echo "$out" | grep -q 'status=0' || bad "$tag hashed-dow begin not OK"
    out=$(g0_mgmt raft-dir "$hx_ino" migrate)
    echo "$out" | grep -qE 'status=0|status=1' || bad "$tag hashed-dow migrate not OK"
    out=$(g0_mgmt raft-dir "$hx_ino" finish)
    echo "$out" | grep -q 'status=0' || bad "$tag hashed-dow finish not OK"
    out=$(g0_mgmt raft-create "$hx_ino" f)
    say "$tag hashed-dow child: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag hashed-dow child not OK"
    out=$(g0_mgmt raft-rename 1 raft-smoke-hs 1 raft-smoke-hx)
    say "$tag hashed-dow nonempty: $out"
    echo "$out" | grep -q 'status=8' || bad "$tag hashed-dow nonempty not NOT_EMPTY"
    out=$(g0_mgmt raft-unlink "$hx_ino" f)
    say "$tag hashed-dow unlink child: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag hashed-dow unlink child not OK"
    out=$(g0_mgmt raft-rename 1 raft-smoke-hs 1 raft-smoke-hx)
    say "$tag hashed-dow: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag hashed-dow not OK"
    if echo "$out" | grep -q 'status=0'; then
        HASHED_DOW=raft-smoke-hx
        HASHED_DOW_OLD=raft-smoke-hs
        HASHED_DOW_INO=$hs_ino
    fi
    out=$(g0_mgmt raft-lookup 1 raft-smoke-hs)
    echo "$out" | grep -q 'status=1' || bad "$tag hashed-dow src not NOT_FOUND"
    out=$(g0_mgmt raft-lookup 1 raft-smoke-hx)
    echo "$out" | grep -q 'status=0' || bad "$tag hashed-dow dest not OK"
    echo "$out" | grep -q "ino=$hs_ino" || bad "$tag hashed-dow dest ino"

    out=$(g0_mgmt raft-create 1 raft-smoke-sp 040755)
    say "$tag split-dir mkdir: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag split-dir mkdir not OK"
    sp_ino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$sp_ino" ] && [ "$sp_ino" != "0" ] || bad "$tag split-dir ino"
    SPLIT_DIR=raft-smoke-sp
    SPLIT_DIR_INO=$sp_ino
    out=$(g0_mgmt raft-create "$sp_ino" pre)
    say "$tag split-pre: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag split-pre not OK"
    SPLIT_PRE=pre
    out=$(g0_mgmt raft-create "$sp_ino" q)
    say "$tag split-q: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag split-q not OK"
    out=$(g0_mgmt raft-create "$sp_ino" e 040755)
    say "$tag split-empty mkdir: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag split-empty mkdir not OK"
    SPLIT_EMPTY=e
    out=$(g0_mgmt raft-dir "$sp_ino" begin)
    say "$tag split-dir begin: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag split-dir begin not OK"
    names=$(hashed_spread_names "$sp_ino")
    nm=$(echo "$names" | awk '{print $1}')
    nm2=$(echo "$names" | awk '{print $2}')
    [ -n "$nm" ] || { bad "$tag split spread name"; return; }
    [ -n "$nm2" ] || { bad "$tag split spread dest name"; return; }
    out=$(g0_mgmt raft-create "$sp_ino" "$nm")
    say "$tag split-create $nm: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag split-create not OK"
    ino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$ino" ] && [ "$ino" != "0" ] || bad "$tag split-create ino"
    psh=$((sp_ino & 4095))
    ish=$((ino & 4095))
    if [ "$psh" -eq "$ish" ]; then
        bad "$tag split-create ino shard=$ish still parent shard (not hashed dest)"
    fi
    SPLIT_FILE=$nm
    out=$(g0_mgmt raft-lookup "$sp_ino" pre)
    echo "$out" | grep -q 'status=0' || bad "$tag split-lookup pre not OK"
    out=$(g0_mgmt raft-lookup "$sp_ino" "$nm")
    echo "$out" | grep -q 'status=0' || bad "$tag split-lookup hashed not OK"
    echo "$out" | grep -q "ino=$ino" || bad "$tag split-lookup hashed ino"
    out=$(g0_mgmt raft-readdir "$sp_ino")
    say "$tag split-readdir: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag split-readdir not OK"
    readdir_has "$out" pre || bad "$tag split-readdir missing pre"
    readdir_has "$out" "$nm" || bad "$tag split-readdir missing hashed"
    readdir_has "$out" e || bad "$tag split-readdir missing empty child"
    out=$(g0_mgmt raft-create "$sp_ino" "$nm")
    say "$tag split-create dup: $out"
    echo "$out" | grep -q 'status=2' || bad "$tag split-create dup not EXIST"
    out=$(g0_mgmt raft-lookup-path "/raft-smoke-sp/pre")
    echo "$out" | grep -q 'status=0' || bad "$tag split lookup-path pre not OK"
    out=$(g0_mgmt raft-lookup-path "/raft-smoke-sp/$nm")
    echo "$out" | grep -q 'status=0' || bad "$tag split lookup-path hashed not OK"
    echo "$out" | grep -q "name=$nm" || bad "$tag split lookup-path hashed name"
    out=$(g0_mgmt raft-rename "$sp_ino" "$nm" "$sp_ino" "$nm2")
    say "$tag split-rename hashed dest $nm -> $nm2: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag split-rename hashed dest not OK"
    out=$(g0_mgmt raft-lookup "$sp_ino" "$nm")
    echo "$out" | grep -q 'status=1' || bad "$tag split-rename hashed src not NOT_FOUND"
    out=$(g0_mgmt raft-lookup "$sp_ino" "$nm2")
    echo "$out" | grep -q 'status=0' || bad "$tag split-rename hashed dest lookup"
    echo "$out" | grep -q "ino=$ino" || bad "$tag split-rename hashed dest ino"
    out=$(g0_mgmt raft-rename "$sp_ino" "$nm2" "$sp_ino" q)
    say "$tag split-rename leftover dest: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag split-rename leftover dest not OK"
    out=$(g0_mgmt raft-lookup "$sp_ino" "$nm2")
    echo "$out" | grep -q 'status=1' || bad "$tag split-rename leftover dest src not NOT_FOUND"
    out=$(g0_mgmt raft-lookup "$sp_ino" q)
    echo "$out" | grep -q 'status=0' || bad "$tag split-rename leftover dest lookup"
    echo "$out" | grep -q "ino=$ino" || bad "$tag split-rename leftover dest ino"
    out=$(g0_mgmt raft-rename "$sp_ino" q "$sp_ino" "$nm")
    say "$tag split-rename tombstone dest: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag split-rename tombstone dest not OK"
    out=$(g0_mgmt raft-lookup "$sp_ino" q)
    echo "$out" | grep -q 'status=1' || bad "$tag split-rename q not NOT_FOUND"
    out=$(g0_mgmt raft-lookup "$sp_ino" "$nm")
    echo "$out" | grep -q 'status=0' || bad "$tag split-rename restored hashed lookup"
    echo "$out" | grep -q "ino=$ino" || bad "$tag split-rename restored hashed ino"
    out=$(g0_mgmt raft-lookup "$sp_ino" pre)
    echo "$out" | grep -q 'status=0' || bad "$tag split-rename leftover pre still"
    lnm=$(python3 -c "
parent=int('$sp_ino')
MASK=0xFFF
skip=set('pre e q $nm $nm2'.split())
def dir_lane(name):
    h=2166136261
    for c in name.encode():
        h ^= c
        h = (h * 16777619) & 0xffffffff
    return h % 64
def lane_shard(ino, lane):
    h = ((ino & 0xffffffff) * 2654435761) & 0xffffffff
    stride = 2 * (h & 0x7FF) + 1
    return ((ino & MASK) + lane * stride) & MASK
psh = parent & MASK
for i in range(8192):
    n='l%d' % i
    lane=dir_lane(n)
    dsh=lane_shard(parent, lane)
    if lane==0 or dsh==psh or (dsh & 1)==(psh & 1) or n in skip:
        continue
    print(n)
    break
")
    if [ -n "$lnm" ] && [ -n "$ino" ] && [ "$ino" != "0" ]; then
        out=$(g0_mgmt raft-link "$ino" "$sp_ino" "$lnm")
        say "$tag split-link $lnm: $out"
        echo "$out" | grep -q 'status=0' || bad "$tag split-link not OK"
        echo "$out" | grep -q 'nlink=2' || bad "$tag split-link nlink"
        if echo "$out" | grep -q 'status=0'; then
            SPLIT_LINK_NAME=$lnm
        fi
        out=$(g0_mgmt raft-lookup "$sp_ino" "$lnm")
        echo "$out" | grep -q 'status=0' || bad "$tag split-link lookup not OK"
        echo "$out" | grep -q "ino=$ino" || bad "$tag split-link lookup ino"
        echo "$out" | grep -q 'nlink=2' || bad "$tag split-link lookup nlink"
        out=$(g0_mgmt raft-lookup "$sp_ino" "$nm")
        echo "$out" | grep -q 'nlink=2' || bad "$tag split-link src nlink"
        out=$(g0_mgmt raft-link "$ino" "$sp_ino" "$lnm")
        say "$tag split-link dup: $out"
        echo "$out" | grep -q 'status=2' || bad "$tag split-link dup not EXIST"
        out=$(g0_mgmt raft-link "$ino" "$sp_ino" pre)
        say "$tag split-link leftover dest: $out"
        echo "$out" | grep -q 'status=2' || bad "$tag split-link leftover dest not EXIST"
        out=$(g0_mgmt raft-readdir "$sp_ino")
        readdir_has "$out" "$lnm" || bad "$tag split-link readdir missing"
        out=$(g0_mgmt raft-lookup-path "/raft-smoke-sp/$lnm")
        echo "$out" | grep -q 'status=0' || bad "$tag split-link lookup-path not OK"
        echo "$out" | grep -q "name=$lnm" || bad "$tag split-link lookup-path name"
    else
        bad "$tag split-link dest name"
    fi
    dnm=$(python3 -c "
parent=int('$sp_ino')
MASK=0xFFF
skip=set('pre e q $nm $nm2 ${lnm:-}'.split())
def dir_lane(name):
    h=2166136261
    for c in name.encode():
        h ^= c
        h = (h * 16777619) & 0xffffffff
    return h % 64
def lane_shard(ino, lane):
    h = ((ino & 0xffffffff) * 2654435761) & 0xffffffff
    stride = 2 * (h & 0x7FF) + 1
    return ((ino & MASK) + lane * stride) & MASK
psh = parent & MASK
for i in range(8192):
    n='d%d' % i
    lane=dir_lane(n)
    dsh=lane_shard(parent, lane)
    if lane==0 or dsh==psh or (dsh & 1)==(psh & 1) or n in skip:
        continue
    print(n)
    break
")
    if [ -n "$dnm" ]; then
        out=$(g0_mgmt raft-create "$sp_ino" "$dnm" 040755)
        say "$tag split-mkdir $dnm: $out"
        echo "$out" | grep -q 'status=0' || bad "$tag split-mkdir not OK"
        dino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
        [ -n "$dino" ] && [ "$dino" != "0" ] || bad "$tag split-mkdir ino"
        out=$(g0_mgmt raft-lookup "$sp_ino" "$dnm")
        echo "$out" | grep -q 'status=0' || bad "$tag split-mkdir lookup not OK"
        echo "$out" | grep -q "ino=$dino" || bad "$tag split-mkdir lookup ino"
        out=$(g0_mgmt raft-readdir "$sp_ino")
        readdir_has "$out" "$dnm" || bad "$tag split-mkdir readdir missing"
        out=$(g0_mgmt raft-create "$sp_ino" "$dnm" 040755)
        say "$tag split-mkdir dup: $out"
        echo "$out" | grep -q 'status=2' || bad "$tag split-mkdir dup not EXIST"
        out=$(g0_mgmt raft-rmdir "$sp_ino" "$dnm")
        say "$tag split-mkdir rmdir: $out"
        echo "$out" | grep -q 'status=0' || bad "$tag split-mkdir rmdir not OK"
        out=$(g0_mgmt raft-lookup "$sp_ino" "$dnm")
        echo "$out" | grep -q 'status=1' || bad "$tag split-mkdir rmdir not NOT_FOUND"
        out=$(g0_mgmt raft-create "$sp_ino" "$dnm" 040755)
        say "$tag split-mkdir recreate: $out"
        echo "$out" | grep -q 'status=0' || bad "$tag split-mkdir recreate not OK"
        dino2=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
        { [ -n "$dino2" ] && [ "$dino2" != "0" ] && [ "$dino2" != "$dino" ]; } || bad "$tag split-mkdir recreate ino"
        out=$(g0_mgmt raft-rmdir "$sp_ino" "$dnm")
        echo "$out" | grep -q 'status=0' || bad "$tag split-mkdir rmdir2 not OK"
    else
        bad "$tag split-mkdir dest name"
    fi
    out=$(g0_mgmt raft-unlink "$sp_ino" "$nm")
    say "$tag split-unlink hashed: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag split-unlink hashed not OK"
    out=$(g0_mgmt raft-lookup "$sp_ino" "$nm")
    echo "$out" | grep -q 'status=1' || bad "$tag split-unlink hashed not NOT_FOUND"
    out=$(g0_mgmt raft-readdir "$sp_ino")
    readdir_has "$out" pre || bad "$tag split-readdir lost pre after hashed unlink"
    if readdir_has "$out" "$nm"; then
        bad "$tag split-readdir hashed survived unlink"
    fi
    if [ -n "$lnm" ] && [ -n "$ino" ] && [ "$ino" != "0" ]; then
        out=$(g0_mgmt raft-link "$ino" "$sp_ino" "$nm")
        say "$tag split-link tombstone dest: $out"
        echo "$out" | grep -q 'status=0' || bad "$tag split-link tombstone dest not OK"
        echo "$out" | grep -q 'nlink=2' || bad "$tag split-link tombstone dest nlink"
        out=$(g0_mgmt raft-lookup "$sp_ino" "$nm")
        echo "$out" | grep -q 'status=0' || bad "$tag split-link tombstone dest lookup"
        echo "$out" | grep -q "ino=$ino" || bad "$tag split-link tombstone dest ino"
        out=$(g0_mgmt raft-unlink "$sp_ino" "$nm")
        echo "$out" | grep -q 'status=0' || bad "$tag split-unlink after tombstone dest not OK"
        out=$(g0_mgmt raft-lookup "$sp_ino" "$nm")
        echo "$out" | grep -q 'status=1' || bad "$tag split-unlink after tombstone dest not NOT_FOUND"
    fi
    out=$(g0_mgmt raft-create "$sp_ino" "$nm")
    say "$tag split-recreate hashed: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag split-recreate hashed not OK"
    out=$(g0_mgmt raft-unlink "$sp_ino" "$nm")
    echo "$out" | grep -q 'status=0' || bad "$tag split-unlink hashed again not OK"
    out=$(g0_mgmt raft-unlink "$sp_ino" pre)
    say "$tag split-unlink pre: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag split-unlink pre not OK"
    out=$(g0_mgmt raft-lookup "$sp_ino" pre)
    echo "$out" | grep -q 'status=1' || bad "$tag split-unlink pre not NOT_FOUND"
    out=$(g0_mgmt raft-rmdir "$sp_ino" e)
    say "$tag split-rmdir e: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag split-rmdir e not OK"
    out=$(g0_mgmt raft-lookup "$sp_ino" e)
    echo "$out" | grep -q 'status=1' || bad "$tag split-rmdir e not NOT_FOUND"
    out=$(g0_mgmt raft-readdir "$sp_ino")
    if readdir_has "$out" pre || readdir_has "$out" "$nm" || readdir_has "$out" e; then
        bad "$tag split-readdir leftover after I8"
    fi
    if [ -n "${SPLIT_LINK_NAME:-}" ]; then
        readdir_has "$out" "$SPLIT_LINK_NAME" || bad "$tag split-readdir lost dest link"
    fi
    out=$(g0_mgmt raft-rmdir 1 raft-smoke-sp)
    say "$tag split-rmdir self: $out"
    echo "$out" | grep -q 'status=5' || bad "$tag split-rmdir self not BUSY"
    SPLIT_PRE_GONE=$SPLIT_PRE
    SPLIT_FILE_GONE=$nm
    SPLIT_EMPTY_GONE=$SPLIT_EMPTY
    SPLIT_PRE=""
    SPLIT_FILE=""
    SPLIT_EMPTY=""
}

# Populated LOCAL leftovers whose HASHED shard is on the other group
# migrate as a 2-shard txn. Same-group leftovers stay DIR_MIGRATE.
# Dedicated raft-smoke-mg so raft-smoke-sp can stay SPLITTING.
check_migrate_populated() {
    local lid=$1
    local tag=$2
    local out mg_ino xn i st
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    leader=$lid
    out=$(g0_mgmt raft-create 1 raft-smoke-mg 040755)
    say "$tag migrate-dir mkdir: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag migrate-dir mkdir not OK"
    mg_ino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$mg_ino" ] && [ "$mg_ino" != "0" ] || { bad "$tag migrate-dir ino"; return; }
    MIGRATE_DIR=raft-smoke-mg
    MIGRATE_DIR_INO=$mg_ino
    out=$(g0_mgmt raft-create "$mg_ino" pre)
    echo "$out" | grep -q 'status=0' || bad "$tag migrate pre not OK"
    xn=$(python3 -c "
parent=int('$mg_ino')
MASK=0xFFF
def dir_lane(name):
    h=2166136261
    for c in name.encode():
        h ^= c
        h = (h * 16777619) & 0xffffffff
    return h % 64
def lane_shard(ino, lane):
    h = ((ino & 0xffffffff) * 2654435761) & 0xffffffff
    stride = 2 * (h & 0x7FF) + 1
    return ((ino & MASK) + lane * stride) & MASK
psh = parent & MASK
for i in range(8192):
    n='x%d' % i
    lane=dir_lane(n)
    dsh=lane_shard(parent, lane)
    if lane==0 or (dsh & 1)==(psh & 1):
        continue
    print(n)
    break
")
    [ -n "$xn" ] || { bad "$tag migrate cross-group name"; return; }
    out=$(g0_mgmt raft-create "$mg_ino" "$xn")
    say "$tag migrate leftover $xn: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag migrate leftover not OK"
    MIGRATE_FILE=$xn
    out=$(g0_mgmt raft-dir "$mg_ino" begin)
    say "$tag migrate begin: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag migrate begin not OK"
    st=0
    for i in $(seq 1 32); do
        out=$(g0_mgmt raft-dir "$mg_ino" migrate)
        say "$tag migrate step $i: $out"
        if echo "$out" | grep -q 'status=0'; then
            continue
        fi
        st=1
        break
    done
    [ "$st" = 1 ] || bad "$tag migrate did not finish"
    echo "$out" | grep -q 'status=1' || bad "$tag migrate end not NOT_FOUND"
    out=$(g0_mgmt raft-dir "$mg_ino" finish)
    say "$tag migrate finish: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag migrate finish not OK"
    out=$(g0_mgmt raft-lookup "$mg_ino" pre)
    echo "$out" | grep -q 'status=0' || bad "$tag migrate lookup pre not OK"
    out=$(g0_mgmt raft-lookup "$mg_ino" "$xn")
    echo "$out" | grep -q 'status=0' || bad "$tag migrate lookup leftover not OK"
    out=$(g0_mgmt raft-readdir "$mg_ino")
    readdir_has "$out" pre || bad "$tag migrate readdir missing pre"
    readdir_has "$out" "$xn" || bad "$tag migrate readdir missing leftover"
}

# Session record + register + establish (10.5c-35a, I23 groundwork).
# CREATE/REGISTER live on hash(uuid); ESTABLISH on the named shard.
# GET is a ReadIndex (not a log cmd). Survives crash as ACTIVE with
# the registered bit. HOLD/FLOCK identity stays the uint64 stand-in.
SESS_UUID=aa000000000000000000000000000001
SESS_EPOCH=1
check_session() {
    local lid=$1
    local tag=$2
    local out ssh
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    leader=$lid
    out=$(g0_mgmt raft-session get "$SESS_UUID")
    say "$tag session get-miss: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag session get-miss not NOT_FOUND"
    out=$(g0_mgmt raft-session create "$SESS_UUID" 1)
    say "$tag session create: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag session create not OK"
    ssh=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ssh=/) {split($i,a,"="); print a[2]}}')
    [ -n "$ssh" ] || { bad "$tag session ssh"; return; }
    SESS_SHARD=$ssh
    out=$(g0_mgmt raft-session register "$SESS_UUID" "$ssh" 1)
    say "$tag session register: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag session register not OK"
    out=$(g0_mgmt raft-session establish "$SESS_UUID" "$ssh" 1)
    say "$tag session establish: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag session establish not OK"
    out=$(g0_mgmt raft-session get "$SESS_UUID" "$ssh")
    say "$tag session get: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag session get not OK"
    echo "$out" | grep -q 'epoch=1' || bad "$tag session epoch"
    echo "$out" | grep -q 'state=1' || bad "$tag session not ACTIVE"
    echo "$out" | grep -q 'touched=1' || bad "$tag session touched bit"
    out=$(g0_mgmt raft-session create "$SESS_UUID" 1)
    say "$tag session create-dup: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag session create-dup not idempotent"
}

# Real (uuid, epoch) on HOLD/FLOCK (10.5c-35b). Optional wire suffix;
# stand-in path (no suffix) stays for the later HOLD/FLOCK checks.
# Accept rejects a not-established epoch. Dedicated raft-smoke-u,
# unlinked before crash so ROOT readdir stays the same.
check_sess_ident() {
    local lid=$1
    local tag=$2
    local out ino ish
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    if [ -z "${SESS_UUID:-}" ]; then
        bad "$tag: no session uuid"
        return
    fi
    leader=$lid
    out=$(g0_mgmt raft-create 1 raft-smoke-u)
    say "$tag sess-ident create: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag sess-ident create not OK"
    ino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$ino" ] && [ "$ino" != "0" ] || { bad "$tag sess-ident ino"; return; }
    ish=$((ino & 4095))
    out=$(g0_mgmt raft-session register "$SESS_UUID" "$ish" 1)
    say "$tag sess-ident register inode-shard: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag sess-ident register not OK"
    out=$(g0_mgmt raft-session establish "$SESS_UUID" "$ish" 1)
    say "$tag sess-ident establish inode-shard: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag sess-ident establish not OK"
    out=$(g0_mgmt raft-hold "$ino" open 1 "$SESS_UUID" 1)
    say "$tag sess-ident hold open: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag sess-ident hold open not OK"
    out=$(g0_mgmt raft-hold "$ino" open 1 "$SESS_UUID" 2)
    say "$tag sess-ident hold wrong-epoch: $out"
    echo "$out" | grep -q 'status=5' || bad "$tag sess-ident hold wrong-epoch not BUSY"
    out=$(g0_mgmt raft-flock "$ino" ex 1 "$SESS_UUID" 1)
    say "$tag sess-ident flock ex: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag sess-ident flock ex not OK"
    out=$(g0_mgmt raft-flock "$ino" ex 2)
    say "$tag sess-ident flock stand-in busy: $out"
    echo "$out" | grep -q 'status=5' || bad "$tag sess-ident flock stand-in not BUSY"
    out=$(g0_mgmt raft-flock "$ino" un 1 "$SESS_UUID" 1)
    say "$tag sess-ident flock un: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag sess-ident flock un not OK"
    out=$(g0_mgmt raft-hold "$ino" close 1 "$SESS_UUID" 1)
    say "$tag sess-ident hold close: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag sess-ident hold close not OK"
    out=$(g0_mgmt raft-unlink 1 raft-smoke-u)
    say "$tag sess-ident unlink: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag sess-ident unlink not OK"
}

# Revocation barrier (10.5c-35c) + append reclaim (10.5c-35d, I23).
# Coordinator-driven: BEGIN →
# FENCE_LOC+ACK every touched shard → FINISH → LEASE_DROP. A queued
# waiter of the old epoch is dequeued (BUSY/STALE) and never granted.
# A pending O_APPEND reserve of that epoch resolves as a FENCED_HOLE.
# Dedicated raft-smoke-z, unlinked before crash. Session epoch becomes 2.
check_sess_fence() {
    local lid=$1
    local tag=$2
    local out ino ish wp i ok
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    if [ -z "${SESS_UUID:-}" ]; then
        bad "$tag: no session uuid"
        return
    fi
    leader=$lid
    out=$(g0_mgmt raft-create 1 raft-smoke-z)
    say "$tag sess-fence create: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag sess-fence create not OK"
    ino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$ino" ] && [ "$ino" != "0" ] || { bad "$tag sess-fence ino"; return; }
    ish=$((ino & 4095))
    out=$(g0_mgmt raft-session register "$SESS_UUID" "$ish" 1)
    say "$tag sess-fence register inode-shard: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag sess-fence register not OK"
    out=$(g0_mgmt raft-session establish "$SESS_UUID" "$ish" 1)
    say "$tag sess-fence establish inode-shard: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag sess-fence establish not OK"
    out=$(g0_mgmt raft-append "$ino" 131072 "$SESS_UUID" 1)
    say "$tag sess-fence append epoch1: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag sess-fence append not OK"
    echo "$out" | grep -q 'size=131072' || bad "$tag sess-fence append watermark"
    out=$(g0_mgmt raft-getattr "$ino")
    say "$tag sess-fence getattr after reserve: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag sess-fence getattr after reserve not OK"
    echo "$out" | grep -q 'size=0' || bad "$tag sess-fence getattr not frontier 0"
    out=$(g0_mgmt raft-fcntl "$ino" ex 1 0 100 "$SESS_UUID" 1)
    say "$tag sess-fence ex owner1: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag sess-fence ex owner1 not OK"
    FENCE_WAIT_OUT=/tmp/raft-smoke-fence-w-$$
    rm -f "$FENCE_WAIT_OUT"
    ( G0_TO=30 G0_TRIES=4 g0_mgmt raft-fcntl "$ino" wex 2 0 100 "$SESS_UUID" 1 >"$FENCE_WAIT_OUT" 2>&1 ) &
    wp=$!
    sleep 2
    if kill -0 $wp 2>/dev/null; then
        say "$tag sess-fence waiter owner2 pending"
    else
        bad "$tag sess-fence waiter returned while conflicted"
        say "$tag sess-fence waiter out: $(cat "$FENCE_WAIT_OUT" 2>/dev/null)"
    fi
    out=$(G0_TO=30 g0_mgmt raft-session fence "$SESS_UUID")
    say "$tag sess-fence barrier: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag sess-fence barrier not OK"
    echo "$out" | grep -q 'epoch=2' || bad "$tag sess-fence epoch not 2"
    echo "$out" | grep -q 'state=1' || bad "$tag sess-fence not ACTIVE"
    out=$(g0_mgmt raft-getattr "$ino")
    say "$tag sess-fence getattr after fence: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag sess-fence getattr after fence not OK"
    echo "$out" | grep -q 'size=131072' || bad "$tag sess-fence reservation not FENCED_HOLE"
    out=$(g0_mgmt raft-append "$ino" 4096 "$SESS_UUID" 1)
    say "$tag sess-fence old-epoch append: $out"
    echo "$out" | grep -q 'status=5' || bad "$tag sess-fence old-epoch append not BUSY"
    ok=0
    for i in $(seq 1 50); do
        if ! kill -0 $wp 2>/dev/null; then ok=1; break; fi
        sleep 0.2
    done
    [ "$ok" = 1 ] || { bad "$tag sess-fence waiter stuck after fence"; kill $wp 2>/dev/null; }
    wait $wp 2>/dev/null || true
    say "$tag sess-fence waiter out: $(cat "$FENCE_WAIT_OUT" 2>/dev/null)"
    grep -q 'status=5' "$FENCE_WAIT_OUT" || bad "$tag sess-fence waiter not BUSY"
    out=$(g0_mgmt raft-fcntl "$ino" ex 3 0 100 "$SESS_UUID" 1)
    say "$tag sess-fence old-epoch ex: $out"
    echo "$out" | grep -q 'status=5' || bad "$tag sess-fence old-epoch not BUSY"
    out=$(g0_mgmt raft-session establish "$SESS_UUID" "$ish" 2)
    say "$tag sess-fence establish epoch2: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag sess-fence establish epoch2 not OK"
    out=$(g0_mgmt raft-fcntl "$ino" ex 3 0 100 "$SESS_UUID" 2)
    say "$tag sess-fence new-epoch ex: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag sess-fence new-epoch not OK"
    out=$(g0_mgmt raft-fcntl "$ino" un 3 0 100 "$SESS_UUID" 2)
    say "$tag sess-fence new-epoch un: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag sess-fence new-epoch un not OK"
    out=$(g0_mgmt raft-unlink 1 raft-smoke-z)
    say "$tag sess-fence unlink: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag sess-fence unlink not OK"
    SESS_EPOCH=2
    rm -f "$FENCE_WAIT_OUT"
}

# Open-unlinked HOLD lease on the inode shard (I19). Dedicated
# raft-smoke-k (not a substring of raft-smoke-h / raft-smoke-hd).
# Last-link unlink keeps the inode at nlink=0 while the lease is
# live; close after crash reclaims. Sessions are not hosted: owner
# is the stand-in (default 1). ROOT nlink is unchanged (create+unlink).
check_hold() {
    local lid=$1
    local tag=$2
    local out
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    leader=$lid
    HOLD_NAME=raft-smoke-k
    out=$(g0_mgmt raft-create 1 "$HOLD_NAME")
    lid=$leader
    say "$tag hold-prep create: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag hold-prep create not OK"
    HOLD_INO=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$HOLD_INO" ] && [ "$HOLD_INO" != "0" ] || { bad "$tag hold-prep ino"; return; }
    out=$(g0_mgmt raft-hold "$HOLD_INO" open)
    say "$tag hold open: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag hold open not OK"
    out=$(g0_mgmt raft-unlink 1 "$HOLD_NAME")
    say "$tag hold unlink: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag hold unlink not OK"
    out=$(g0_mgmt raft-lookup 1 "$HOLD_NAME")
    say "$tag hold lookup unlinked: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag hold lookup unlinked not NOT_FOUND"
    out=$(g0_mgmt raft-getattr "$HOLD_INO")
    say "$tag hold getattr nlink0: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag hold getattr not OK"
    echo "$out" | grep -q 'nlink=0' || bad "$tag hold getattr not nlink=0"
    out=$(g0_mgmt raft-hold 0 open)
    say "$tag hold ino0: $out"
    echo "$out" | grep -q 'status=6' || bad "$tag hold ino0 not INVAL"
    out=$(g0_mgmt raft-hold 999999999 open)
    say "$tag hold miss: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag hold miss not NOT_FOUND"
}

# Non-blocking flock on the inode shard. Dedicated raft-smoke-w (not a
# substring of -h / -hd / -k / -c). Conflict is BUSY; leave owner=2 EX
# held through crash. ROOT nlink is unchanged (file create). File stays
# in READDIR. Blocking wait / ranges / F_GETLK are not hosted.
check_flock() {
    local lid=$1
    local tag=$2
    local out
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    leader=$lid
    FLOCK_NAME=raft-smoke-w
    out=$(g0_mgmt raft-create 1 "$FLOCK_NAME")
    lid=$leader
    say "$tag flock-prep create: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag flock-prep create not OK"
    FLOCK_INO=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$FLOCK_INO" ] && [ "$FLOCK_INO" != "0" ] || { bad "$tag flock-prep ino"; return; }
    out=$(g0_mgmt raft-flock "$FLOCK_INO" ex 1)
    say "$tag flock ex owner1: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag flock ex owner1 not OK"
    out=$(g0_mgmt raft-flock "$FLOCK_INO" ex 2)
    say "$tag flock ex owner2 busy: $out"
    echo "$out" | grep -q 'status=5' || bad "$tag flock ex owner2 not BUSY"
    out=$(g0_mgmt raft-flock "$FLOCK_INO" un 1)
    say "$tag flock un owner1: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag flock un owner1 not OK"
    out=$(g0_mgmt raft-flock "$FLOCK_INO" ex 2)
    say "$tag flock ex owner2: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag flock ex owner2 not OK"
    out=$(g0_mgmt raft-flock 0 ex)
    say "$tag flock ino0: $out"
    echo "$out" | grep -q 'status=6' || bad "$tag flock ino0 not INVAL"
    out=$(g0_mgmt raft-flock 999999999 ex)
    say "$tag flock miss: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag flock miss not NOT_FOUND"
}

# Non-blocking whole-file fcntl on the same FLOCK opcode with
# EFS_FLOCK_FCNTL. Dedicated raft-smoke-c (not a substring of -h / -hd /
# -k / -w). Same-domain conflict is BUSY; flock EX on the same file is
# the other domain and must succeed. Leave owner=1 EX held through crash.
# ROOT nlink is unchanged. Ranges / F_GETLK / blocking wait are not hosted.
check_fcntl() {
    local lid=$1
    local tag=$2
    local out
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    leader=$lid
    FCNTL_NAME=raft-smoke-c
    out=$(g0_mgmt raft-create 1 "$FCNTL_NAME")
    lid=$leader
    say "$tag fcntl-prep create: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag fcntl-prep create not OK"
    FCNTL_INO=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$FCNTL_INO" ] && [ "$FCNTL_INO" != "0" ] || { bad "$tag fcntl-prep ino"; return; }
    out=$(g0_mgmt raft-fcntl "$FCNTL_INO" ex 1)
    say "$tag fcntl ex owner1: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag fcntl ex owner1 not OK"
    out=$(g0_mgmt raft-fcntl "$FCNTL_INO" ex 2)
    say "$tag fcntl ex owner2 busy: $out"
    echo "$out" | grep -q 'status=5' || bad "$tag fcntl ex owner2 not BUSY"
    out=$(g0_mgmt raft-flock "$FCNTL_INO" ex 2)
    say "$tag fcntl cross-domain flock: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag fcntl cross-domain flock not OK"
    out=$(g0_mgmt raft-flock "$FCNTL_INO" un 2)
    say "$tag fcntl cross-domain flock un: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag fcntl cross-domain flock un not OK"
    out=$(g0_mgmt raft-fcntl "$FCNTL_INO" un 1)
    say "$tag fcntl un owner1: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag fcntl un owner1 not OK"
    out=$(g0_mgmt raft-fcntl "$FCNTL_INO" ex 1)
    say "$tag fcntl ex owner1 held: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag fcntl ex owner1 held not OK"
    out=$(g0_mgmt raft-fcntl 0 ex)
    say "$tag fcntl ino0: $out"
    echo "$out" | grep -q 'status=6' || bad "$tag fcntl ino0 not INVAL"
    out=$(g0_mgmt raft-fcntl 999999999 ex)
    say "$tag fcntl miss: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag fcntl miss not NOT_FOUND"
}

# Non-blocking fcntl byte ranges (half-open). Dedicated raft-smoke-t.
# Adjacent ranges do not conflict; overlap is BUSY. Flock domain
# rejects a range (INVAL). Inverted start>=end is INVAL. Leave
# owner=2 EX [100,200) through crash. Blocking wait not hosted.
check_fcntl_range() {
    local lid=$1
    local tag=$2
    local out
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    leader=$lid
    RANGE_NAME=raft-smoke-t
    out=$(g0_mgmt raft-create 1 "$RANGE_NAME")
    lid=$leader
    say "$tag range-prep create: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag range-prep create not OK"
    RANGE_INO=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$RANGE_INO" ] && [ "$RANGE_INO" != "0" ] || { bad "$tag range-prep ino"; return; }
    out=$(g0_mgmt raft-fcntl "$RANGE_INO" ex 1 0 100)
    say "$tag range ex [0,100) owner1: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag range ex [0,100) owner1 not OK"
    out=$(g0_mgmt raft-fcntl "$RANGE_INO" ex 2 100 200)
    say "$tag range ex [100,200) owner2: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag range adjacent owner2 not OK"
    out=$(g0_mgmt raft-fcntl "$RANGE_INO" gex 3 50 150)
    say "$tag range gex [50,150) owner3: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag range gex overlap not OK"
    echo "$out" | grep -q 'type=ex' || bad "$tag range gex type"
    echo "$out" | grep -q 'owner=1' || bad "$tag range gex owner"
    echo "$out" | grep -q 'start=0' || bad "$tag range gex start"
    echo "$out" | grep -q 'end=100' || bad "$tag range gex end"
    out=$(g0_mgmt raft-fcntl "$RANGE_INO" gex 2 100 200)
    say "$tag range gex own [100,200): $out"
    echo "$out" | grep -q 'status=0' || bad "$tag range gex own not OK"
    echo "$out" | grep -q 'type=un' || bad "$tag range gex own not UNLCK"
    out=$(g0_mgmt raft-fcntl "$RANGE_INO" gex 3 200 300)
    say "$tag range gex [200,300) free: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag range gex free not OK"
    echo "$out" | grep -q 'type=un' || bad "$tag range gex free not UNLCK"
    out=$(g0_mgmt raft-fcntl 0 gex)
    say "$tag range gex ino0: $out"
    echo "$out" | grep -q 'status=6' || bad "$tag range gex ino0 not INVAL"
    out=$(g0_mgmt raft-fcntl 999999999 gex)
    say "$tag range gex miss: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag range gex miss not NOT_FOUND"
    out=$(g0_mgmt raft-fcntl "$RANGE_INO" ex 3 50 150)
    say "$tag range ex [50,150) busy: $out"
    echo "$out" | grep -q 'status=5' || bad "$tag range overlap not BUSY"
    out=$(g0_mgmt raft-fcntl "$RANGE_INO" un 1 0 100)
    say "$tag range un [0,100) owner1: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag range un owner1 not OK"
    out=$(g0_mgmt raft-fcntl "$RANGE_INO" ex 3 0 100)
    say "$tag range ex [0,100) owner3: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag range disjoint owner3 not OK"
    out=$(g0_mgmt raft-fcntl "$RANGE_INO" un 3 0 100)
    say "$tag range un [0,100) owner3: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag range un owner3 not OK"
    out=$(g0_mgmt raft-fcntl "$RANGE_INO" ex 1 200 100)
    say "$tag range inverted: $out"
    echo "$out" | grep -q 'status=6' || bad "$tag range inverted not INVAL"
    out=$(g0_mgmt raft-flock "$RANGE_INO" ex 1 0 100)
    say "$tag range flock-domain: $out"
    echo "$out" | grep -q 'status=6' || bad "$tag range flock-domain not INVAL"
}

# Blocking lock waits (EFS_FLOCK_WAIT). Dedicated raft-smoke-q. A
# conflicting grant pends until the holder releases; the held RPC's
# reply IS the grant. FIFO: a queued EX blocks a later SH (no
# starvation). WAIT with an inverted range or a flock-domain range is
# INVAL; a missing inode is NOT_FOUND. Leaves owner=5 EX [500,600)
# held through crash; a second waiter (owner=8) is started before the
# leader kill and must re-issue on the new leader and grant after the
# release.
check_lock_wait() {
    local lid=$1
    local tag=$2
    local out wp2 wp3 i ok
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    leader=$lid
    WAIT_NAME=raft-smoke-q
    out=$(g0_mgmt raft-create 1 "$WAIT_NAME")
    lid=$leader
    say "$tag wait-prep create: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag wait-prep create not OK"
    WAIT_INO=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$WAIT_INO" ] && [ "$WAIT_INO" != "0" ] || { bad "$tag wait-prep ino"; return; }
    WAIT2_OUT=/tmp/raft-smoke-wq2-$$
    WAIT3_OUT=/tmp/raft-smoke-wq3-$$
    WAIT8_OUT=/tmp/raft-smoke-wq8-$$
    rm -f "$WAIT2_OUT" "$WAIT3_OUT" "$WAIT8_OUT"
    out=$(g0_mgmt raft-fcntl "$WAIT_INO" ex 1 0 100)
    say "$tag wait ex [0,100) owner1: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag wait ex owner1 not OK"
    ( G0_TO=30 G0_TRIES=3 g0_mgmt raft-fcntl "$WAIT_INO" wex 2 50 150 >"$WAIT2_OUT" 2>&1 ) &
    wp2=$!
    sleep 2
    if kill -0 $wp2 2>/dev/null; then
        say "$tag wait wex [50,150) owner2 pending"
    else
        bad "$tag wait wex owner2 returned while conflicted"
    fi
    ( G0_TO=30 G0_TRIES=3 g0_mgmt raft-fcntl "$WAIT_INO" wsh 3 50 150 >"$WAIT3_OUT" 2>&1 ) &
    wp3=$!
    sleep 1
    out=$(g0_mgmt raft-fcntl "$WAIT_INO" un 1 0 100)
    say "$tag wait un owner1: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag wait un owner1 not OK"
    ok=0
    for i in $(seq 1 50); do
        if ! kill -0 $wp2 2>/dev/null; then ok=1; break; fi
        sleep 0.2
    done
    [ "$ok" = 1 ] || bad "$tag wait wex owner2 stuck after release"
    wait $wp2 2>/dev/null || true
    say "$tag wait wex owner2 out: $(cat "$WAIT2_OUT" 2>/dev/null)"
    grep -q 'status=0' "$WAIT2_OUT" || bad "$tag wait wex owner2 not granted"
    if kill -0 $wp3 2>/dev/null; then
        say "$tag wait wsh owner3 still pending behind EX (FIFO)"
    else
        bad "$tag wait wsh owner3 barged past queued EX"
    fi
    out=$(g0_mgmt raft-fcntl "$WAIT_INO" sh 4 60 140)
    say "$tag wait sh [60,140) owner4 busy: $out"
    echo "$out" | grep -q 'status=5' || bad "$tag wait sh owner4 not BUSY"
    out=$(g0_mgmt raft-fcntl "$WAIT_INO" un 2 50 150)
    say "$tag wait un owner2: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag wait un owner2 not OK"
    ok=0
    for i in $(seq 1 50); do
        if ! kill -0 $wp3 2>/dev/null; then ok=1; break; fi
        sleep 0.2
    done
    [ "$ok" = 1 ] || bad "$tag wait wsh owner3 stuck after release"
    wait $wp3 2>/dev/null || true
    say "$tag wait wsh owner3 out: $(cat "$WAIT3_OUT" 2>/dev/null)"
    grep -q 'status=0' "$WAIT3_OUT" || bad "$tag wait wsh owner3 not granted"
    out=$(g0_mgmt raft-fcntl "$WAIT_INO" un 3 50 150)
    say "$tag wait un owner3: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag wait un owner3 not OK"
    out=$(g0_mgmt raft-fcntl "$WAIT_INO" wex 1 200 100)
    say "$tag wait inverted: $out"
    echo "$out" | grep -q 'status=6' || bad "$tag wait inverted not INVAL"
    out=$(g0_mgmt raft-flock "$WAIT_INO" wex 1 0 100)
    say "$tag wait flock-domain range: $out"
    echo "$out" | grep -q 'status=6' || bad "$tag wait flock-domain range not INVAL"
    out=$(g0_mgmt raft-fcntl 0 wex 1)
    say "$tag wait ino0: $out"
    echo "$out" | grep -q 'status=6' || bad "$tag wait ino0 not INVAL"
    out=$(g0_mgmt raft-fcntl 999999999 wex 1)
    say "$tag wait miss: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag wait miss not NOT_FOUND"
    out=$(g0_mgmt raft-fcntl "$WAIT_INO" ex 5 500 600)
    say "$tag wait ex [500,600) owner5 (held through crash): $out"
    echo "$out" | grep -q 'status=0' || bad "$tag wait ex owner5 not OK"
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
    # Truncate can land on a restarted/bouncing leader; empty is a
    # 10s SSH miss, not INVAL. Use g0_mgmt so NOT_PRIMARY/empty retry.
    out=$(G0_TO=20 G0_TRIES=4 g0_mgmt raft-setattr "$ino" 8 131072)
    lid=$leader
    say "$tag setattr size: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag setattr size not OK"
    echo "$out" | grep -q 'size=131072' || bad "$tag setattr size"
    out=$(g0_mgmt raft-getattr "$ino")
    lid=$leader
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

# O_APPEND: reserve then publish. Dedicated file so raft-smoke-p's
# trunc-0 stays as it is. Visible getattr size is the frontier (0
# until resolve-on-report). Reply size is the watermark.
check_append() {
    local lid=$1
    local tag=$2
    local out ino
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-create ${ADDRS[$lid]}:${PORT} 1 raft-smoke-a" 2>/dev/null || true)
    say "$tag append create: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag append create not OK"
    ino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$ino" ] && [ "$ino" != "0" ] || bad "$tag append ino"
    APPEND_NAME=raft-smoke-a
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-append ${ADDRS[$lid]}:${PORT} $ino 131072" 2>/dev/null || true)
    say "$tag append: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag append not OK"
    echo "$out" | grep -q 'size=131072' || bad "$tag append watermark"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-getattr ${ADDRS[$lid]}:${PORT} $ino" 2>/dev/null || true)
    say "$tag getattr after reserve: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag getattr after reserve not OK"
    echo "$out" | grep -q 'size=0' || bad "$tag getattr after reserve not frontier 0"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-publish ${ADDRS[$lid]}:${PORT} $ino 0 131072" 2>/dev/null || true)
    say "$tag append publish: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag append publish not OK"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-getattr ${ADDRS[$lid]}:${PORT} $ino" 2>/dev/null || true)
    say "$tag getattr after append pub: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag getattr after append pub not OK"
    echo "$out" | grep -q 'size=131072' || bad "$tag getattr after append pub size"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-append ${ADDRS[$lid]}:${PORT} 999999 4096" 2>/dev/null || true)
    say "$tag append miss: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag append miss not NOT_FOUND"
}

# SYMLINK is CREATE S_IFLNK + publish of the target bytes. Dedicated
# name so raft-smoke-f / -p / -a stay. No SYMLINK opcode; no target
# column on the inode row. File create does not bump ROOT nlink.
check_symlink() {
    local lid=$1
    local tag=$2
    local out ino
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-create ${ADDRS[$lid]}:${PORT} 1 raft-smoke-s 0120777" 2>/dev/null || true)
    say "$tag symlink create: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag symlink create not OK"
    echo "$out" | grep -q 'mode=0120777' || bad "$tag symlink mode"
    echo "$out" | grep -q 'nlink=1' || bad "$tag symlink nlink"
    ino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$ino" ] && [ "$ino" != "0" ] || bad "$tag symlink ino"
    SYMLINK_NAME=raft-smoke-s
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-getattr ${ADDRS[$lid]}:${PORT} $ino" 2>/dev/null || true)
    say "$tag getattr empty symlink: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag getattr empty symlink not OK"
    echo "$out" | grep -q 'size=0' || bad "$tag getattr empty symlink size"
    echo "$out" | grep -q 'mode=0120777' || bad "$tag getattr empty symlink mode"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-publish ${ADDRS[$lid]}:${PORT} $ino 0 11" 2>/dev/null || true)
    say "$tag symlink publish: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag symlink publish not OK"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-getattr ${ADDRS[$lid]}:${PORT} $ino" 2>/dev/null || true)
    say "$tag getattr symlink: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag getattr symlink not OK"
    echo "$out" | grep -q 'size=11' || bad "$tag getattr symlink size"
    echo "$out" | grep -q 'mode=0120777' || bad "$tag getattr symlink mode"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-getchunks ${ADDRS[$lid]}:${PORT} $ino" 2>/dev/null || true)
    say "$tag getchunks symlink: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag getchunks symlink not OK"
    echo "$out" | grep -q 'count=1' || bad "$tag getchunks symlink count"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-create ${ADDRS[$lid]}:${PORT} 1 raft-smoke-s 0120777" 2>/dev/null || true)
    say "$tag symlink dup: $out"
    echo "$out" | grep -q 'status=2' || bad "$tag symlink dup not EXIST"
}

# Exact token in raft-readdir names=a,b,c (raft-smoke-h must not match
# raft-smoke-hd).
readdir_has() {
    local names
    names=$(echo "$1" | sed -n 's/.*names=//p')
    echo ",$names," | grep -q ",$2,"
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
    readdir_has "$out" raft-smoke-f || bad "$tag readdir missing f"
    if [ -n "${RENAME_NAME:-}" ]; then
        readdir_has "$out" "$RENAME_NAME" || bad "$tag readdir missing renamed"
    fi
    if [ -n "${RENAME_OLD:-}" ]; then
        readdir_has "$out" "$RENAME_OLD" && bad "$tag readdir still has rename-old"
    fi
    if [ -n "${LINK_NAME:-}" ]; then
        readdir_has "$out" "$LINK_NAME" || bad "$tag readdir missing link"
    fi
    if [ -n "${MKDIR_NAME:-}" ]; then
        readdir_has "$out" "$MKDIR_NAME" || bad "$tag readdir missing mkdir"
    fi
    if [ -n "${XG_NAME:-}" ]; then
        readdir_has "$out" "$XG_NAME" || bad "$tag readdir missing cross-group"
    fi
    if [ -n "${PUBLISH_NAME:-}" ]; then
        readdir_has "$out" "$PUBLISH_NAME" || bad "$tag readdir missing published"
    fi
    if [ -n "${APPEND_NAME:-}" ]; then
        readdir_has "$out" "$APPEND_NAME" || bad "$tag readdir missing append"
    fi
    if [ -n "${SYMLINK_NAME:-}" ]; then
        readdir_has "$out" "$SYMLINK_NAME" || bad "$tag readdir missing symlink"
    fi
    if [ -n "${DIR_RENAME_NAME:-}" ]; then
        readdir_has "$out" "$DIR_RENAME_NAME" || bad "$tag readdir missing dir-renamed"
    fi
    if [ -n "${DIR_RENAME_OLD:-}" ]; then
        readdir_has "$out" "$DIR_RENAME_OLD" && bad "$tag readdir still has dir-rename-old"
    fi
    if [ -n "${HASHED_DIR:-}" ]; then
        readdir_has "$out" "$HASHED_DIR" || bad "$tag readdir missing hashed-dir"
    fi
    if [ -n "${HASHED_DOW:-}" ]; then
        readdir_has "$out" "$HASHED_DOW" || bad "$tag readdir missing hashed-dow"
    fi
    if [ -n "${HASHED_DOW_OLD:-}" ]; then
        readdir_has "$out" "$HASHED_DOW_OLD" && bad "$tag readdir still has hashed-dow-old"
    fi
    if [ -n "${SPLIT_DIR:-}" ]; then
        readdir_has "$out" "$SPLIT_DIR" || bad "$tag readdir missing split-dir"
    fi
    if [ -n "${MIGRATE_DIR:-}" ]; then
        readdir_has "$out" "$MIGRATE_DIR" || bad "$tag readdir missing migrate-dir"
    fi
    if [ -n "${HOLD_NAME:-}" ]; then
        readdir_has "$out" "$HOLD_NAME" && bad "$tag readdir still has hold-unlinked"
    fi
    if [ -n "${FLOCK_NAME:-}" ]; then
        readdir_has "$out" "$FLOCK_NAME" || bad "$tag readdir missing flock"
    fi
    if [ -n "${FCNTL_NAME:-}" ]; then
        readdir_has "$out" "$FCNTL_NAME" || bad "$tag readdir missing fcntl"
    fi
    if [ -n "${RANGE_NAME:-}" ]; then
        readdir_has "$out" "$RANGE_NAME" || bad "$tag readdir missing range"
    fi
    readdir_has "$out" raft-smoke-u && bad "$tag readdir still has unlinked"
    if [ -n "${UNLINK_NLINK_NAME:-}" ]; then
        readdir_has "$out" "$UNLINK_NLINK_NAME" && bad "$tag readdir still has nlink-unlinked"
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
    if [ -n "${DIR_RENAME_NAME:-}" ]; then
        out=$(g0_mgmt raft-lookup-path "/$DIR_RENAME_NAME")
        say "$tag lookup-path dir-renamed: $out"
        echo "$out" | grep -q 'status=0' || bad "$tag lookup-path dir-renamed not OK"
        echo "$out" | grep -q "name=$DIR_RENAME_NAME" || bad "$tag lookup-path dir-renamed name"
    fi
    if [ -n "${DIR_RENAME_OLD:-}" ]; then
        out=$(g0_mgmt raft-lookup-path "/$DIR_RENAME_OLD")
        say "$tag lookup-path dir-rename-old: $out"
        echo "$out" | grep -q 'status=1' || bad "$tag lookup-path dir-rename-old not NOT_FOUND"
    fi
    if [ -n "${HASHED_DIR:-}" ] && [ -n "${HASHED_FILE:-}" ]; then
        out=$(g0_mgmt raft-lookup-path "/$HASHED_DIR/$HASHED_FILE")
        say "$tag lookup-path hashed: $out"
        echo "$out" | grep -q 'status=0' || bad "$tag lookup-path hashed not OK"
        echo "$out" | grep -q "name=$HASHED_FILE" || bad "$tag lookup-path hashed name"
    fi
    if [ -n "${HASHED_DIR:-}" ] && [ -n "${HASHED_LINK_NAME:-}" ]; then
        out=$(g0_mgmt raft-lookup-path "/$HASHED_DIR/$HASHED_LINK_NAME")
        say "$tag lookup-path hashed-link: $out"
        echo "$out" | grep -q 'status=0' || bad "$tag lookup-path hashed-link not OK"
        echo "$out" | grep -q "name=$HASHED_LINK_NAME" || bad "$tag lookup-path hashed-link name"
    fi
    if [ -n "${HASHED_DOW:-}" ]; then
        out=$(g0_mgmt raft-lookup-path "/$HASHED_DOW")
        say "$tag lookup-path hashed-dow: $out"
        echo "$out" | grep -q 'status=0' || bad "$tag lookup-path hashed-dow not OK"
        echo "$out" | grep -q "name=$HASHED_DOW" || bad "$tag lookup-path hashed-dow name"
    fi
    if [ -n "${HASHED_DOW_OLD:-}" ]; then
        out=$(g0_mgmt raft-lookup-path "/$HASHED_DOW_OLD")
        say "$tag lookup-path hashed-dow-old: $out"
        echo "$out" | grep -q 'status=1' || bad "$tag lookup-path hashed-dow-old not NOT_FOUND"
    fi
    if [ -n "${SPLIT_DIR:-}" ]; then
        out=$(g0_mgmt raft-lookup-path "/$SPLIT_DIR")
        say "$tag lookup-path split-dir: $out"
        echo "$out" | grep -q 'status=0' || bad "$tag lookup-path split-dir not OK"
        echo "$out" | grep -q "name=$SPLIT_DIR" || bad "$tag lookup-path split-dir name"
    fi
    if [ -n "${SPLIT_DIR:-}" ] && [ -n "${SPLIT_PRE:-}" ]; then
        out=$(g0_mgmt raft-lookup-path "/$SPLIT_DIR/$SPLIT_PRE")
        say "$tag lookup-path split-pre: $out"
        echo "$out" | grep -q 'status=0' || bad "$tag lookup-path split-pre not OK"
        echo "$out" | grep -q "name=$SPLIT_PRE" || bad "$tag lookup-path split-pre name"
    fi
    if [ -n "${SPLIT_DIR:-}" ] && [ -n "${SPLIT_PRE_GONE:-}" ]; then
        out=$(g0_mgmt raft-lookup-path "/$SPLIT_DIR/$SPLIT_PRE_GONE")
        say "$tag lookup-path split-pre-gone: $out"
        echo "$out" | grep -q 'status=1' || bad "$tag lookup-path split-pre-gone not NOT_FOUND"
    fi
    if [ -n "${SPLIT_DIR:-}" ] && [ -n "${SPLIT_FILE:-}" ]; then
        out=$(g0_mgmt raft-lookup-path "/$SPLIT_DIR/$SPLIT_FILE")
        say "$tag lookup-path split-file: $out"
        echo "$out" | grep -q 'status=0' || bad "$tag lookup-path split-file not OK"
        echo "$out" | grep -q "name=$SPLIT_FILE" || bad "$tag lookup-path split-file name"
    fi
    if [ -n "${SPLIT_DIR:-}" ] && [ -n "${SPLIT_FILE_GONE:-}" ]; then
        out=$(g0_mgmt raft-lookup-path "/$SPLIT_DIR/$SPLIT_FILE_GONE")
        say "$tag lookup-path split-file-gone: $out"
        echo "$out" | grep -q 'status=1' || bad "$tag lookup-path split-file-gone not NOT_FOUND"
    fi
    if [ -n "${SPLIT_DIR:-}" ] && [ -n "${SPLIT_EMPTY_GONE:-}" ]; then
        out=$(g0_mgmt raft-lookup-path "/$SPLIT_DIR/$SPLIT_EMPTY_GONE")
        say "$tag lookup-path split-empty-gone: $out"
        echo "$out" | grep -q 'status=1' || bad "$tag lookup-path split-empty-gone not NOT_FOUND"
    fi
    if [ -n "${SPLIT_DIR:-}" ] && [ -n "${SPLIT_LINK_NAME:-}" ]; then
        out=$(g0_mgmt raft-lookup-path "/$SPLIT_DIR/$SPLIT_LINK_NAME")
        say "$tag lookup-path split-link: $out"
        echo "$out" | grep -q 'status=0' || bad "$tag lookup-path split-link not OK"
        echo "$out" | grep -q "name=$SPLIT_LINK_NAME" || bad "$tag lookup-path split-link name"
    fi
    if [ -n "${MIGRATE_DIR:-}" ]; then
        out=$(g0_mgmt raft-lookup-path "/$MIGRATE_DIR")
        say "$tag lookup-path migrate-dir: $out"
        echo "$out" | grep -q 'status=0' || bad "$tag lookup-path migrate-dir not OK"
        echo "$out" | grep -q "name=$MIGRATE_DIR" || bad "$tag lookup-path migrate-dir name"
    fi
    if [ -n "${MIGRATE_DIR:-}" ] && [ -n "${MIGRATE_FILE:-}" ]; then
        out=$(g0_mgmt raft-lookup-path "/$MIGRATE_DIR/$MIGRATE_FILE")
        say "$tag lookup-path migrate leftover: $out"
        echo "$out" | grep -q 'status=0' || bad "$tag lookup-path migrate leftover not OK"
        echo "$out" | grep -q "name=$MIGRATE_FILE" || bad "$tag lookup-path migrate leftover name"
    fi
    out=$(g0_mgmt raft-lookup-path /no-such-efs-name)
    say "$tag lookup-path miss: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag lookup-path miss not NOT_FOUND"
}

say "build 4 nodes (scratch, not live cluster)"
bfail=0
for h in "${HOSTS[@]}"; do
    say "build $h"
    build_one "$h" || bfail=1
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
say "O_APPEND through Raft (fresh)"
APPEND_NAME=""
check_append "$leader" "fresh"
say "SYMLINK through Raft (fresh)"
SYMLINK_NAME=""
check_symlink "$leader" "fresh"
say "MKDIR through Raft (fresh)"
MKDIR_NAME=""
check_mkdir "$leader" "fresh"
say "cross-group MKDIR from raft_id=0 (fresh)"
XG_NAME=""
check_cross_group
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
say "directory RENAME through Raft (fresh)"
DIR_RENAME_NAME=""
DIR_RENAME_OLD=""
check_dir_rename "$leader" "fresh"
say "HASHED dest CREATE through Raft (fresh)"
HASHED_DIR=""
HASHED_DIR_INO=""
HASHED_FILE=""
HASHED_FILE2=""
HASHED_FILE_OLD=""
HASHED_UNLINK_OLD=""
HASHED_RMDIR_OLD=""
HASHED_LINK_NAME=""
HASHED_DOW=""
HASHED_DOW_OLD=""
HASHED_DOW_INO=""
SPLIT_DIR=""
SPLIT_DIR_INO=""
SPLIT_FILE=""
SPLIT_PRE=""
SPLIT_EMPTY=""
SPLIT_PRE_GONE=""
SPLIT_FILE_GONE=""
SPLIT_EMPTY_GONE=""
SPLIT_LINK_NAME=""
MIGRATE_DIR=""
MIGRATE_DIR_INO=""
MIGRATE_FILE=""
check_hashed_create "$leader" "fresh"
check_migrate_populated "$leader" "fresh"
say "session record through Raft (fresh)"
SESS_SHARD=""
check_session "$leader" "fresh"
say "session identity on HOLD/FLOCK (fresh)"
check_sess_ident "$leader" "fresh"
say "session revocation barrier (fresh)"
check_sess_fence "$leader" "fresh"
say "HOLD open-unlinked through Raft (fresh)"
HOLD_NAME=""
HOLD_INO=""
check_hold "$leader" "fresh"
say "FLOCK grant/release through Raft (fresh)"
FLOCK_NAME=""
FLOCK_INO=""
check_flock "$leader" "fresh"
say "FCNTL grant/release through Raft (fresh)"
FCNTL_NAME=""
FCNTL_INO=""
check_fcntl "$leader" "fresh"
say "FCNTL byte ranges through Raft (fresh)"
RANGE_NAME=""
RANGE_INO=""
check_fcntl_range "$leader" "fresh"
say "FCNTL blocking waits through Raft (fresh)"
WAIT_NAME=""
WAIT_INO=""
WAIT8_PID=""
check_lock_wait "$leader" "fresh"
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
# 10.5c-34: a blocking waiter pending across the leader kill must
# re-issue on the new leader (g0_mgmt retries conn loss / NOT_PRIMARY)
# and grant only after owner=5's surviving lock is released.
if [ -n "${WAIT_INO:-}" ]; then
    # The wait queue is in-memory at the leader and lost on the kill, so the
    # waiter must re-issue on the new leader until granted — a real client
    # retries for as long as it blocks. g0_mgmt's try COUNT is the wrong
    # budget here: tries are fast while no leader is up (election) and slow
    # while pending, so a count exhausts early in the window. Loop on a
    # wall-clock deadline instead; a long per-try window (60s) keeps one live
    # connection pending so the grant lands without orphaning a queued wait
    # (same-owner re-issue is not a conflict, but needn't be exercised here).
    ( deadline=$(( $(date +%s) + 400 )); o=""
      while [ "$(date +%s)" -lt "$deadline" ]; do
          o=$(G0_TO=60 g0_mgmt raft-fcntl "$WAIT_INO" wex 8 550 650 2>/dev/null)
          echo "$o" | grep -q 'status=0' && break
          sleep 1
      done
      printf '%s\n' "$o" ) >"$WAIT8_OUT" 2>&1 &
    WAIT8_PID=$!
    sleep 1
    say "blocking waiter owner=8 pending on leader raft_id=$leader"
fi
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
    # Survivors can still be in an election when this wait ends.
    # Restarting the old leader is the next step; after-crash serving
    # + data checks are the gate, not this intermediate poll.
    say "no new leader yet after kill"
    for qidx in 0 1 2 3; do
        st=$(ssh_to 10 "${HOSTS[$qidx]}" "cd /tmp/efs && ./efs-mgmt raft-status ${ADDRS[$qidx]}:${PORT}" 2>/dev/null || true)
        say "status ${HOSTS[$qidx]}: $st"
    done
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

say "wait group-0 serving after restart"
after_lid="-1"
ready=0
# GETATTR/LOOKUP are the truth (raft-status leader= lags and can hang
# on a restarting voter). Do not sit on g0_leader first.
for i in $(seq 1 40); do
    cur=$(g0_serving)
    if [ -z "$cur" ] || [ "$cur" = "-1" ]; then
        continue
    fi
    after_lid=$cur
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
# HASHED dest LOOKUP needs group 2. A group-0-only leader answers
# BUSY until bounce finds a host that serves that group. Probe all
# four voters (group 2's leader is often raft_id 3). Prefer a node
# that also serves ROOT, so later g0_mgmt bounce works both ways.
if [ -n "${HASHED_DIR:-}" ]; then
    hashed_ok=0
    for i in $(seq 1 80); do
        for idx in 0 1 2 3; do
            out=$(ssh_to 5 "${HOSTS[$idx]}" "cd /tmp/efs && ./efs-mgmt raft-lookup ${ADDRS[$idx]}:${PORT} 1 $HASHED_DIR" 2>/dev/null || true)
            if echo "$out" | grep -q 'status=0'; then
                root=$(ssh_to 5 "${HOSTS[$idx]}" "cd /tmp/efs && ./efs-mgmt raft-getattr ${ADDRS[$idx]}:${PORT} 1" 2>/dev/null || true)
                if echo "$root" | grep -q 'status=0'; then
                    leader=$idx
                    hashed_ok=1
                    say "after-crash hashed-dir serving raft_id=$leader"
                    break 2
                fi
            fi
        done
    done
    [ "$hashed_ok" = 1 ] || say "after-crash hashed-dir still BUSY"
fi
say "ReadIndex GETATTR/LOOKUP (after crash)"
root_nlink=2
[ -n "${MKDIR_NAME:-}" ] && root_nlink=$((root_nlink + 1))
[ -n "${XG_NAME:-}" ] && root_nlink=$((root_nlink + 1))
[ -n "${DIR_RENAME_NAME:-}" ] && root_nlink=$((root_nlink + 1))
[ -n "${HASHED_DIR:-}" ] && root_nlink=$((root_nlink + 1))
[ -n "${HASHED_DOW:-}" ] && root_nlink=$((root_nlink + 1))
[ -n "${SPLIT_DIR:-}" ] && root_nlink=$((root_nlink + 1))
[ -n "${MIGRATE_DIR:-}" ] && root_nlink=$((root_nlink + 1))
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
if [ -n "${APPEND_NAME:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$APPEND_NAME")
    say "after-crash lookup append: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash append name missing"
    aino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$aino" ] && [ "$aino" != "0" ] || bad "after-crash append ino"
    out=$(g0_mgmt raft-getattr "$aino")
    say "after-crash getattr append: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash append getattr missing"
    echo "$out" | grep -q 'size=131072' || bad "after-crash append size lost"
fi
if [ -n "${SYMLINK_NAME:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$SYMLINK_NAME")
    say "after-crash lookup symlink: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash symlink name missing"
    sino=$(echo "$out" | awk '{for(i=1;i<=NF;i++) if($i ~ /^ino=/) {split($i,a,"="); print a[2]}}')
    [ -n "$sino" ] && [ "$sino" != "0" ] || bad "after-crash symlink ino"
    out=$(g0_mgmt raft-getattr "$sino")
    say "after-crash getattr symlink: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash symlink getattr missing"
    echo "$out" | grep -q 'mode=0120777' || bad "after-crash symlink mode lost"
    echo "$out" | grep -q 'size=11' || bad "after-crash symlink size lost"
fi
if [ -n "${MKDIR_NAME:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$MKDIR_NAME")
    say "after-crash lookup mkdir: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash mkdir name missing"
fi
if [ -n "${XG_NAME:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$XG_NAME")
    say "after-crash lookup cross-group: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash cross-group name missing"
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
if [ -n "${DIR_RENAME_NAME:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$DIR_RENAME_NAME")
    say "after-crash lookup dir-renamed: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash dir-renamed name missing"
    echo "$out" | grep -q 'mode=040755' || bad "after-crash dir-renamed mode lost"
fi
if [ -n "${DIR_RENAME_OLD:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$DIR_RENAME_OLD")
    say "after-crash lookup dir-rename-old: $out"
    echo "$out" | grep -q 'status=1' || bad "after-crash dir-rename old name came back"
fi
if [ -n "${HASHED_DIR:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$HASHED_DIR")
    say "after-crash lookup hashed-dir: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash hashed-dir missing"
    echo "$out" | grep -q 'mode=040755' || bad "after-crash hashed-dir mode lost"
fi
if [ -n "${HASHED_DIR_INO:-}" ] && [ -n "${HASHED_FILE:-}" ]; then
    out=$(g0_mgmt raft-lookup "$HASHED_DIR_INO" "$HASHED_FILE")
    say "after-crash lookup hashed-file: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash hashed-file missing"
    if [ -n "${HASHED_LINK_NAME:-}" ]; then
        echo "$out" | grep -q 'nlink=2' || bad "after-crash hashed-file nlink"
    fi
fi
if [ -n "${HASHED_DIR_INO:-}" ] && [ -n "${HASHED_FILE_OLD:-}" ]; then
    out=$(g0_mgmt raft-lookup "$HASHED_DIR_INO" "$HASHED_FILE_OLD")
    say "after-crash lookup hashed-rename-old: $out"
    echo "$out" | grep -q 'status=1' || bad "after-crash hashed-rename old name came back"
fi
if [ -n "${HASHED_DIR_INO:-}" ] && [ -n "${HASHED_FILE2:-}" ]; then
    out=$(g0_mgmt raft-lookup "$HASHED_DIR_INO" "$HASHED_FILE2")
    say "after-crash lookup hashed-file2: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash hashed-file2 missing"
fi
if [ -n "${HASHED_DIR_INO:-}" ] && [ -n "${HASHED_UNLINK_OLD:-}" ]; then
    out=$(g0_mgmt raft-lookup "$HASHED_DIR_INO" "$HASHED_UNLINK_OLD")
    say "after-crash lookup hashed-unlink: $out"
    echo "$out" | grep -q 'status=1' || bad "after-crash hashed-unlink name came back"
fi
if [ -n "${HASHED_DIR_INO:-}" ] && [ -n "${HASHED_LINK_NAME:-}" ]; then
    out=$(g0_mgmt raft-lookup "$HASHED_DIR_INO" "$HASHED_LINK_NAME")
    say "after-crash lookup hashed-link: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash hashed-link missing"
    echo "$out" | grep -q 'nlink=2' || bad "after-crash hashed-link nlink"
fi
if [ -n "${HASHED_RMDIR_OLD:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$HASHED_RMDIR_OLD")
    say "after-crash lookup hashed-rmdir: $out"
    echo "$out" | grep -q 'status=1' || bad "after-crash hashed-rmdir name came back"
fi
if [ -n "${HASHED_DOW:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$HASHED_DOW")
    say "after-crash lookup hashed-dow: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash hashed-dow missing"
    if [ -n "${HASHED_DOW_INO:-}" ]; then
        echo "$out" | grep -q "ino=$HASHED_DOW_INO" || bad "after-crash hashed-dow ino"
    fi
fi
if [ -n "${HASHED_DOW_OLD:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$HASHED_DOW_OLD")
    say "after-crash lookup hashed-dow-old: $out"
    echo "$out" | grep -q 'status=1' || bad "after-crash hashed-dow old name came back"
fi
if [ -n "${SPLIT_DIR:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$SPLIT_DIR")
    say "after-crash lookup split-dir: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash split-dir missing"
    echo "$out" | grep -q 'mode=040755' || bad "after-crash split-dir mode lost"
fi
if [ -n "${SPLIT_DIR_INO:-}" ] && [ -n "${SPLIT_PRE:-}" ]; then
    out=$(g0_mgmt raft-lookup "$SPLIT_DIR_INO" "$SPLIT_PRE")
    say "after-crash lookup split-pre: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash split-pre missing"
fi
if [ -n "${SPLIT_DIR_INO:-}" ] && [ -n "${SPLIT_PRE_GONE:-}" ]; then
    out=$(g0_mgmt raft-lookup "$SPLIT_DIR_INO" "$SPLIT_PRE_GONE")
    say "after-crash lookup split-pre-gone: $out"
    echo "$out" | grep -q 'status=1' || bad "after-crash split-pre came back"
fi
if [ -n "${SPLIT_DIR_INO:-}" ] && [ -n "${SPLIT_FILE:-}" ]; then
    out=$(g0_mgmt raft-lookup "$SPLIT_DIR_INO" "$SPLIT_FILE")
    say "after-crash lookup split-file: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash split-file missing"
fi
if [ -n "${SPLIT_DIR_INO:-}" ] && [ -n "${SPLIT_FILE_GONE:-}" ]; then
    out=$(g0_mgmt raft-lookup "$SPLIT_DIR_INO" "$SPLIT_FILE_GONE")
    say "after-crash lookup split-file-gone: $out"
    echo "$out" | grep -q 'status=1' || bad "after-crash split-file came back"
fi
if [ -n "${SPLIT_DIR_INO:-}" ] && [ -n "${SPLIT_EMPTY_GONE:-}" ]; then
    out=$(g0_mgmt raft-lookup "$SPLIT_DIR_INO" "$SPLIT_EMPTY_GONE")
    say "after-crash lookup split-empty-gone: $out"
    echo "$out" | grep -q 'status=1' || bad "after-crash split-empty came back"
fi
if [ -n "${SPLIT_DIR_INO:-}" ] && [ -n "${SPLIT_LINK_NAME:-}" ]; then
    out=$(g0_mgmt raft-lookup "$SPLIT_DIR_INO" "$SPLIT_LINK_NAME")
    say "after-crash lookup split-link: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash split-link missing"
    echo "$out" | grep -q 'nlink=1' || bad "after-crash split-link nlink"
fi
if [ -n "${MIGRATE_DIR:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$MIGRATE_DIR")
    say "after-crash lookup migrate-dir: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash migrate-dir missing"
    echo "$out" | grep -q 'mode=040755' || bad "after-crash migrate-dir mode lost"
fi
if [ -n "${MIGRATE_DIR_INO:-}" ]; then
    out=$(g0_mgmt raft-lookup "$MIGRATE_DIR_INO" pre)
    say "after-crash lookup migrate-pre: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash migrate-pre missing"
fi
if [ -n "${MIGRATE_DIR_INO:-}" ] && [ -n "${MIGRATE_FILE:-}" ]; then
    out=$(g0_mgmt raft-lookup "$MIGRATE_DIR_INO" "$MIGRATE_FILE")
    say "after-crash lookup migrate leftover: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash migrate leftover missing"
fi
if [ -n "${HOLD_NAME:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$HOLD_NAME")
    say "after-crash lookup hold-unlinked: $out"
    echo "$out" | grep -q 'status=1' || bad "after-crash hold name came back"
fi
if [ -n "${HOLD_INO:-}" ]; then
    out=$(g0_mgmt raft-getattr "$HOLD_INO")
    say "after-crash getattr hold: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash hold inode missing"
    echo "$out" | grep -q 'nlink=0' || bad "after-crash hold nlink not 0"
    out=$(g0_mgmt raft-hold "$HOLD_INO" close)
    say "after-crash hold close: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash hold close not OK"
    out=$(g0_mgmt raft-getattr "$HOLD_INO")
    say "after-crash getattr hold reclaimed: $out"
    echo "$out" | grep -q 'status=1' || bad "after-crash hold close did not reclaim"
fi
if [ -n "${FLOCK_NAME:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$FLOCK_NAME")
    say "after-crash lookup flock: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash flock name missing"
fi
if [ -n "${FLOCK_INO:-}" ]; then
    out=$(g0_mgmt raft-getattr "$FLOCK_INO")
    say "after-crash getattr flock: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash flock inode missing"
    out=$(g0_mgmt raft-flock "$FLOCK_INO" ex 1)
    say "after-crash flock ex owner1 busy: $out"
    echo "$out" | grep -q 'status=5' || bad "after-crash flock lock lost"
    out=$(g0_mgmt raft-flock "$FLOCK_INO" un 2)
    say "after-crash flock un owner2: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash flock un not OK"
    out=$(g0_mgmt raft-flock "$FLOCK_INO" ex 1)
    say "after-crash flock ex owner1: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash flock ex owner1 not OK"
fi
if [ -n "${FCNTL_NAME:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$FCNTL_NAME")
    say "after-crash lookup fcntl: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash fcntl name missing"
fi
if [ -n "${FCNTL_INO:-}" ]; then
    out=$(g0_mgmt raft-getattr "$FCNTL_INO")
    say "after-crash getattr fcntl: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash fcntl inode missing"
    out=$(g0_mgmt raft-fcntl "$FCNTL_INO" ex 2)
    say "after-crash fcntl ex owner2 busy: $out"
    echo "$out" | grep -q 'status=5' || bad "after-crash fcntl lock lost"
    out=$(g0_mgmt raft-fcntl "$FCNTL_INO" un 1)
    say "after-crash fcntl un owner1: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash fcntl un not OK"
    out=$(g0_mgmt raft-fcntl "$FCNTL_INO" ex 2)
    say "after-crash fcntl ex owner2: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash fcntl ex owner2 not OK"
fi
if [ -n "${RANGE_NAME:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$RANGE_NAME")
    say "after-crash lookup range: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash range name missing"
fi
if [ -n "${RANGE_INO:-}" ]; then
    out=$(g0_mgmt raft-getattr "$RANGE_INO")
    say "after-crash getattr range: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash range inode missing"
    out=$(g0_mgmt raft-fcntl "$RANGE_INO" gex 1 100 200)
    say "after-crash range gex [100,200) owner1: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash range gex not OK"
    echo "$out" | grep -q 'type=ex' || bad "after-crash range gex type"
    echo "$out" | grep -q 'owner=2' || bad "after-crash range gex owner"
    echo "$out" | grep -q 'start=100' || bad "after-crash range gex start"
    echo "$out" | grep -q 'end=200' || bad "after-crash range gex end"
    out=$(g0_mgmt raft-fcntl "$RANGE_INO" ex 1 100 200)
    say "after-crash range ex [100,200) owner1 busy: $out"
    echo "$out" | grep -q 'status=5' || bad "after-crash range lock lost"
    out=$(g0_mgmt raft-fcntl "$RANGE_INO" ex 1 0 100)
    say "after-crash range ex [0,100) owner1: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash range disjoint not OK"
    out=$(g0_mgmt raft-fcntl "$RANGE_INO" un 2 100 200)
    say "after-crash range un [100,200) owner2: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash range un not OK"
    out=$(g0_mgmt raft-fcntl "$RANGE_INO" ex 1 100 200)
    say "after-crash range ex [100,200) owner1: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash range ex owner1 not OK"
fi
if [ -n "${WAIT_NAME:-}" ]; then
    out=$(g0_mgmt raft-lookup 1 "$WAIT_NAME")
    say "after-crash lookup wait: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash wait name missing"
fi
if [ -n "${WAIT_INO:-}" ]; then
    out=$(g0_mgmt raft-getattr "$WAIT_INO")
    say "after-crash getattr wait: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash wait inode missing"
    out=$(g0_mgmt raft-fcntl "$WAIT_INO" ex 9 500 600)
    say "after-crash wait ex [500,600) owner9 busy: $out"
    echo "$out" | grep -q 'status=5' || bad "after-crash wait lock lost"
    if [ -n "${WAIT8_PID:-}" ]; then
        local_ok=0
        if kill -0 $WAIT8_PID 2>/dev/null; then
            say "after-crash waiter owner=8 re-issued and pending"
            local_ok=1
        else
            bad "after-crash waiter owner=8 died (no re-issue)"
        fi
        out=$(g0_mgmt raft-fcntl "$WAIT_INO" un 5 500 600)
        say "after-crash wait un owner5: $out"
        echo "$out" | grep -q 'status=0' || bad "after-crash wait un owner5 not OK"
        if [ "$local_ok" = 1 ]; then
            ok=0
            for i in $(seq 1 50); do
                if ! kill -0 $WAIT8_PID 2>/dev/null; then ok=1; break; fi
                sleep 0.2
            done
            [ "$ok" = 1 ] || { bad "after-crash waiter owner=8 stuck after release"; kill $WAIT8_PID 2>/dev/null; }
            wait $WAIT8_PID 2>/dev/null || true
            say "after-crash waiter owner=8 out: $(cat "$WAIT8_OUT" 2>/dev/null)"
            grep -q 'status=0' "$WAIT8_OUT" || bad "after-crash waiter owner=8 not granted"
            out=$(g0_mgmt raft-fcntl "$WAIT_INO" un 8 550 650)
            say "after-crash wait un owner8: $out"
            echo "$out" | grep -q 'status=0' || bad "after-crash wait un owner8 not OK"
        fi
    fi
    rm -f "${WAIT2_OUT:-/dev/null}" "${WAIT3_OUT:-/dev/null}" "${WAIT8_OUT:-/dev/null}"
fi
if [ -n "${SESS_SHARD:-}" ]; then
    # GET is ReadIndex on hash(uuid). BUSY (rc=-13, status=3) means
    # that group has not finished electing; not a missing record.
    out=""
    for i in $(seq 1 40); do
        out=$(g0_mgmt raft-session get "$SESS_UUID" "$SESS_SHARD")
        echo "$out" | grep -q 'status=0' && break
        echo "$out" | grep -qE 'status=3|status=5|status=7|rc=-13' || break
    done
    say "after-crash session get: $out"
    echo "$out" | grep -q 'status=0' || bad "after-crash session missing"
    echo "$out" | grep -q "epoch=${SESS_EPOCH:-1}" || bad "after-crash session epoch"
    echo "$out" | grep -q 'state=1' || bad "after-crash session not ACTIVE"
    echo "$out" | grep -q 'touched=1' || bad "after-crash session touched bit"
fi
say "ReadIndex READDIR/LOOKUP_PATH (after crash)"
check_readdir_path "$leader" "after-crash"

if [ "$FAIL" -eq 0 ]; then
    say "PASS"
    exit 0
fi
say "FAIL count=$FAIL"
exit 1
