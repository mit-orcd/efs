#!/bin/bash
#SBATCH --job-name=efs-client1
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=1
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/client1-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/client1-%j.err

set -e

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
mkdir -p "$SHARED/state" "$SHARED/logs"
MNT="/scratch/efs/mnt1-${SLURM_JOB_ID}"
# Clean up any stale mount from a previous run on this node, then start fresh.
fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
rm -rf "$MNT"
mkdir -p "$MNT"
# when the harness finishes, always cleanup after yourself in /scratch.
cleanup() {
    set +e
    fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
    [ -n "${FUSE_PID:-}" ] && kill "$FUSE_PID" 2>/dev/null || true
    [ -n "${FUSE_PID:-}" ] && wait "$FUSE_PID" 2>/dev/null || true
    rm -rf "$MNT" 2>/dev/null || true
}
trap cleanup EXIT

# Wait for all server addresses.
for i in 1 2 3; do
    while [ ! -f "$SHARED/state/s${i}.addr" ]; do
        sleep 2
    done
done

S1=$(cat "$SHARED/state/s1.addr")
S2=$(cat "$SHARED/state/s2.addr")
S3=$(cat "$SHARED/state/s3.addr")

# Wait for TCP ports to be listening (up to 5 minutes per server).
for addr in "$S1" "$S2" "$S3"; do
    host=${addr%:*}
    port=${addr#*:}
    WAITED=0
    while ! timeout 2 bash -c "exec 3<>/dev/tcp/$host/$port" 2>/dev/null; do
        sleep 2
        WAITED=$((WAITED + 2))
        if [ "$WAITED" -ge 300 ]; then
            echo "Timed out waiting for $addr to be reachable"
            exit 1
        fi
    done
done

# Create the export (idempotent on first run, fails on subsequent runs; both are fine).
"$REPO/efs-mgmt" mkfs "$S1" myexport || true

# Mount FUSE in the foreground; background the process so we can run tests.
"$REPO/efs-fuse" "$S1" "$S2" "$S3" myexport "$MNT" -f > "$SHARED/logs/client1-fuse.log" 2>&1 &
FUSE_PID=$!

# Wait for the mount to be active.
for i in $(seq 1 30); do
    if mountpoint -q "$MNT" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$FUSE_PID" 2>/dev/null; then
        echo "FUSE client exited before mounting"
        exit 1
    fi
    sleep 1
done

if ! mountpoint -q "$MNT" 2>/dev/null; then
    echo "FUSE mount failed"
    echo "--- FUSE log ---"
    cat "$SHARED/logs/client1-fuse.log" 2>/dev/null || true
    kill "$FUSE_PID" 2>/dev/null || true
    wait "$FUSE_PID" 2>/dev/null || true
    exit 1
fi

echo "FUSE mount OK, running tests"

# Run tests.
RC=0
"$REPO/slurm-jobs/client-tests.sh" "$MNT" 1 || RC=$?

if [ "$RC" != "0" ]; then
    echo "--- FUSE log (after failure) ---"
    cat "$SHARED/logs/client1-fuse.log" 2>/dev/null || true
fi

exit $RC
