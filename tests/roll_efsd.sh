#!/bin/bash
# roll_efsd.sh [node...]      default: 1 2 3 4   (fcstor003..006)
# roll_efsd.sh --all          build all four, STOP all four, START all four
#
# Prefer tests/cluster.sh for a stop or a start. It calls this script's
# --all for the server start and adds the client / perf / strace choice
# and a SIGTERM stop that finalizes perf.
#
# Rolling restart of efsd on the 19810 cluster, one node at a time, per the
# efs-fcstor-deploy rule: rsync + clean build ON the node, `pkill -9 -x efsd`
# (exact name, never -f / never the ps|awk kill-by-port), start detached,
# then wait until every group the node hosts reports commit == applied and
# matches the other voters before touching the next node. Same build ID
# only — after a new commit the HELLO gate rejects mixed IDs, so use --all:
# it builds on all four in parallel, checks the four built IDs agree, kills
# all four, starts all four (storage is kept, so no --join), then waits for
# both groups to elect and report commit == applied on every voter.
#
# Run it from node9901 via efs-bg.sh; each step is a short ssh. Exit 1 on
# the first node that does not come back caught up (do not continue: a
# second node down loses quorum).
set -u
S="$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh"
ALL=0; [ "${1:-}" = "--all" ] && { ALL=1; shift; }
# --all --fresh: storage was wiped (tests/wipe_cluster.sh). Node 1 is the
# seed, nodes 2–4 start with --join, and raft-mkfs runs on node 1 once
# all four are up. Refuses if any /data1/0*/efs on fcstor003 has content.
FRESH=0; [ "${1:-}" = "--fresh" ] && { FRESH=1; shift; }
[ $FRESH = 1 ] && [ $ALL = 0 ] && { echo "--fresh needs --all"; exit 1; }
NODES=("$@"); [ ${#NODES[@]} -gt 0 ] || NODES=(1 2 3 4)
# Environment for the efsd process, e.g. EFSD_ENV='EFS_TRANSPORT=tcp'
# (peer pool + raft AE over TCP instead of the ungated RDMA upgrade).
# EFSD_ARGS is extra argv, e.g. EFSD_ARGS=--perf. With --perf the
# recorder writes the node-local /tmp/efs-perf/efsd.data; with --strace
# the trace goes to /tmp/efs-perf/efsd.strace (EFS_STRACE_EXPR narrows
# it, e.g. 'trace=fsync,fdatasync,pwrite64,futex'; it is forwarded).
EFSD_ENV="${EFSD_ENV:-}"
EFSD_ARGS="${EFSD_ARGS:-}"
PERF_ENV=""
case " $EFSD_ARGS " in
*" --perf "*) PERF_ENV='EFS_PERF_PATH=/tmp/efs-perf/efsd.data' ;;
esac
case " $EFSD_ARGS " in
*" --strace "*)
    PERF_ENV="$PERF_ENV EFS_STRACE_PATH=/tmp/efs-perf/efsd.strace"
    [ -n "${EFS_STRACE_EXPR:-}" ] && PERF_ENV="$PERF_ENV EFS_STRACE_EXPR=$EFS_STRACE_EXPR"
    ;;
esac
STORAGE=/data1/01/efs,/data1/02/efs,/data1/03/efs,/data1/04/efs,/data1/05/efs,/data1/06/efs
host_of() { echo "fcstor00$(( $1 + 2 ))"; }
addr_of() { echo "172.16.223.$(( $1 + 56 ))"; }

ssh_() { local t=$1 h=$2; shift 2; EFS_SSH_TIMEOUT=$t "$S" "$h.ib" "$@" 2>&1 | grep -v '^Identity added'; }

# Highest commit seen for a group across the voters we can reach.
group_max_commit() {
    local g=$1 n best=0 c
    for n in 1 2 3 4; do
        c=$(ssh_ 15 "$(host_of $n)" "cd /tmp/efs && ./efs-mgmt raft-status $(addr_of $n):19810 2>/dev/null" \
            | awk -v g="group $g " 'index($0,g) && /hosted=1/ {for(i=1;i<=NF;i++) if($i ~ /^commit=/){sub("commit=","",$i); print $i}}')
        [ -n "$c" ] && [ "$c" -gt "$best" ] && best=$c
    done
    echo "$best"
}

BUILD_CMD="rsync -a --checksum --delete --exclude=/mnt/ --exclude='*.log' --exclude=/results/ \$HOME/git/efs/ /tmp/efs/ >/dev/null 2>&1; cd /tmp/efs && make clean >/dev/null 2>&1 && make -j\$(nproc) efsd efs-mgmt >build.log 2>&1 && echo BUILD_OK; grep -o 'build=[^ ]*' efsd.log | head -1; id=\$(git rev-parse --short=12 HEAD); git status --short 2>/dev/null | grep -qv '^??' && id=\$id-dirty; echo build=\$id"

# All hosted groups on host $1 (addr $2): commit==applied and within 2 of
# the cluster max. Prints the status lines; returns 0 when caught up.
node_caught_up() {
    local h=$1 a=$2 st lag=0 line g c ap m
    st=$(ssh_ 15 "$h" "cd /tmp/efs && ./efs-mgmt raft-status $a:19810 2>/dev/null" | grep 'hosted=1')
    [ -n "$st" ] || return 1
    while read -r line; do
        g=$(echo "$line" | grep -o 'group [0-9]*' | awk '{print $2}')
        c=$(echo "$line" | grep -o 'commit=[0-9]*' | cut -d= -f2)
        ap=$(echo "$line" | grep -o 'applied=[0-9]*' | cut -d= -f2)
        m=$(group_max_commit "$g")
        [ "$c" = "$ap" ] && [ $(( m - c )) -le 2 ] || lag=1
    done <<< "$st"
    echo "$st"
    [ $lag = 0 ]
}

if [ $ALL = 1 ]; then
    tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
    echo "== build all four (parallel)"
    for n in 1 2 3 4; do ( ssh_ 240 "$(host_of $n)" "$BUILD_CMD" > "$tmp/$n" ) & done
    wait
    ids=""
    for n in 1 2 3 4; do
        echo "-- node $n"; cat "$tmp/$n"
        grep -q BUILD_OK "$tmp/$n" || { echo "FAIL: build on $(host_of $n)"; exit 1; }
        ids="$ids $(grep -o 'build=[^ ]*' "$tmp/$n" | sed -n 2p)"
    done
    [ "$(echo $ids | tr ' ' '\n' | sort -u | wc -l)" = 1 ] || { echo "FAIL: built IDs differ:$ids"; exit 1; }
    echo "== built$ids"
    if [ $FRESH = 1 ]; then
        echo "== fresh: storage must be empty"
        for n in 1 2 3 4; do
            cnt=$(ssh_ 15 "$(host_of $n)" "find /data1/0[1-6]/efs -mindepth 1 -maxdepth 1 2>/dev/null | wc -l")
            echo "$(host_of $n) entries=$cnt"
            [ "$cnt" = 0 ] || { echo "FAIL: $(host_of $n) storage not empty; --fresh refuses (wipe first)"; exit 1; }
        done
    fi
    echo "== stop all four"
    for n in 1 2 3 4; do ssh_ 15 "$(host_of $n)" "pkill -9 -x efsd; sleep 1; echo $(host_of $n) up=\$(pgrep -x efsd | wc -l)"; done
    echo "== start all four"
    for n in 1 2 3 4; do
        h=$(host_of $n); a=$(addr_of $n)
        join=""; [ $FRESH = 1 ] && [ "$n" != 1 ] && join="--join 172.16.223.57:19810"
        ssh_ 30 "$h" "mkdir -p /tmp/efs-perf; rm -f /tmp/efs-perf/efsd.data /tmp/efs-perf/efsd.strace; cd /tmp/efs && mv -f efsd.log efsd.log.prev 2>/dev/null; (env $EFSD_ENV $PERF_ENV setsid ./efsd --node-id $n --addr $a --port 19810 --storage $STORAGE --quota 36T --direct-io $join $EFSD_ARGS >efsd.log 2>&1 </dev/null &); sleep 1; echo $h up=\$(pgrep -x efsd | wc -l) perf=\$(pgrep -x perf | wc -l)"
    done
    if [ $FRESH = 1 ]; then
        # Both groups need a leader first: mkfs commits MKFS on group 0
        # and then the SALT record on group 2; a group-2 proposal before
        # its first election is BUSY. And mkfs goes to ONE node only —
        # the salt is h->salt of the node that serves it, so a retry on
        # another node after a BUSY proposes a second salt and group 2's
        # apply wedges on the SALT record forever (PROTO, Oct 1 07:15Z).
        echo "== wait for a leader in both groups (node 2 hosts both)"
        ok=0
        for i in $(seq 1 60); do
            st=$(ssh_ 15 fcstor004 "cd /tmp/efs && ./efs-mgmt raft-status 172.16.223.58:19810 2>/dev/null" | grep 'hosted=1')
            if [ "$(echo "$st" | grep -c 'leader=[0-9]')" = 2 ]; then ok=1; break; fi
            sleep 2
        done
        echo "$st"
        [ $ok = 1 ] || { echo "FAIL: no leader in both groups after 120 s"; exit 1; }
        echo "== raft-mkfs on node 1 only (same salt on every retry)"
        ok=0
        for i in $(seq 1 30); do
            out=$(ssh_ 20 fcstor003 "cd /tmp/efs && ./efs-mgmt raft-mkfs 172.16.223.57:19810 efs-test; echo mkfs_exit=\$?")
            echo "fcstor003: $(echo "$out" | tail -2 | tr '\n' ' ')"
            if echo "$out" | grep -q 'mkfs_exit=0'; then ok=1; break; fi
            sleep 2
        done
        [ $ok = 1 ] || { echo "FAIL: raft-mkfs not accepted after 60 s"; exit 1; }
        echo "MKFS_OK"
    fi
    echo "== wait for both groups"
    for n in 1 2 3 4; do
        h=$(host_of $n); a=$(addr_of $n); ok=0
        for i in $(seq 1 90); do
            st=$(node_caught_up "$h" "$a") && { ok=1; break; }
            sleep 2
        done
        echo "$st"
        [ $ok = 1 ] || { echo "FAIL: $h not caught up after 180 s"; exit 1; }
        echo "== node $n caught up"
    done
    echo "ROLL_OK"
    exit 0
fi

for n in "${NODES[@]}"; do
    h=$(host_of "$n"); a=$(addr_of "$n")
    join=""; [ "$n" != 1 ] && join="--join 172.16.223.57:19810"
    echo "== node $n ($h) build"
    out=$(ssh_ 240 "$h" "$BUILD_CMD")
    echo "$out"
    echo "$out" | grep -q BUILD_OK || { echo "FAIL: build on $h"; exit 1; }
    running=$(echo "$out" | grep -o 'build=[^ ]*' | sed -n 1p)
    built=$(echo "$out" | grep -o 'build=[^ ]*' | sed -n 2p)
    if [ -n "$built" ] && [ -n "$running" ] && [ "$built" != "$running" ]; then
        echo "FAIL: build id changed ($running -> $built): stop all four, then start all four"; exit 1
    fi
    echo "== node $n restart"
    ssh_ 30 "$h" "mkdir -p /tmp/efs-perf; rm -f /tmp/efs-perf/efsd.data /tmp/efs-perf/efsd.strace; pkill -9 -x efsd; sleep 1; cd /tmp/efs && mv -f efsd.log efsd.log.prev 2>/dev/null; (env $EFSD_ENV $PERF_ENV setsid ./efsd --node-id $n --addr $a --port 19810 --storage $STORAGE --quota 36T --direct-io $join $EFSD_ARGS >efsd.log 2>&1 </dev/null &); sleep 2; echo up=\$(pgrep -x efsd | wc -l) perf=\$(pgrep -x perf | wc -l)"
    # Wait for catch-up: every hosted group commit==applied and within 2 of the cluster max.
    ok=0
    for i in $(seq 1 90); do
        st=$(ssh_ 15 "$h" "cd /tmp/efs && ./efs-mgmt raft-status $a:19810 2>/dev/null" | grep 'hosted=1')
        if [ -n "$st" ]; then
            lag=0
            while read -r line; do
                g=$(echo "$line" | grep -o 'group [0-9]*' | awk '{print $2}')
                c=$(echo "$line" | grep -o 'commit=[0-9]*' | cut -d= -f2)
                ap=$(echo "$line" | grep -o 'applied=[0-9]*' | cut -d= -f2)
                m=$(group_max_commit "$g")
                [ "$c" = "$ap" ] && [ $(( m - c )) -le 2 ] || lag=1
            done <<< "$st"
            [ $lag = 0 ] && { ok=1; break; }
        fi
        sleep 2
    done
    echo "$st"
    [ $ok = 1 ] || { echo "FAIL: $h not caught up after 180 s"; exit 1; }
    echo "== node $n caught up"
done
echo "ROLL_OK"
