#!/bin/bash
# Tear down the fcstor efs test cluster and wipe on-disk state.
#
# Order is mandatory. A leftover efs-fuse still holds the old table in
# RAM and will REPORT/flush it onto a fresh mkfs (the recurring
# "adopted cache blob 232MB / 1.9M chunks / gen=102"). pkill without -9
# misses D-state daemons stuck on a busy mount.
#
#   1. kill -9 every efs-fuse on fcstor003–015 (clients first)
#   2. kill -9 every efsd on fcstor003–006
#   3. edelete /data1/01..06/efs on the four servers (never rm -rf)
#   4. mkdir the storage dirs back (edelete removes the start dir)
#
# Run from the login node. Does not mkfs or restart — do that after.
set -eu
SSH="${EFS_SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
CLIENTS=(fcstor003.ib fcstor004.ib fcstor005.ib fcstor006.ib \
         fcstor007.ib fcstor008.ib fcstor009.ib fcstor010.ib \
         fcstor011.ib fcstor012.ib fcstor013.ib fcstor014.ib fcstor015.ib)
SERVERS=(fcstor003.ib fcstor004.ib fcstor005.ib fcstor006.ib)

say() { echo "[wipe] $*"; }

ssh_to() { # timeout_sec host [remote]
    local t=$1; shift
    EFS_SSH_TIMEOUT=$t "$SSH" "$@"
    local rc=$?
    if [ $rc -eq 124 ]; then
        say "TIMEOUT ${t}s: $*"
    fi
    return $rc
}

kill_fuse() {
    local h=$1
    # Kill first. fusermount on a D-state mount hangs the SSH session
    # forever (ServerAlive does not fire — the host still answers).
    ssh_to 10 "$h" 'killall -9 efs-fuse 2>/dev/null || pkill -9 -x efs-fuse 2>/dev/null || true
        timeout 3 fusermount3 -uz /tmp/efs/mnt 2>/dev/null || true
        n=$(pgrep -x efs-fuse | wc -l)
        echo fuse=$n'
}

kill_efsd() {
    local h=$1
    ssh_to 10 "$h" 'pkill -9 -x efsd 2>/dev/null || true
        n=$(pgrep -x efsd | wc -l)
        echo efsd=$n'
}

say "1/4 kill efs-fuse on ${#CLIENTS[@]} clients (parallel)"
left=0
tmp=$(mktemp -d)
for h in "${CLIENTS[@]}"; do
    (
        out=$(kill_fuse "$h") || out="TIMEOUT fuse=1"
        echo "  ${h%.ib} $out"
        echo "${out##*fuse=}" >"$tmp/${h}"
    ) &
done
wait
for h in "${CLIENTS[@]}"; do
    n=$(cat "$tmp/$h" 2>/dev/null || echo 1)
    left=$((left + ${n:-1}))
done
rm -rf "$tmp"
if [ "$left" -ne 0 ]; then
    echo "ERROR: $left efs-fuse still alive — not wiping disks" >&2
    exit 1
fi

say "2/4 kill efsd on 4 servers (parallel)"
for h in "${SERVERS[@]}"; do
    ( echo "  ${h%.ib} $(kill_efsd "$h" || echo TIMEOUT)" ) &
done
wait

say "3/4 edelete storage"
for h in "${SERVERS[@]}"; do
    ssh_to 30 "$h" 'for d in 01 02 03 04 05 06; do
        ~/git/ereport/edelete --delete --force /data1/$d/efs
    done
    echo WIPED' &
done
wait

say "4/4 recreate empty storage dirs (parallel)"
for h in "${SERVERS[@]}"; do
    ( ssh_to 10 "$h" 'for d in 01 02 03 04 05 06; do mkdir -p /data1/$d/efs; done
        echo READY' ) &
done
wait

say "cluster wiped. mkfs + start efsd next. do not start efs-fuse until mkfs."
