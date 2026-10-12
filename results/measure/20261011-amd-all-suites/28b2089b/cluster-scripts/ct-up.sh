#!/bin/bash
# ct-up.sh — bring up the test client VMs ($CLIENTS) end to end (idempotent):
# boot the VMs if needed, ship the current efs binaries + scripts + posix
# suites from the seed, and mount the cluster at /mnt/efs as a plain FUSE
# client. Unlike gw-up.sh there is no EFS_FUSE_EXPORT and no nfsd — these
# hosts are pure test clients (posix.sh 2client runs across them).
usage() {
    cat <<'EOF'
usage: ./ct-up.sh [--remount]
Bring up the cluster's test client VMs ($CLIENTS) end to end (idempotent):
boot the VMs if needed, ship the current efs binaries + scripts + posix
suites from the seed node, and mount the efs cluster at /mnt/efs as a plain
FUSE client. No NFS on these hosts (that is the gateway's job, ./gw-up.sh).
--remount stops existing mounts before loading the shipped client.
Without it, an old mounted executable fails with a remount instruction.
Requires the cluster to be running (./deploy.sh && ./cluster.sh start first).
Fails for clusters without CLIENTS in clusters/<name>.env.

Acts on the cluster from $CLUSTER or .current-cluster (see ./use.sh).
EOF
}
case "${1:-}" in -h|--help|help) usage; exit 0 ;; esac
[ $# -le 1 ] && { [ $# -eq 0 ] || [ "$1" = --remount ]; } || { usage >&2; exit 2; }
set -eo pipefail
cd "$(dirname "$0")" && source ./env.sh
source ./lib/client-user.sh
source ./lib/client-stop.sh

source ./lib/transfer.sh
# Compare actual running executable bytes; --version of the installed path
# cannot establish which client is serving an existing mount.
client_binary_current() {
    ssh_node "$1" 'sudo -n -H -u efs bash -c '"'"'seen=0; for p in $(pgrep -x efs-fuse || true); do seen=1; test "$(ps -p "$p" -o uid= | tr -d " ")" = "$(id -u efs)" || exit 1; cmp -s /proc/$p/exe /tmp/efs/efs-fuse || exit 1; done; test "$seen" = 1'"'"''
}

REMOUNT=""
case "${1:-}" in "") ;; --remount) REMOUNT=1 ;; *) usage >&2; exit 2 ;; esac

if [ ${#CLIENTS[@]} -eq 0 ]; then
    echo "cluster $CLUSTER_NAME has no CLIENTS (see clusters/$CLUSTER_NAME.env)" >&2
    exit 1
fi

SEED=${NODES[0]}
SEEDIP=$(ip_of "$SEED")
if ! ssh_node "$SEED" 'pgrep -x efsd >/dev/null'; then
    echo "ERROR: efsd is not running on $SEED ($SEEDIP) — start the cluster first:" >&2
    echo "  ./deploy.sh && ./cluster.sh start" >&2
    exit 1
fi

BINS="efsd efs-fuse efs-mgmt efs-query efs-bench"
for ct in "${CLIENTS[@]}"; do
    # 1. boot
    if vm_is_running "$ct"; then
        echo "$ct already running"
    else
        echo "starting $ct"
        vm_start "$ct"
    fi
    wait_ssh "$ct"
    ensure_efs_user "$ct"
    ensure_guest_tz "$ct"

    # 2. binaries + scripts + posix suites (tmpfs: gone after every reboot,
    #    and a fresh deploy must reach the clients too — always ship)
    echo "== ship binaries + scripts + tests/posix -> $ct"
    ssh_node "$ct" 'mkdir -p /tmp/efs/scripts /tmp/efs/tests/posix'
    ship_binaries "$SEED" "$ct"
    rsync_to "$ct" "$EFS_SRC/scripts/" /tmp/efs/scripts/
    ship_bench_harness "$ct" "$EFS_SRC"
    rsync_to "$ct" "$EFS_SRC/tests/posix/" /tmp/efs/tests/posix/

    # Invisible clients must not be bypassed by a mountpoint check.
    if [ -z "$REMOUNT" ] && ! ssh_node "$ct" 'mountpoint -q /mnt/efs'; then
        process_rc=0
        ssh_stream_once "$ct" 'sudo -n -H -u efs python3 /tmp/efs/scripts/client_processes.py has /mnt/efs' || process_rc=$?
        if [ "$process_rc" -ne 1 ]; then
            echo "ERROR: detached client or discovery failure on $ct; use --remount to drain before replacement" >&2
            exit 1
        fi
    fi
    if [ -n "$REMOUNT" ]; then
        echo "$ct: remount requested (stop workloads first)"
        stop_client "$ct" /mnt/efs
        if ssh_node "$ct" 'mountpoint -q /mnt/efs'; then
            echo "ERROR: $ct remains mounted after stop" >&2
            exit 1
        fi
    fi
    # 3. plain FUSE mount (no export support — no NFS here)
    if ssh_node "$ct" 'mountpoint -q /mnt/efs'; then
        if ! client_binary_current "$ct"; then
            echo "ERROR: $ct mount is served by a non-efs-owned/old/missing efs-fuse process; stop workloads, then run ./ct-up.sh --remount" >&2
            exit 1
        fi
        echo "$ct: /mnt/efs already mounted with current binary"
    else
        echo "$ct: mounting efs (seed $SEEDIP:$EFS_PORT)"
        prepare_client_mount "$ct" /mnt/efs
        ssh_node_once "$ct" "cd /tmp/efs && \
            sudo -n -H -u efs env EFS_TRANSPORT=$TRANSPORT ./scripts/client.sh $SEEDIP:$EFS_PORT /mnt/efs -o allow_other"
    fi
done

./hosts.sh  # clients join the ssh-by-name mesh
echo "test clients ready: ${CLIENTS[*]} (posix.sh 2client runs across them)"
