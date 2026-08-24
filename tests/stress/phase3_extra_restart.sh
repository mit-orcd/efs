#!/bin/bash
# Phase 3 milestone: extra-shard restart + owner flush + owner-only RAM.
#
# Creates a throwaway bits=3 export (efs-s3), spreads files across all 8
# shards, waits for extras flush, kills a non-primary extra-shard owner,
# checks failover reads, restarts that owner, cold-mounts, and verifies
# every file survived. Does NOT touch efs-test / ImageNet.
#
# Usage (login node, cluster already up on the new build):
#   tests/stress/phase3_extra_restart.sh
set -u
SSH="${SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
PRIMARY="${PRIMARY:-fcstor003.ib}"
EXTRA_OWNER="${EXTRA_OWNER:-fcstor005.ib}"
EXTRA_NODE_ID="${EXTRA_NODE_ID:-3}"
EXTRA_ADDR="${EXTRA_ADDR:-172.16.223.59}"
CLIENT_A="${CLIENT_A:-fcstor007.ib}"
CLIENT_B="${CLIENT_B:-fcstor008.ib}"
SEED="${SEED:-172.16.223.57:19810}"
N="${N:-32}"
BITS="${BITS:-3}"
SC=$((1 << BITS))
MNT=/tmp/efs/mnt-s3
EXPORT=efs-s3
FAIL=0

say() { echo "[phase3] $*"; }
bad() { echo "[phase3] FAIL: $*"; FAIL=$((FAIL+1)); }

wait_heal() {
    local i
    for i in $(seq 1 60); do
        local st
        st=$($SSH "$PRIMARY" "cd /tmp/efs && ./efs-mgmt status $SEED 2>/dev/null" || true)
        echo "$st" | grep -q 'Cluster state: OK' || { sleep 2; continue; }
        echo "$st" | grep -q healing && { sleep 2; continue; }
        return 0
    done
    return 1
}

mount_s3() {
    local h=$1
    $SSH "$h" "mkdir -p $MNT
        if ! grep -q \"efs-fuse $MNT \" /proc/mounts; then
            cd /tmp/efs
            setsid ./efs-fuse $SEED $EXPORT $MNT >fuse-s3.log 2>&1 </dev/null &
            for i in \$(seq 1 30); do
                sleep 0.5
                grep -q \"efs-fuse $MNT \" /proc/mounts && exit 0
            done
            echo mount-timeout; tail -12 fuse-s3.log; exit 1
        fi"
}

umount_s3() {
    local h=$1
    $SSH "$h" "fusermount3 -uz $MNT 2>/dev/null; sleep 1; true"
}

start_extra_owner() {
    $SSH "$EXTRA_OWNER" "cd /tmp/efs
        pgrep -x efsd >/dev/null && exit 0
        (setsid ./efsd --node-id $EXTRA_NODE_ID --addr $EXTRA_ADDR --port 19810 \
            --storage /data1/01/efs,/data1/02/efs,/data1/03/efs,/data1/04/efs,/data1/05/efs,/data1/06/efs \
            --quota 36T --direct-io --join 172.16.223.57:19810 \
            >efsd.log 2>&1 </dev/null &)
        echo started; exit 0"
}

say "ensure throwaway export $EXPORT bits=$BITS (leave efs-test alone)"
$SSH "$PRIMARY" "cd /tmp/efs
    ./efs-mgmt destroy $SEED $EXPORT >/tmp/s3-rm.out 2>&1 || true
    ./efs-mgmt mkfs $SEED $EXPORT && ./efs-mgmt upgrade $SEED $EXPORT $BITS" || {
    echo "[phase3] mkfs/upgrade failed"; exit 2
}

say "mount $EXPORT on $CLIENT_A and $CLIENT_B"
mount_s3 "$CLIENT_A" || { echo "[phase3] mount A failed"; exit 2; }
mount_s3 "$CLIENT_B" || { echo "[phase3] mount B failed"; exit 2; }

DIR="p3-$$"
$SSH "$CLIENT_A" "rm -rf $MNT/p3-* 2>/dev/null; mkdir -p $MNT/$DIR" || {
    echo "[phase3] mkdir failed"; exit 2
}

say "create $N files + 64 KiB payload on $CLIENT_A"
INOS=$($SSH "$CLIENT_A" "cd $MNT/$DIR || exit 2
    for i in \$(seq 1 $N); do
        dd if=/dev/zero of=f-\$i bs=4096 count=16 status=none conv=fsync || exit 2
        printf 'p3-%s-%s\\n' \$i \$(hostname) | dd of=f-\$i conv=notrunc status=none
        stat -c %i f-\$i
    done")

say "inos:" $INOS
declare -A SHARDS=()
NONPRIM=0
while read -r ino; do
    [ -z "$ino" ] && continue
    sh=$((ino & (SC - 1)))
    SHARDS[$sh]=1
    if [ $((sh % 4)) -ne 0 ]; then
        NONPRIM=1
    fi
    echo "  ino=$ino shard=$sh"
done <<< "$INOS"

NSH=${#SHARDS[@]}
say "distinct shards=$NSH (want $SC) non-primary-owned=$NONPRIM"
if [ "$NSH" -ne "$SC" ]; then
    bad "expected $SC distinct shards, got $NSH (${!SHARDS[*]})"
fi
if [ "$NONPRIM" -eq 0 ]; then
    bad "all files landed on primary-owned shards"
fi

say "wait extras flush"
sleep 2
wait_heal || bad "heal not idle after create"

MD5A=$($SSH "$CLIENT_A" "cd $MNT/$DIR && md5sum f-*")
MD5B=$($SSH "$CLIENT_B" "cd $MNT/$DIR && md5sum f-*")
SUMSA=$(echo "$MD5A" | awk 'NF>=2{print $1}' | sort)
SUMSB=$(echo "$MD5B" | awk 'NF>=2{print $1}' | sort)
if [ "$SUMSA" != "$SUMSB" ] || [ -z "$SUMSA" ]; then
    bad "md5 mismatch before kill A vs B"
fi

say "kill extra-shard owner $EXTRA_OWNER (node $EXTRA_NODE_ID)"
$SSH "$EXTRA_OWNER" "pkill -9 -x efsd; sleep 1; pgrep -x efsd && echo still-up || echo down"

say "failover read on $CLIENT_B"
sleep 3
MD5B2=$($SSH "$CLIENT_B" "cd $MNT/$DIR && md5sum f-*")
SUMSB2=$(echo "$MD5B2" | awk 'NF>=2{print $1}' | sort)
if [ "$SUMSA" != "$SUMSB2" ]; then
    bad "failover md5 mismatch after extra-owner kill"
    echo "$MD5B2"
fi

say "restart extra-shard owner"
start_extra_owner
sleep 3
wait_heal || bad "heal not idle after extra-owner restart"

say "owner-only extras: only owned shards should rebuild on $EXTRA_OWNER"
OWNED=$($SSH "$EXTRA_OWNER" "grep -E 'rebuilt export=$EXPORT shard=' /tmp/efs/efsd.log | tail -20" || true)
echo "$OWNED"
# node 3 with 4 live owns shards 2 and 6 (and nothing else extra)
while read -r line; do
    [ -z "$line" ] && continue
    sh=$(echo "$line" | sed -n 's/.*shard=\([0-9]*\).*/\1/p')
    [ -z "$sh" ] && continue
    rem=$((sh % 4))
    if [ "$rem" -ne $((EXTRA_NODE_ID - 1)) ]; then
        bad "node $EXTRA_NODE_ID rebuilt unowned extra shard=$sh"
    fi
done <<< "$OWNED"

say "cold remount $CLIENT_A"
umount_s3 "$CLIENT_A"
mount_s3 "$CLIENT_A" || { echo "[phase3] remount A failed"; exit 2; }
MD5A2=$($SSH "$CLIENT_A" "cd $MNT/$DIR && md5sum f-*")
SUMSA2=$(echo "$MD5A2" | awk 'NF>=2{print $1}' | sort)
if [ "$SUMSA" != "$SUMSA2" ]; then
    bad "cold-mount md5 mismatch after extra-owner restart"
    echo "$MD5A2"
fi

say "post-restart creates (owner flush persist)"
POST=$($SSH "$CLIENT_A" "cd $MNT/$DIR || exit 2
    for i in \$(seq 1 8); do
        dd if=/dev/zero of=g-\$i bs=4096 count=8 status=none conv=fsync || exit 2
        printf 'p3-post-%s\\n' \$i | dd of=g-\$i conv=notrunc status=none
        stat -c %i g-\$i
    done")
echo "post inos:" $POST
sleep 2
wait_heal || bad "heal not idle after post-restart creates"

say "kill+restart extra owner again (persist new extras)"
$SSH "$EXTRA_OWNER" "pkill -9 -x efsd; sleep 1; true"
start_extra_owner
sleep 3
wait_heal || bad "heal not idle after second extra-owner restart"
umount_s3 "$CLIENT_A"
mount_s3 "$CLIENT_A" || { echo "[phase3] remount A failed"; exit 2; }
POST_N=$($SSH "$CLIENT_A" "ls $MNT/$DIR/g-* 2>/dev/null | wc -l")
if [ "${POST_N:-0}" -lt 8 ]; then
    bad "post-restart files missing after second bounce (got $POST_N)"
fi
MD5G=$($SSH "$CLIENT_A" "cd $MNT/$DIR && md5sum g-*")
echo "$MD5G"
GCOUNT=$(echo "$MD5G" | awk 'NF>=2{print $1}' | grep -c .)
if [ "$GCOUNT" -lt 8 ]; then
    bad "post-restart md5 count $GCOUNT want 8"
fi

say "cleanup s3 mounts (leave export for inspection)"
umount_s3 "$CLIENT_A"
umount_s3 "$CLIENT_B"

if [ "$FAIL" -eq 0 ]; then
    say "PASS shards=$NSH failover+restart+persist OK"
    exit 0
fi
say "FAIL ($FAIL)"
exit 1
