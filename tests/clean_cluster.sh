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
    # RDMA handshake / first-frame trace (rdma-first: lines on both sides).
    [ -n "${EFS_RDMA_FIRST:-}" ] && prof="$prof EFS_RDMA_FIRST=$EFS_RDMA_FIRST"
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
    say "2/4 rsync+build efsd on 4 servers (parallel)"
    for h in "${SERVERS[@]}"; do
        ( ssh_to 120 "$h" "rsync -a --delete --exclude='/mnt/' --exclude='*.log' \
              \"\$HOME/git/efs/\" /tmp/efs/ >/dev/null 2>&1 && \
              cd /tmp/efs && make clean >/dev/null 2>&1 && \
              make -j\"\$(nproc)\" efsd efs-mgmt >/dev/null 2>&1" \
              && echo "  ${h%.ib} build OK" || echo "  ${h%.ib} BUILD FAIL" ) &
    done
    wait
fi

# Raft mkfs needs a quorum, so ALL nodes come up first, then mkfs.
say "3/4 start node 1"
start_efsd fcstor003.ib 1 172.16.223.57 || { echo "node 1 failed to start"; exit 1; }

say "4/4 start 3 joiners (parallel) + mkfs"
for i in 1 2 3; do
    ( start_efsd "${SERVERS[$i]}" $((i+1)) "${ADDR[$i]}" 172.16.223.57:19810 \
        && echo "  ${SERVERS[$i]%.ib} up" || echo "  ${SERVERS[$i]%.ib} FAILED" ) &
done
wait
# mkfs waits for the group-0 commit AND replicates the export salt to
# group 2 (EFS_MD_CMD_SALT) before returning rc=0; the host forwards to the
# current leader itself. The hint retry below is only a fallback for an
# older efsd: a NOT_PRIMARY (rc=-15) reply carries leader_hint = the
# leader's raw raft_id (0-based); node N has raft_id N-1 and addr
# 172.16.223.(56+N).
mkfs_out=$(ssh_to 20 fcstor003.ib 'cd /tmp/efs && ./efs-mgmt mkfs 172.16.223.57:19810 efs-test') || true
echo "$mkfs_out"
if ! echo "$mkfs_out" | grep -q "rc=0"; then
    hint=$(echo "$mkfs_out" | sed -n 's/.*leader_hint=\([0-9-]*\).*/\1/p' | head -1)
    case "$hint" in
        0|1|2|3)
            tip=$((57 + hint))
            say "mkfs NOT_PRIMARY, retrying on leader raft_id=$hint (172.16.223.$tip)"
            ssh_to 20 fcstor003.ib "cd /tmp/efs && ./efs-mgmt mkfs 172.16.223.$tip:19810 efs-test" \
                || { echo "mkfs failed (via hint $hint)"; exit 1; }
            ;;
        *)
            echo "mkfs failed: $mkfs_out"; exit 1
            ;;
    esac
fi

# Verify: poll for 4 up.
for _ in 1 2 3 4 5; do
    up=$(ssh_to 15 fcstor003.ib 'cd /tmp/efs && ./efs-mgmt status 172.16.223.57:19810' \
         2>/dev/null | grep -cE "node [0-9]+: .* up ")
    [ "$up" = 4 ] && { say "cluster up (4 nodes, fresh mkfs)"; exit 0; }
    sleep 2
done
echo "ERROR: cluster did not converge (up=$up)"; exit 1
