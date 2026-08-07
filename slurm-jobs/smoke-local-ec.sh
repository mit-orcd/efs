#!/bin/bash
#SBATCH --job-name=efs-smoke-localec
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=1
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-localec-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/smoke-localec-%j.err
#
# Smoke: local multi-disk EC — 3 paths lose 1 disk; 4 paths lose 2 disks.

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
OUT="$SHARED/logs/smoke-localec-${SLURM_JOB_ID}"
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

echo "smoke-localec on $(hostname) LOCAL=$LOCAL"

# --- unit ---
"$REPO/tests/test_local_ec" || { fail "test_local_ec"; exit 1; }
pass "test_local_ec"
"$REPO/tests/test_numa_locality" || { fail "test_numa_locality"; exit 1; }
pass "test_numa_locality"

run_cluster_kill() {
    local tag=$1
    local ndisks=$2
    shift 2
    local kill_idxs=("$@")

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

    # Explicit --no-direct-io (also the efsd default); /scratch + O_DIRECT is flaky.
    "$REPO/efsd" --node-id 1 --addr "$IP" --port "$p1" \
        "${storage_args[@]}" --writers 2 --no-direct-io \
        > "$OUT/${tag}-s1.stdout" 2>&1 &
    S1=$!
    for i in $(seq 1 40); do
        grep -q "listening on" "$OUT/${tag}-s1.stdout" 2>/dev/null && break
        kill -0 "$S1" 2>/dev/null || { cat "$OUT/${tag}-s1.stdout"; return 1; }
        sleep 0.25
    done
    grep -q "local_ec=" "$OUT/${tag}-s1.stdout" || { fail "$tag s1 banner"; return 1; }
    # Soft NUMA discovery logs numa=<n>|unknown (presence only; scratch may be unknown).
    if grep -q "numa=" "$OUT/${tag}-s1.stdout"; then
        pass "$tag storage numa= logged"
    else
        fail "$tag missing numa= in s1 log"
        return 1
    fi

    # Single-node is enough to exercise local EC (network 2+1 still wants 3 for
    # cluster writes). Use a 3-node cluster with only node1 multi-disk.
    mkdir -p "$base/s2" "$base/s3"
    "$REPO/efsd" --node-id 2 --addr "$IP" --port "$p2" --storage "$base/s2" \
        --join "$IP:$p1" --writers 2 --no-direct-io > "$OUT/${tag}-s2.stdout" 2>&1 &
    S2=$!
    "$REPO/efsd" --node-id 3 --addr "$IP" --port "$p3" --storage "$base/s3" \
        --join "$IP:$p1" --writers 2 --no-direct-io > "$OUT/${tag}-s3.stdout" 2>&1 &
    S3=$!
    sleep 2

    "$REPO/efs-mgmt" mkfs "$IP:$p1" "lec-$tag" || return 1

    mount_fuse() {
        "$REPO/efs-fuse" "$IP:$p1" "$IP:$p2" "$IP:$p3" "lec-$tag" "$mnt" -f \
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
    # Soft NIC NUMA log (presence only; unknown OK on loopback/scratch).
    if grep -q "nic_numa=" "$OUT/${tag}-fuse.stdout"; then
        pass "$tag fuse nic_numa= logged"
    else
        fail "$tag missing nic_numa= in fuse log"
    fi

    local payload="hello-local-ec-$tag"
    echo "$payload" > "$mnt/file.txt"
    sync
    sleep 1
    # Unmount so the re-read cannot be satisfied from client cache.
    unmount_fuse

    for idx in "${kill_idxs[@]}"; do
        echo "killing disk index $idx under ${disks[$idx]}"
        rm -rf "${disks[$idx]}/data"
        mkdir -p "${disks[$idx]}/data"
    done

    mount_fuse || { fail "$tag remount"; cat "$OUT/${tag}-fuse.stdout"; return 1; }

    local after
    after=$(timeout 15 cat "$mnt/file.txt" || true)
    if [ "$payload" = "$after" ]; then
        pass "$tag read after killing ${#kill_idxs[@]} disk(s)"
    else
        fail "$tag read after kill (got '$after')"
        ls -laR "${disks[0]}/.." 2>/dev/null | head -80 || true
    fi

    unmount_fuse
    kill -INT "$S1" "$S2" "$S3" 2>/dev/null || true
    sleep 1
    kill -KILL "$S1" "$S2" "$S3" 2>/dev/null || true
    wait "$S1" "$S2" "$S3" 2>/dev/null || true
    S1=; S2=; S3=
}

# n=2 must fail startup
mkdir -p "$LOCAL/bad1" "$LOCAL/bad2"
if "$REPO/efsd" --node-id 1 --addr "$IP" --port 1999 \
    --storage "$LOCAL/bad1" --storage "$LOCAL/bad2" \
    > "$OUT/n2.stdout" 2>&1; then
    fail "n=2 should reject"
else
    grep -qi "not 2" "$OUT/n2.stdout" && pass "n=2 rejected" || pass "n=2 rejected (nonzero exit)"
fi

# n=1 regression: single path, no local EC, basic write/read.
run_n1_regression() {
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
        --writers 2 --no-direct-io > "$OUT/${tag}-s1.stdout" 2>&1 &
    S1=$!
    for i in $(seq 1 40); do
        grep -q "listening on" "$OUT/${tag}-s1.stdout" 2>/dev/null && break
        sleep 0.25
    done
    grep -q "local_ec=none" "$OUT/${tag}-s1.stdout" && pass "n=1 local_ec=none" \
        || fail "n=1 banner"
    grep -q "numa=" "$OUT/${tag}-s1.stdout" && pass "n=1 numa= logged" \
        || fail "n=1 missing numa="
    "$REPO/efsd" --node-id 2 --addr "$IP" --port "$p2" --storage "$base/s2" \
        --join "$IP:$p1" --writers 2 --no-direct-io > "$OUT/${tag}-s2.stdout" 2>&1 &
    S2=$!
    "$REPO/efsd" --node-id 3 --addr "$IP" --port "$p3" --storage "$base/s3" \
        --join "$IP:$p1" --writers 2 --no-direct-io > "$OUT/${tag}-s3.stdout" 2>&1 &
    S3=$!
    sleep 2
    "$REPO/efs-mgmt" mkfs "$IP:$p1" "lec-n1" || { fail "n1 mkfs"; return 1; }
    "$REPO/efs-fuse" "$IP:$p1" "$IP:$p2" "$IP:$p3" lec-n1 "$mnt" -f \
        > "$OUT/${tag}-fuse.stdout" 2>&1 &
    CPID=$!
    for i in $(seq 1 40); do
        mountpoint -q "$mnt" 2>/dev/null && break
        sleep 0.25
    done
    if mountpoint -q "$mnt" 2>/dev/null; then
        grep -q "nic_numa=" "$OUT/${tag}-fuse.stdout" && pass "n=1 nic_numa= logged" \
            || fail "n=1 missing nic_numa="
        echo "n1-ok" > "$mnt/f.txt"
        sync
        sleep 0.5
        if [ "$(timeout 15 cat "$mnt/f.txt")" = "n1-ok" ]; then
            pass "n=1 write/read"
        else
            fail "n=1 read"
            cat "$OUT/${tag}-fuse.stdout" || true
        fi
    else
        fail "n=1 mount"
        cat "$OUT/${tag}-fuse.stdout" || true
    fi
    fusermount -u "$mnt" 2>/dev/null || true
    kill -INT "$CPID" "$S1" "$S2" "$S3" 2>/dev/null || true
    sleep 1
    kill -KILL "$CPID" "$S1" "$S2" "$S3" 2>/dev/null || true
    wait "$S1" "$S2" "$S3" "$CPID" 2>/dev/null || true
    CPID=; S1=; S2=; S3=
}
run_n1_regression || fail "n1 scenario"

run_cluster_kill "n3" 3 1 || fail "n3 scenario"
run_cluster_kill "n4" 4 0 2 || fail "n4 scenario"

if [ "$FAIL" -eq 0 ]; then
    echo "SMOKE_LOCAL_EC_OK"
else
    echo "SMOKE_LOCAL_EC_FAIL"
    exit 1
fi
