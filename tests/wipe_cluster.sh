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
#   3. mv /data1/01..06/efs to /data1/01..06/_delete/efs.<unique> (instant
#      same-filesystem rename) and mkdir the storage dirs back — the wipe
#      returns in ms instead of blocking on edelete of millions of chunks
#   4. a detached background edelete reclaims the _delete dirs (never rm -rf)
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
    # A D-state efs-fuse is transiently alive right after kill -9 (the
    # signal is pending until the D-state resolves), so poll pgrep for a
    # few seconds rather than aborting the wipe on a daemon about to die.
    # 20s (not 15): the shared login node is sometimes so loaded (load >500
    # from other tenants) that the ssh connection setup itself takes seconds.
    ssh_to 20 "$h" 'killall -9 efs-fuse 2>/dev/null || pkill -9 -x efs-fuse 2>/dev/null || true
        timeout 3 fusermount3 -uz /tmp/efs-mount 2>/dev/null || true
        n=$(pgrep -x efs-fuse | wc -l)
        for i in $(seq 1 40); do
            [ "$n" -eq 0 ] && break
            sleep 0.2
            n=$(pgrep -x efs-fuse | wc -l)
        done
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
# Serial retry for any host the parallel pass didn't confirm dead. Parallel
# ssh fails when the shared login node is overloaded (rc=255 before auth —
# the box can't schedule 13 concurrent connections); a single ssh gets CPU.
# Don't abort the wipe on a transient ssh failure.
for h in "${CLIENTS[@]}"; do
    n=$(cat "$tmp/$h" 2>/dev/null || echo 1)
    if [ "${n:-1}" != "0" ]; then
        for _att in 1 2 3 4; do
            out=$(kill_fuse "$h") || out="TIMEOUT fuse=1"
            n="${out##*fuse=}"
            [ "$n" = "0" ] && break
            sleep 1
        done
        echo "  ${h%.ib} serial-retry: $out"
        echo "$n" >"$tmp/${h}"
    fi
done
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

say "3/4 move storage aside (instant rename) + recreate empty dirs"
# Rename /data1/$d/efs to /data1/$d/_delete/efs.<unique> (instant on the same
# filesystem) and recreate the empty dir. efsd is already dead (step 2), so no
# open fds point into the moved tree. The actual reclaim is deferred to a
# detached background edelete (step 4) so the wipe returns in milliseconds.
for h in "${SERVERS[@]}"; do
    ( ssh_to 15 "$h" 'for d in 01 02 03 04 05 06; do
        if [ -d /data1/$d/efs ]; then
            mkdir -p /data1/$d/_delete
            mv /data1/$d/efs "/data1/$d/_delete/efs.$(date +%s%N).$$"
        fi
        mkdir -p /data1/$d/efs
    done
    echo MOVED' ) &
done
wait

say "4/4 background edelete of _delete dirs (detached, survives ssh)"
# setsid + </dev/null + & detaches so the ssh session returns immediately and
# the edelete keeps running after we disconnect. Globs all 6 data dirs at once.
for h in "${SERVERS[@]}"; do
    ( ssh_to 10 "$h" "setsid bash -c 'for old in /data1/*/_delete/*; do [ -e \"\$old\" ] && ~/git/ereport/edelete --delete --force \"\$old\" >/dev/null 2>&1; done' </dev/null >/dev/null 2>&1 & echo BG-DELETE" ) &
done
wait

say "cluster wiped. mkfs + start efsd next. do not start efs-fuse until mkfs."
