#!/bin/bash
# Multi-node medium profile over InfiniBand (hostname.ib).
# Three servers on three distinct nodes + one client excluded from those nodes.
#
# Usage: ROUND=6 PARTITION=mit_quicktest ./slurm-jobs/profile-medium-ib.sh

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
ROUND="${ROUND:-1}"
# Override with PARTITION=mit_quicktest|mit_preemptable when mit_normal is Priority-stuck.
PARTITION="${PARTITION:-mit_normal}"
cd "$REPO"

# shellcheck source=lib-harness.sh
source "$REPO/slurm-jobs/lib-harness.sh"

echo "Building efs binaries via Slurm..."
BUILD_JOB=$(sbatch --parsable --wait slurm-jobs/build.sh)
echo "Build job: $BUILD_JOB"

mkdir -p "$SHARED/state" "$SHARED/logs" "$SHARED/profile"
# Fresh state so we do not pick up stale Ethernet addresses.
rm -f "$SHARED/state"/s*.addr "$SHARED/state"/s*.host "$SHARED/state"/s*.node

PREV=$(squeue -h -o %i -n efs-s1,efs-s2,efs-s3,efs-med-cli 2>/dev/null || true)
if [ -n "$PREV" ]; then
    echo "Cancelling previous jobs: $PREV"
    echo "$PREV" | xargs -r scancel || true
    sleep 3
fi

echo "Submitting IB servers on distinct nodes (partition=$PARTITION)..."
JOB1=$(sbatch --parsable -p "$PARTITION" --time=00:15:00 slurm-jobs/server1.sh)
echo "  s1 job $JOB1 — waiting for address..."
efs_wait_addr 1 600
N1=$(efs_slurm_node "$(cat "$SHARED/state/s1.host")")
echo "  s1 on $N1 ($(cat "$SHARED/state/s1.addr"))"

JOB2=$(sbatch --parsable -p "$PARTITION" --time=00:15:00 \
    --exclude="$N1" slurm-jobs/server2.sh)
echo "  s2 job $JOB2 — waiting for address (exclude=$N1)..."
efs_wait_addr 2 600
N2=$(efs_slurm_node "$(cat "$SHARED/state/s2.host")")
echo "  s2 on $N2 ($(cat "$SHARED/state/s2.addr"))"

JOB3=$(sbatch --parsable -p "$PARTITION" --time=00:15:00 \
    --exclude="${N1},${N2}" slurm-jobs/server3.sh)
echo "  s3 job $JOB3 — waiting for address (exclude=${N1},${N2})..."
efs_wait_addr 3 600
N3=$(efs_slurm_node "$(cat "$SHARED/state/s3.host")")
echo "  s3 on $N3 ($(cat "$SHARED/state/s3.addr"))"

efs_assert_distinct_servers

# Sanity: addresses must be *.ib
for i in 1 2 3; do
    addr=$(cat "$SHARED/state/s${i}.addr")
    case "$addr" in
        *.ib:*) ;;
        *)
            echo "ERROR: s${i}.addr is not an IB hostname: $addr"
            squeue -h -o %i -n efs-s1,efs-s2,efs-s3 2>/dev/null | xargs -r scancel || true
            exit 1
            ;;
    esac
done

PROF_STAGING="$SHARED/profile/medium-ib-r${ROUND}-pending"
mkdir -p "$PROF_STAGING"
echo "$JOB1 $JOB2 $JOB3" > "$PROF_STAGING/server_jobs.txt"
echo "$N1 $N2 $N3" > "$PROF_STAGING/server_nodes.txt"
cp -f "$SHARED/state"/s*.addr "$SHARED/state"/s*.host "$SHARED/state"/s*.node \
    "$PROF_STAGING/" 2>/dev/null || true

EXCLUDE_SERVERS="${N1},${N2},${N3}"
echo "Submitting medium client over IB (partition=$PARTITION exclude=$EXCLUDE_SERVERS)..."
CLIENT=$(ROUND="$ROUND" sbatch --parsable -p "$PARTITION" --time=00:15:00 \
    --exclude="$EXCLUDE_SERVERS" \
    --export=ALL,ROUND="$ROUND" \
    slurm-jobs/client-medium.sh)
echo "Client job: $CLIENT"

echo "Waiting for client job $CLIENT ..."
while squeue -h -j "$CLIENT" 2>/dev/null | grep -q .; do
    sleep 60
    echo "  $(date +%H:%M:%S) still running; $(squeue -h -j "$CLIENT" -o '%T %M %N' 2>/dev/null || true)"
done

echo "Client finished. Tearing down servers (each job cleans its /scratch)..."
for job in "$JOB1" "$JOB2" "$JOB3"; do
    scancel "$job" 2>/dev/null || true
done
sleep 3
squeue -h -o %i -n efs-s1,efs-s2,efs-s3,efs-med-cli 2>/dev/null | xargs -r scancel || true

PROF=$(ls -d "$SHARED/profile/medium-ib-r${ROUND}-${CLIENT}" 2>/dev/null || true)
if [ -z "$PROF" ]; then
    PROF=$(ls -dt "$SHARED/profile"/medium-ib-r${ROUND}-* 2>/dev/null | head -1 || true)
fi
echo "=== results ==="
if [ -n "${PROF:-}" ] && [ -d "$PROF" ]; then
    echo "PROF=$PROF"
    cat "$PROF/IB.txt" 2>/dev/null || true
    grep -E 'write_wall_sec|sample_read' "$SHARED/logs/client-medium-${CLIENT}.out" 2>/dev/null || true
    # Confirm topology from client log
    grep -E 'medium IB client|servers \(IB\)' "$SHARED/logs/client-medium-${CLIENT}.out" 2>/dev/null || true
    tail -40 "$SHARED/logs/client-medium-${CLIENT}.out" 2>/dev/null || true
    echo "=== client report top ==="
    grep -E '^\s+[0-9]+\.[0-9]+%|Overhead' "$PROF/client.report.txt" 2>/dev/null | head -30 || true
else
    echo "No profile dir found; see $SHARED/logs/client-medium-${CLIENT}.out"
    tail -50 "$SHARED/logs/client-medium-${CLIENT}.out" 2>/dev/null || true
fi
echo "Done."
