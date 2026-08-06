#!/bin/bash
#SBATCH --job-name=efs-ckill1
#SBATCH --partition=mit_normal
#SBATCH --time=00:30:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/ckill1-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/ckill1-%j.err

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
CLIENT=1
SCRATCH="/scratch/efs-testing/${SLURM_JOB_ID}"
MNT="$SCRATCH/mnt"

mkdir -p "$SHARED/state" "$SHARED/logs"
fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
rm -rf "$SCRATCH"
mkdir -p "$MNT"

cleanup() {
    set +e
    fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
    [ -n "${FUSE_PID:-}" ] && kill "$FUSE_PID" 2>/dev/null || true
    [ -n "${FUSE_PID:-}" ] && wait "$FUSE_PID" 2>/dev/null || true
    echo "cleaning /scratch: $SCRATCH"
    rm -rf "$SCRATCH" 2>/dev/null || true
}
trap cleanup EXIT

for i in 1 2 3 4; do
    while [ ! -s "$SHARED/state/s${i}.addr" ]; do
        sleep 2
    done
done

S1=$(cat "$SHARED/state/s1.addr")
for addr in "$S1" "$(cat "$SHARED/state/s2.addr")" \
            "$(cat "$SHARED/state/s3.addr")" "$(cat "$SHARED/state/s4.addr")"; do
    host=${addr%:*}
    port=${addr#*:}
    WAITED=0
    while ! timeout 2 bash -c "exec 3<>/dev/tcp/$host/$port" 2>/dev/null; do
        sleep 2
        WAITED=$((WAITED + 2))
        if [ "$WAITED" -ge 300 ]; then
            echo "Timed out waiting for $addr"
            exit 1
        fi
    done
done

# Create export once (client 2 ignores exists failure).
"$REPO/efs-mgmt" mkfs "$S1" kill4export || true
"$REPO/efs-mgmt" list-exports "$S1" || true

# Discover full 4-node membership from s1.
"$REPO/efs-fuse" "$S1" kill4export "$MNT" -f \
    > "$SHARED/logs/ckill1-fuse.log" 2>&1 &
FUSE_PID=$!

for i in $(seq 1 60); do
    if mountpoint -q "$MNT" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$FUSE_PID" 2>/dev/null; then
        echo "FUSE client exited before mounting"
        cat "$SHARED/logs/ckill1-fuse.log" 2>/dev/null || true
        exit 1
    fi
    sleep 1
done
if ! mountpoint -q "$MNT" 2>/dev/null; then
    echo "FUSE mount failed"
    cat "$SHARED/logs/ckill1-fuse.log" 2>/dev/null || true
    exit 1
fi

echo "FUSE mount OK on client $CLIENT"
RC=0
"$REPO/slurm-jobs/client-kill4-tests.sh" "$MNT" "$CLIENT" || RC=$?
if [ "$RC" != 0 ]; then
    echo "--- FUSE log ---"
    cat "$SHARED/logs/ckill1-fuse.log" 2>/dev/null || true
fi
exit "$RC"
