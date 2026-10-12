#!/bin/bash
# gw-up.sh [--remount] — bring up the $GW NFS gateway end to end (idempotent):
# boot the VM if needed, make sure the current efs binaries are there,
# mount the efs cluster with export support, and re-export it via nfsd.
# --remount: tear down a possibly-hung /mnt/efs FUSE mount first (the plain
# idempotent path skips a mountpoint that still answers).
usage() {
    cat <<'EOF'
usage: ./gw-up.sh [--remount]
Bring up the cluster's NFS gateway end to end (idempotent): boot the
gateway VM if needed, make sure the current efs binaries are there, mount
the efs cluster at /mnt/efs with export support, and re-export it via nfsd
(writes /etc/exports.d/efs.exports, scrubs any /mnt/efs line from
/etc/exports). Requires the cluster to be running (./deploy.sh &&
./cluster.sh start first). Afterwards: ./mount-mac.sh

  --remount   tear down a possibly-hung /mnt/efs FUSE mount first (the
              plain idempotent path skips a mountpoint that still answers)

Acts on the cluster from $CLUSTER or .current-cluster (see ./use.sh).
EOF
}
case "${1:-}" in -h|--help|help) usage; exit 0 ;; esac
[ $# -le 1 ] && { [ $# -eq 0 ] || [ "$1" = --remount ]; } || { usage >&2; exit 2; }
set -eo pipefail
cd "$(dirname "$0")" && source ./env.sh
EFS_RPC_PROF=${EFS_RPC_PROF:-0}
case "$EFS_RPC_PROF" in 0|1) ;; *) echo "EFS_RPC_PROF must be 0 or 1" >&2; exit 2 ;; esac
RPC_ENV=""
[ "$EFS_RPC_PROF" = 1 ] && RPC_ENV="EFS_RPC_PROF=1"
source ./lib/client-user.sh
source ./lib/client-stop.sh

source ./lib/transfer.sh
# Compare actual running executable bytes; --version of the installed path
# cannot establish which client is serving an existing mount.
client_binary_current() {
    ssh_node "$1" 'sudo -n -H -u efs bash -c '"'"'seen=0; for p in $(pgrep -x efs-fuse || true); do seen=1; test "$(ps -p "$p" -o uid= | tr -d " ")" = "$(id -u efs)" || exit 1; cmp -s /proc/$p/exe /tmp/efs/efs-fuse || exit 1; done; test "$seen" = 1'"'"''
}

REMOUNT=""
[ "${1:-}" = "--remount" ] && REMOUNT=1

SEED=${NODES[0]}

# 1. boot
if vm_is_running "$GW"; then
    echo "$GW already running"
else
    echo "starting $GW"
    vm_start "$GW"
fi
wait_ssh "$GW"
ensure_efs_user "$GW"
ensure_guest_tz "$GW"
GWIP=$(ip_of "$GW")
echo "$GW up at $GWIP"

# 2. binaries (tmpfs: gone after every reboot). Copy the build from the seed.
    echo "shipping binaries from $SEED"
    ssh_node "$GW" 'mkdir -p /tmp/efs'
    ship_binaries "$SEED" "$GW"
    rsync_to "$GW" "$EFS_SRC/scripts/" /tmp/efs/scripts/

# 3. efs mount with NFS-export support (fsid=0 export of /mnt/efs)
SEEDIP=$(ip_of "$SEED")
if ! ssh_node "$SEED" 'pgrep -x efsd >/dev/null'; then
    echo "ERROR: efsd is not running on $SEED ($SEEDIP) — start the cluster first:" >&2
    echo "  ./deploy.sh && ./cluster.sh start" >&2
    exit 1
fi
# Invisible clients must not be bypassed by a mountpoint check.
    if [ -z "$REMOUNT" ] && ! ssh_node "$GW" 'mountpoint -q /mnt/efs'; then
        process_rc=0
        ssh_stream_once "$GW" 'sudo -n -H -u efs python3 /tmp/efs/scripts/client_processes.py has /mnt/efs' || process_rc=$?
        if [ "$process_rc" -ne 1 ]; then
            echo "ERROR: detached client or discovery failure on $GW; use --remount to drain before replacement" >&2
            exit 1
        fi
    fi
    if [ -n "$REMOUNT" ]; then
    echo "remount requested: stopping efs-fuse on $GW"
    stop_client "$GW" /mnt/efs
    if ssh_node "$GW" 'mountpoint -q /mnt/efs'; then
        echo "ERROR: $GW remains mounted after stop" >&2
        exit 1
    fi
fi
if ssh_node "$GW" 'mountpoint -q /mnt/efs'; then
    if ! client_binary_current "$GW"; then
        echo "ERROR: $GW mount is served by a non-efs-owned/old/missing efs-fuse process; stop workloads, then run ./gw-up.sh --remount" >&2
        exit 1
    fi
    echo "/mnt/efs already mounted with current binary"
else
    echo "mounting efs on $GW (seed $SEEDIP:$EFS_PORT)"
    prepare_client_mount "$GW" /mnt/efs
    ssh_node_once "$GW" "cd /tmp/efs && \
        sudo -n -H -u efs env $RPC_ENV EFS_FUSE_EXPORT=1 EFS_TRANSPORT=$TRANSPORT \
        ./scripts/client.sh $SEEDIP:$EFS_PORT /mnt/efs -o allow_other"
fi

# 4. nfsd export. Idempotent: scrub any /mnt/efs line from /etc/exports (the
#    Tart image has one baked in), then (re)write the exports.d drop-in and
#    re-export in case the FUSE mount was recreated (stale export dentry is
#    the classic failure). all_squash→uid 0 works around efs-fuse denying
#    OPEN on the mode-0 files that macOS EXCLUSIVE4 creates (uid-0 bypass in
#    check_access).
EXPORTS_LINE='/mnt/efs *(rw,fsid=0,no_subtree_check,no_root_squash,all_squash,anonuid=0,anongid=0,insecure)'
ssh_node "$GW" "sudo mkdir -p /etc/exports.d; \
    sudo sed -i -e '\|^/mnt/efs[[:space:]]|d' /etc/exports 2>/dev/null || true; \
    echo '$EXPORTS_LINE' | sudo tee /etc/exports.d/efs.exports >/dev/null; \
    sudo exportfs -r; sudo systemctl restart nfs-server; sudo exportfs"

echo "gateway ready at $GWIP (from the Mac: ./mount-mac.sh)"
./hosts.sh  # refresh /etc/hosts + ssh-by-name everywhere
