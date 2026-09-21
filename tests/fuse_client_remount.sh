#!/bin/bash
# Runs ON a client node (NFS $HOME is shared, so invoke it remotely as
# `bash $HOME/git/efs/tests/fuse_client_remount.sh`): rsync the tree to
# node-local /tmp/efs, clean-build efs-fuse there (never on NFS, never on a
# login node), kill + unmount the old client, mount the new one, and print
# one status line. Deploy rule "Restart one efs-fuse".
#
# EFS_NO_BUILD=1     remount the binary already in /tmp/efs (no rsync, no
#                    make) — for measurement runbooks that must keep the
#                    cluster version unchanged and only add an env var.
# EFS_FUSE_ENV='A=1 B=2'  extra environment for the efs-fuse process
#                    (e.g. EFS_RPC_PROF=1 for per-RPC counters in fuse.log).
set -u
SERVER="${EFS_SERVER:-172.16.223.57:19810}"
EXPORT="${EFS_EXPORT:-efs-test}"
MNT="${EFS_MNT:-/tmp/efs-mount}"

if [ "${EFS_NO_BUILD:-0}" != 1 ]; then
    rsync -a --delete --exclude='/mnt/' --exclude='*.log' --exclude='/results/' \
        "$HOME/git/efs/" /tmp/efs/ >/dev/null 2>&1
    cd /tmp/efs || { echo "$(hostname -s): no /tmp/efs"; exit 1; }
    make clean >/dev/null 2>&1
    # fcstor003-006 run efsd too: the rsync --delete + make clean above
    # removed their efsd/efs-mgmt binaries, and preflight needs efs-mgmt
    # (Sep 21: PREFLIGHT_FAIL "efs-mgmt missing" right after a client
    # deploy). Rebuild what the host serves with.
    extra=""
    pgrep -x efsd >/dev/null 2>&1 && extra="efsd efs-mgmt"
    if ! make -j"$(nproc)" efs-fuse $extra >/tmp/efs/build-fuse.log 2>&1; then
        echo "$(hostname -s): BUILD_FAIL $(grep -m3 -E 'error' /tmp/efs/build-fuse.log | tr '\n' ' ')"
        exit 1
    fi
else
    cd /tmp/efs || { echo "$(hostname -s): no /tmp/efs"; exit 1; }
    [ -x ./efs-fuse ] || { echo "$(hostname -s): no /tmp/efs/efs-fuse to remount"; exit 1; }
fi
killall -9 efs-fuse 2>/dev/null
sleep 0.5
timeout 3 fusermount3 -uz "$MNT" 2>/dev/null
mkdir -p "$MNT"
rm -f fuse.log
(env EFS_TRANSPORT="${EFS_TRANSPORT:-tcp}" ${EFS_FUSE_ENV:-} setsid ./efs-fuse "$SERVER" "$EXPORT" "$MNT" >fuse.log 2>&1 </dev/null &)
for _ in $(seq 1 40); do
    sleep 0.5
    findmnt -no FSTYPE "$MNT" 2>/dev/null | grep -q fuse.efs-fuse && break
done
fst=$(findmnt -no FSTYPE "$MNT" 2>/dev/null)
ok=$(stat "$MNT/" >/dev/null 2>&1 && echo MOUNT_OK || echo MOUNT_BAD)
echo "$(hostname -s): fstype=${fst:-none} $ok fuse=$(pgrep -x efs-fuse | wc -l) bin=$(stat -c %y efs-fuse | cut -c12-19) $(grep -o 'build=[^ ]*' fuse.log | head -1)"
[ "$fst" = fuse.efs-fuse ] && [ "$ok" = MOUNT_OK ]
