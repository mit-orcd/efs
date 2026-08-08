#!/bin/bash
#SBATCH --job-name=efs-smoke-stripe
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=1
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-stripe-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-stripe-%j.err
#
# Smoke: multi-path least-queue striping (not local EC).
#   - n=2 accepted; banner stripe=leastq
#   - write spreads full fragments across disks (no .s* EC shards)
#   - cluster 2+1 still covers node loss; local disks have no parity
#
set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
OUT="$SHARED/logs/smoke-stripe-${SLURM_JOB_ID}"
LOCAL="/scratch/efs-testing/${SLURM_JOB_ID}"
mkdir -p "$OUT" "$SHARED/logs"

IP="127.0.0.1"
FAIL=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAIL=1; }

cleanup() {
    set +e
    for pid in ${CPID:-} ${S1:-} ${S2:-} ${S3:-}; do
        kill -INT "$pid" 2>/dev/null || true
    done
    sleep 1
    for pid in ${CPID:-} ${S1:-} ${S2:-} ${S3:-}; do
        kill -KILL "$pid" 2>/dev/null || true
    done
    fusermount -u "$LOCAL/mnt" 2>/dev/null || umount "$LOCAL/mnt" 2>/dev/null || true
    if [ -n "${LOCAL:-}" ]; then
        echo "cleaning /scratch: $LOCAL"
        rm -rf "$LOCAL"
    fi
}
trap cleanup EXIT

rm -rf "$LOCAL"
mkdir -p "$LOCAL" "$OUT"

echo "smoke-stripe on $(hostname) LOCAL=$LOCAL"

# n=2 must be accepted (was rejected under local EC)
mkdir -p "$LOCAL/n2/d1" "$LOCAL/n2/d2"
fuser -k 1998/tcp 2>/dev/null || true
sleep 0.3
"$REPO/efsd" --node-id 1 --addr "$IP" --port 1998 \
    --storage "$LOCAL/n2/d1" --storage "$LOCAL/n2/d2" \
    > "$OUT/n2.stdout" 2>&1 &
N2PID=$!
for i in $(seq 1 120); do
    grep -q "listening on" "$OUT/n2.stdout" 2>/dev/null && break
    kill -0 "$N2PID" 2>/dev/null || break
    sleep 0.25
done
if grep -q "stripe=leastq" "$OUT/n2.stdout" && grep -q "writers=8/path (16 total)" "$OUT/n2.stdout"; then
    pass "n=2 accepted stripe=leastq writers=8/path"
else
    fail "n=2 banner"
    cat "$OUT/n2.stdout" || true
fi
kill -INT "$N2PID" 2>/dev/null || true
sleep 0.5
kill -KILL "$N2PID" 2>/dev/null || true
wait "$N2PID" 2>/dev/null || true

run_stripe() {
    local tag=$1
    local ndisks=$2

    local base="$LOCAL/$tag"
    local mnt="$base/mnt"
    mkdir -p "$mnt"
    local storage_args=()
    local disks=()
    for i in $(seq 1 "$ndisks"); do
        mkdir -p "$base/d$i"
        disks+=("$base/d$i")
        storage_args+=(--storage "$base/d$i")
    done

    local p1=$((1900 + RANDOM % 100))
    local p2=$((p1 + 1))
    local p3=$((p1 + 2))

    for port in "$p1" "$p2" "$p3"; do
        fuser -k "${port}/tcp" 2>/dev/null || true
    done
    sleep 0.5

    "$REPO/efsd" --node-id 1 --addr "$IP" --port "$p1" \
        "${storage_args[@]}" --no-direct-io \
        > "$OUT/${tag}-s1.stdout" 2>&1 &
    S1=$!
    for i in $(seq 1 120); do
        grep -q "listening on" "$OUT/${tag}-s1.stdout" 2>/dev/null && break
        kill -0 "$S1" 2>/dev/null || { cat "$OUT/${tag}-s1.stdout"; return 1; }
        sleep 0.25
    done
    grep -q "stripe=leastq" "$OUT/${tag}-s1.stdout" || { fail "$tag s1 banner"; return 1; }
    local expect_total=$((8 * ndisks))
    grep -q "writers=8/path ($expect_total total)" "$OUT/${tag}-s1.stdout" \
        && pass "$tag writers=8/path ($expect_total total)" \
        || fail "$tag writers banner"
    pass "$tag stripe=leastq banner"

    mkdir -p "$base/s2" "$base/s3"
    "$REPO/efsd" --node-id 2 --addr "$IP" --port "$p2" --storage "$base/s2" \
        --join "$IP:$p1" --no-direct-io > "$OUT/${tag}-s2.stdout" 2>&1 &
    S2=$!
    "$REPO/efsd" --node-id 3 --addr "$IP" --port "$p3" --storage "$base/s3" \
        --join "$IP:$p1" --no-direct-io > "$OUT/${tag}-s3.stdout" 2>&1 &
    S3=$!
    sleep 2

    "$REPO/efs-mgmt" mkfs "$IP:$p1" "stripe-$tag" || return 1

    mount_fuse() {
        "$REPO/efs-fuse" "$IP:$p1" "$IP:$p2" "$IP:$p3" "stripe-$tag" "$mnt" -f \
            > "$OUT/${tag}-fuse.stdout" 2>&1 &
        CPID=$!
        for i in $(seq 1 40); do
            mountpoint -q "$mnt" 2>/dev/null && return 0
            kill -0 "$CPID" 2>/dev/null || return 1
            sleep 0.25
        done
        mountpoint -q "$mnt" 2>/dev/null
    }
    unmount_fuse() {
        fusermount -u "$mnt" 2>/dev/null || umount "$mnt" 2>/dev/null || true
        kill -INT "$CPID" 2>/dev/null || true
        sleep 0.5
        kill -KILL "$CPID" 2>/dev/null || true
        wait "$CPID" 2>/dev/null || true
        CPID=
    }

    mount_fuse || { fail "$tag mount"; cat "$OUT/${tag}-fuse.stdout"; return 1; }

    # Enough chunks to hit every stripe lane (128 KiB chunks).
    dd if=/dev/urandom of="$mnt/big.bin" bs=128K count=16 status=none
    sync
    sleep 1
    unmount_fuse

    local nonempty=0
    for d in "${disks[@]}"; do
        local bytes
        bytes=$(du -sb "$d/data" 2>/dev/null | awk '{print $1}')
        if [ "${bytes:-0}" -gt 0 ]; then
            nonempty=$((nonempty + 1))
        fi
    done
    if [ "$nonempty" -ge 2 ]; then
        pass "$tag data on $nonempty/$ndisks disks"
    else
        fail "$tag expected stripe across disks (got $nonempty nonempty)"
        du -sh "${disks[@]}/data" 2>/dev/null || true
    fi

    # Layout: full 64 KiB fragments, no legacy local-EC .sN shard names.
    local frag
    frag=$(find "${disks[0]}/data" -type f -name '*.[0-9]' 2>/dev/null | head -1 || true)
    if [ -z "$frag" ]; then
        frag=$(find "$base"/d*/data -type f ! -name '*.sum' ! -name '*.s*' 2>/dev/null | head -1 || true)
    fi
    if [ -n "$frag" ]; then
        local fsz
        fsz=$(stat -c%s "$frag" 2>/dev/null || echo 0)
        if [ "$fsz" -eq 65536 ]; then
            pass "$tag full-fragment file size 65536 ($frag)"
        else
            fail "$tag expected 65536-byte fragment, got $fsz ($frag)"
        fi
    else
        fail "$tag no fragment files found under disks"
    fi
    local shards
    shards=$(find "$base"/d*/data -type f -name '*.s[0-9]*' 2>/dev/null | wc -l)
    if [ "${shards:-0}" -eq 0 ]; then
        pass "$tag no local-EC .s* shard files"
    else
        fail "$tag found $shards legacy .s* shard files"
    fi

    mount_fuse || { fail "$tag remount intact"; return 1; }
    if timeout 20 sha256sum "$mnt/big.bin" >/dev/null; then
        pass "$tag read intact"
    else
        fail "$tag read intact"
    fi
    unmount_fuse

    kill -INT "$S1" "$S2" "$S3" 2>/dev/null || true
    sleep 1
    kill -KILL "$S1" "$S2" "$S3" 2>/dev/null || true
    wait "$S1" "$S2" "$S3" 2>/dev/null || true
    S1=; S2=; S3=
}

# n=1 regression
run_n1() {
    local tag=n1
    local base="$LOCAL/$tag"
    local mnt="$base/mnt"
    mkdir -p "$base/d1" "$mnt"
    local p1=$((1950 + RANDOM % 40))
    local p2=$((p1 + 1))
    local p3=$((p1 + 2))
    for port in "$p1" "$p2" "$p3"; do
        fuser -k "${port}/tcp" 2>/dev/null || true
    done
    sleep 0.3
    mkdir -p "$base/s2" "$base/s3"
    "$REPO/efsd" --node-id 1 --addr "$IP" --port "$p1" --storage "$base/d1" \
        --no-direct-io > "$OUT/${tag}-s1.stdout" 2>&1 &
    S1=$!
    for i in $(seq 1 120); do
        grep -q "listening on" "$OUT/${tag}-s1.stdout" 2>/dev/null && break
        kill -0 "$S1" 2>/dev/null || break
        sleep 0.25
    done
    grep -q "stripe=none" "$OUT/${tag}-s1.stdout" && pass "n=1 stripe=none" \
        || { fail "n=1 banner"; cat "$OUT/${tag}-s1.stdout" || true; }
    grep -q "writers=8/path (8 total)" "$OUT/${tag}-s1.stdout" && pass "n=1 writers=8" \
        || fail "n=1 writers banner"
    "$REPO/efsd" --node-id 2 --addr "$IP" --port "$p2" --storage "$base/s2" \
        --join "$IP:$p1" --no-direct-io > "$OUT/${tag}-s2.stdout" 2>&1 &
    S2=$!
    "$REPO/efsd" --node-id 3 --addr "$IP" --port "$p3" --storage "$base/s3" \
        --join "$IP:$p1" --no-direct-io > "$OUT/${tag}-s3.stdout" 2>&1 &
    S3=$!
    sleep 2
    "$REPO/efs-mgmt" mkfs "$IP:$p1" "stripe-n1" || { fail "n1 mkfs"; return 1; }
    "$REPO/efs-fuse" "$IP:$p1" "$IP:$p2" "$IP:$p3" stripe-n1 "$mnt" -f \
        > "$OUT/${tag}-fuse.stdout" 2>&1 &
    CPID=$!
    for i in $(seq 1 40); do
        mountpoint -q "$mnt" 2>/dev/null && break
        sleep 0.25
    done
    if mountpoint -q "$mnt" 2>/dev/null; then
        echo "n1-ok" > "$mnt/f.txt"
        sync
        sleep 0.5
        if [ "$(timeout 15 cat "$mnt/f.txt")" = "n1-ok" ]; then
            pass "n=1 write/read"
        else
            fail "n=1 read"
        fi
    else
        fail "n=1 mount"
    fi
    fusermount -u "$mnt" 2>/dev/null || true
    kill -INT "$CPID" "$S1" "$S2" "$S3" 2>/dev/null || true
    sleep 1
    kill -KILL "$CPID" "$S1" "$S2" "$S3" 2>/dev/null || true
    wait "$S1" "$S2" "$S3" "$CPID" 2>/dev/null || true
    CPID=; S1=; S2=; S3=
}

run_n1 || fail "n1 scenario"
run_stripe "n4" 4 || fail "n4 scenario"

if [ "$FAIL" -eq 0 ]; then
    echo "SMOKE_STRIPE_OK"
else
    echo "SMOKE_STRIPE_FAIL"
    exit 1
fi
