#!/bin/bash
# Scratch-cluster gate for the production Raft host (10.5c P1).
#
# Four efsd on fcstor003-006, port 19820, storage under /tmp — NEVER the
# live cluster (port 19810, /data1). Does not pkill -x efsd.
#
# Gate: elect, raft-mkfs lands ROOT on every voter, LOOKUP/GETATTR via
# ReadIndex + KV (leader serves ROOT, follower is NOT_PRIMARY, missing
# name is NOT_FOUND), kill -9 a follower then the leader, restart
# catch-up keeps ROOT and the same reads.
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

# Print group-0 leader raft id (-1 if none). Tries every voter; a just-
# restarted node may not know the leader yet even with ROOT in KV.
g0_leader() {
    local idx st l
    for idx in 0 1 2; do
        st=$(ssh_to 10 "${HOSTS[$idx]}" "cd /tmp/efs && ./efs-mgmt raft-status ${ADDRS[$idx]}:${PORT}" 2>/dev/null || true)
        l=$(echo "$st" | awk '/group 0 / {print $5}' | sed 's/leader=//' | head -1)
        if [ -n "$l" ] && [ "$l" != "-1" ]; then
            echo "$l"
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

# ReadIndex GETATTR/LOOKUP against group-0. Leader must serve ROOT;
# a follower must refuse (NOT_PRIMARY=7); a missing name is NOT_FOUND=1.
check_reads() {
    local lid=$1
    local tag=$2
    local out fid rid
    if [ -z "$lid" ] || [ "$lid" = "-1" ]; then
        bad "$tag: no leader"
        return
    fi
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-getattr ${ADDRS[$lid]}:${PORT} 1" 2>/dev/null || true)
    say "$tag getattr leader=$lid: $out"
    echo "$out" | grep -q 'status=0' || bad "$tag getattr not OK"
    echo "$out" | grep -q 'nlink=2' || bad "$tag getattr nlink"
    echo "$out" | grep -q 'mode=040755' || bad "$tag getattr not dir"
    fid=""
    for rid in 0 1 2; do
        if [ "$rid" != "$lid" ]; then
            fid=$rid
            break
        fi
    done
    out=$(ssh_to 10 "${HOSTS[$fid]}" "cd /tmp/efs && ./efs-mgmt raft-getattr ${ADDRS[$fid]}:${PORT} 1" 2>/dev/null || true)
    say "$tag getattr follower=$fid: $out"
    echo "$out" | grep -q 'status=7' || bad "$tag follower not NOT_PRIMARY"
    out=$(ssh_to 10 "${HOSTS[$lid]}" "cd /tmp/efs && ./efs-mgmt raft-lookup ${ADDRS[$lid]}:${PORT} 1 no-such-efs-name" 2>/dev/null || true)
    say "$tag lookup miss: $out"
    echo "$out" | grep -q 'status=1' || bad "$tag lookup miss not NOT_FOUND"
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

# Pick a group-0 follower to kill (raft ids 0,1,2 minus leader).
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

leader=$(g0_leader)
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
        cand=$(echo "$st" | awk '/group 0 / {print $5}' | sed 's/leader=//' | head -1)
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
say "ReadIndex GETATTR/LOOKUP (after crash)"
check_reads "$leader" "after-crash"

if [ "$FAIL" -eq 0 ]; then
    say "PASS"
    exit 0
fi
say "FAIL count=$FAIL"
exit 1
