#!/bin/bash
# shard_spread_probe.sh — Stage 0 compass for Phase 3 data-path sharding.
#
# Creates 32 files on a bits=3 export, asserts they land in all 8 shards
# (and at least one non-primary-owned shard), writes 512 KiB each, and
# verifies content from a second client.
#
# Usage (from the login node, cluster already up):
#   tests/stress/shard_spread_probe.sh
#
# Env:
#   SSH          efs-ssh wrapper (default: ~/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh)
#   PRIMARY      seed node (default: fcstor003.ib)
#   CLIENT_A     writer (default: fcstor007.ib)
#   CLIENT_B     reader (default: fcstor008.ib)
#   SEED         primary mgmt addr (default: 172.16.223.57:19810)
#   N            file count (default: 32)
#   BITS         shard bits (default: 3 → 8 shards)
set -u
SSH="${SSH:-$HOME/.cursor/skills/efs-test-ssh/scripts/efs-ssh.sh}"
PRIMARY="${PRIMARY:-fcstor003.ib}"
CLIENT_A="${CLIENT_A:-fcstor007.ib}"
CLIENT_B="${CLIENT_B:-fcstor008.ib}"
SEED="${SEED:-172.16.223.57:19810}"
N="${N:-32}"
BITS="${BITS:-3}"
SC=$((1 << BITS))
MNT=/tmp/efs/mnt-s3
EXPORT=efs-s3
FAIL=0

say() { echo "[spread-probe] $*"; }
bad() { echo "[spread-probe] FAIL: $*"; FAIL=$((FAIL+1)); }

say "ensure export $EXPORT bits=$BITS"
$SSH "$PRIMARY" "cd /tmp/efs
    if ./efs-mgmt mkfs $SEED $EXPORT >/tmp/s3-mkfs.out 2>&1; then
        ./efs-mgmt upgrade $SEED $EXPORT $BITS
    else
        echo already-exists
    fi" || {
    echo "[spread-probe] mkfs/upgrade failed"; exit 2
}

mount_s3() { # host
    local h=$1
    $SSH "$h" "mkdir -p $MNT
        if ! grep -q \"efs-fuse $MNT \" /proc/mounts; then
            cd /tmp/efs
            setsid ./efs-fuse $SEED $EXPORT $MNT >fuse-s3.log 2>&1 </dev/null &
            for i in \$(seq 1 20); do
                sleep 0.5
                grep -q \"efs-fuse $MNT \" /proc/mounts && exit 0
            done
            echo mount-timeout; tail -8 fuse-s3.log; exit 1
        fi"
}

say "mount $EXPORT on $CLIENT_A and $CLIENT_B"
mount_s3 "$CLIENT_A" || { echo "[spread-probe] mount A failed"; exit 2; }
mount_s3 "$CLIENT_B" || { echo "[spread-probe] mount B failed"; exit 2; }

DIR="spread-probe-$$"
$SSH "$CLIENT_A" "rm -rf $MNT/spread-probe $MNT/spread-probe-* 2>/dev/null; mkdir -p $MNT/$DIR" || {
    echo "[spread-probe] mkdir failed"; exit 2
}

say "create $N files on $CLIENT_A in $DIR"
INOS=$($SSH "$CLIENT_A" "cd $MNT/$DIR || exit 2
    for i in \$(seq 1 $N); do
        touch f-\$i || { echo TOUCH_FAIL \$i >&2; continue; }
        stat -c %i f-\$i
    done")

say "inos:" $INOS
declare -A SHARDS=()
NONPRIM=0
# primary owns shards s where s % nlive == 0 (sorted live [1,2,3,4], nlive=4)
# shard 0,4 owned by node 1 (primary). Any other shard is non-primary.
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

say "write 512 KiB + md5 on $CLIENT_A"
MD5A=$($SSH "$CLIENT_A" "cd $MNT/$DIR || exit 2
    for i in \$(seq 1 $N); do
        dd if=/dev/urandom of=f-\$i bs=65536 count=8 status=none 2>/dev/null || exit 2
    done
    sync
    md5sum f-*")
echo "$MD5A"

say "verify md5 on $CLIENT_B"
MD5B=$($SSH "$CLIENT_B" "cd $MNT/$DIR && md5sum f-*")
echo "$MD5B"

SUMSA=$(echo "$MD5A" | awk 'NF>=2{print $1}' | sort)
SUMSB=$(echo "$MD5B" | awk 'NF>=2{print $1}' | sort)
NA=$(echo "$SUMSA" | grep -c .)
NB=$(echo "$SUMSB" | grep -c .)
if [ "$NA" -lt "$N" ] || [ "$NB" -lt "$N" ]; then
    bad "md5 count A=$NA B=$NB want $N (dir vanished or write failed)"
fi
if [ "$SUMSA" != "$SUMSB" ]; then
    bad "md5 mismatch between $CLIENT_A and $CLIENT_B"
fi

if [ "$FAIL" -eq 0 ]; then
    say "PASS distinct_shards=$NSH non_primary=1 files=$N"
    exit 0
fi
say "FAIL ($FAIL)"
exit 1
