#!/bin/bash
# deploy_fuse_clients.sh fcstor008 fcstor009 ...
# Rebuild + remount efs-fuse on each client in parallel (tests/fuse_client_remount.sh
# runs on the node; the tree comes from the NFS $HOME). Exit 1 if any client
# is not fuse.efs-fuse + MOUNT_OK afterwards. Run it from node9901 via
# efs-bg.sh, not from the login node.
set -u
S="$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh"
[ $# -gt 0 ] || { echo "usage: $0 <host>..." >&2; exit 2; }
tmp=$(mktemp -d)
for h in "$@"; do
    ( EFS_SSH_TIMEOUT=300 "$S" "$h.ib" 'bash $HOME/git/efs/tests/fuse_client_remount.sh' \
        2>&1 | grep -v '^Identity added' > "$tmp/$h" ) &
done
wait
rc=0
for h in "$@"; do
    cat "$tmp/$h"
    grep -q 'fstype=fuse.efs-fuse MOUNT_OK' "$tmp/$h" || rc=1
done
rm -rf "$tmp"
exit $rc
