#!/bin/bash
# Wipe the fcstor efs test cluster and bring up a fresh mkfs'd efs-test.
#
# Does: wipe_cluster.sh -> (optional) rsync+build efsd on the 4 servers ->
# start primary -> mkfs efs-test -> start 3 joiners -> verify 4 up.
# Clients are NOT mounted here — use run_tests.sh setup for that.
#
# Usage: clean_cluster.sh [--no-deploy]
#   --no-deploy   skip the rsync+build (reuse the binaries already in /tmp/efs)
set -u
SSH="${EFS_SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
SERVERS=(fcstor003.ib fcstor004.ib fcstor005.ib fcstor006.ib)
ADDR=(172.16.223.57 172.16.223.58 172.16.223.59 172.16.223.60)
STORAGE=/data1/01/efs,/data1/02/efs,/data1/03/efs,/data1/04/efs,/data1/05/efs,/data1/06/efs
DEPLOY=1
[ "${1:-}" = "--no-deploy" ] && DEPLOY=0

say() { echo "[clean] $*"; }
ssh_to() { local t=$1; shift; EFS_SSH_TIMEOUT=$t "$SSH" "$@"; }

start_efsd() { # host node-id addr [join]
    local h=$1 nid=$2 addr=$3 join=${4:-}
    local j=""; [ -n "$join" ] && j="--join $join"
    # setsid detaches; the ssh session may linger (harmless) so allow rc=124.
    # EFS_FLUSH_PROF=1 makes efsd emit FLUSH-PROF/FLUSH-WINDOW lines (metadata
    # flush per-stage timing). Must stay UNSET when the caller did not ask for
    # it — efsd tests it with getenv(), so even an empty value turns it on.
    local prof=""
    [ -n "${EFS_FLUSH_PROF:-}" ] && prof="EFS_FLUSH_PROF=$EFS_FLUSH_PROF"
    [ -n "${EFS_LOCK_PROF:-}" ] && prof="$prof EFS_LOCK_PROF=$EFS_LOCK_PROF"
    [ -n "${EFS_TRANSPORT:-}" ] && prof="$prof EFS_TRANSPORT=$EFS_TRANSPORT"
    ssh_to 15 "$h" "cd /tmp/efs && $prof \
        setsid ./efsd --node-id $nid --addr $addr \
        --port 19810 --storage $STORAGE --quota 36T --direct-io $j \
        >efsd.log 2>&1 </dev/null & sleep 3; pgrep -x efsd >/dev/null && echo UP" \
        2>/dev/null | grep -q UP
}

say "1/4 wipe"
bash "$(dirname "$0")/wipe_cluster.sh" || exit 1

if [ "$DEPLOY" = 1 ]; then
    extra_defs=""
    [ -n "${EFS_MKFS_SHARD_BITS:-}" ] && extra_defs="-DEFS_DEFAULT_SHARD_BITS=$EFS_MKFS_SHARD_BITS"
    say "2/4 rsync+build efsd on 4 servers (parallel)${extra_defs:+ bits=$EFS_MKFS_SHARD_BITS}"
    for h in "${SERVERS[@]}"; do
        ( ssh_to 120 "$h" "rsync -a --delete --exclude='/mnt/' --exclude='*.log' \
              \"\$HOME/git/efs/\" /tmp/efs/ >/dev/null 2>&1 && \
              cd /tmp/efs && make clean >/dev/null 2>&1 && \
              make -j\"\$(nproc)\" EXTRA_DEFS='$extra_defs' efsd efs-mgmt >/dev/null 2>&1" \
              && echo "  ${h%.ib} build OK" || echo "  ${h%.ib} BUILD FAIL" ) &
    done
    wait
fi

say "3/4 start primary + mkfs"
start_efsd fcstor003.ib 1 172.16.223.57 || { echo "primary failed to start"; exit 1; }
ssh_to 20 fcstor003.ib 'cd /tmp/efs && ./efs-mgmt mkfs 172.16.223.57:19810 efs-test' \
    || { echo "mkfs failed"; exit 1; }

say "4/4 start 3 joiners (parallel)"
for i in 1 2 3; do
    ( start_efsd "${SERVERS[$i]}" $((i+1)) "${ADDR[$i]}" 172.16.223.57:19810 \
        && echo "  ${SERVERS[$i]%.ib} up" || echo "  ${SERVERS[$i]%.ib} FAILED" ) &
done
wait

# Verify: poll for 4 up.
for _ in 1 2 3 4 5; do
    up=$(ssh_to 15 fcstor003.ib 'cd /tmp/efs && ./efs-mgmt status 172.16.223.57:19810' \
         2>/dev/null | grep -cE "node [0-9]+: .* up ")
    [ "$up" = 4 ] && { say "cluster up (4 nodes, fresh mkfs)"; exit 0; }
    sleep 2
done
echo "ERROR: cluster did not converge (up=$up)"; exit 1
