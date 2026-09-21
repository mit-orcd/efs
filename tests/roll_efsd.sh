#!/bin/bash
# roll_efsd.sh [node...]      default: 1 2 3 4   (fcstor003..006)
#
# Rolling restart of efsd on the 19810 cluster, one node at a time, per the
# efs-fcstor-deploy rule: rsync + clean build ON the node, `pkill -9 -x efsd`
# (exact name, never -f / never the ps|awk kill-by-port), start detached,
# then wait until every group the node hosts reports commit == applied and
# matches the other voters before touching the next node. Same build ID
# only — after a new commit stop all four first (this script refuses when
# the running build string differs from what it just built).
#
# Run it from node9901 via efs-bg.sh; each step is a short ssh. Exit 1 on
# the first node that does not come back caught up (do not continue: a
# second node down loses quorum).
set -u
S="$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh"
NODES=("$@"); [ ${#NODES[@]} -gt 0 ] || NODES=(1 2 3 4)
# Environment for the efsd process, e.g. EFSD_ENV='EFS_TRANSPORT=tcp'
# (peer pool + raft AE over TCP instead of the ungated RDMA upgrade).
EFSD_ENV="${EFSD_ENV:-}"
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

for n in "${NODES[@]}"; do
    h=$(host_of "$n"); a=$(addr_of "$n")
    join=""; [ "$n" != 1 ] && join="--join 172.16.223.57:19810"
    echo "== node $n ($h) build"
    out=$(ssh_ 240 "$h" "rsync -a --delete --exclude=/mnt/ --exclude='*.log' --exclude=/results/ \$HOME/git/efs/ /tmp/efs/ >/dev/null 2>&1; cd /tmp/efs && make clean >/dev/null 2>&1 && make -j\$(nproc) efsd efs-mgmt >build.log 2>&1 && echo BUILD_OK; grep -o 'build=[^ ]*' efsd.log | head -1; id=\$(git rev-parse --short=12 HEAD); git status --short 2>/dev/null | grep -qv '^??' && id=\$id-dirty; echo build=\$id")
    echo "$out"
    echo "$out" | grep -q BUILD_OK || { echo "FAIL: build on $h"; exit 1; }
    running=$(echo "$out" | grep -o 'build=[^ ]*' | sed -n 1p)
    built=$(echo "$out" | grep -o 'build=[^ ]*' | sed -n 2p)
    if [ -n "$built" ] && [ -n "$running" ] && [ "$built" != "$running" ]; then
        echo "FAIL: build id changed ($running -> $built): stop all four, then start all four"; exit 1
    fi
    echo "== node $n restart"
    ssh_ 30 "$h" "pkill -9 -x efsd; sleep 1; cd /tmp/efs && mv -f efsd.log efsd.log.prev 2>/dev/null; (env $EFSD_ENV setsid ./efsd --node-id $n --addr $a --port 19810 --storage $STORAGE --quota 36T --direct-io $join >efsd.log 2>&1 </dev/null &); sleep 2; echo up=\$(pgrep -x efsd | wc -l)"
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
