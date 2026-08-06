#!/bin/bash
# Reusable harness: submit a 3-server efs cluster on distinct nodes and 2 client
# test jobs via Slurm. when the harness finishes, always cleanup after yourself
# in /scratch. (server/client job scripts own their /scratch teardown)

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
cd "$REPO"

# shellcheck source=lib-harness.sh
source "$REPO/slurm-jobs/lib-harness.sh"

# Build on a compute node, never on the login node.
echo "Building efs binaries via Slurm..."
BUILD_JOB=$(sbatch --parsable --wait slurm-jobs/build.sh)
echo "Build job: $BUILD_JOB"
if [ ! -x "$REPO/efsd" ] || [ ! -x "$REPO/efs-fuse" ] || [ ! -x "$REPO/efs-mgmt" ]; then
    echo "Build failed; binaries missing. See $SHARED/logs/build-${BUILD_JOB}.out"
    exit 1
fi

# Burn down any previous harness state (shared across all nodes). Clear logs
# too so each run's log files are unambiguous.
echo "Cleaning up previous harness state..."
rm -rf "$SHARED/state" "$SHARED/logs"
mkdir -p "$SHARED/state" "$SHARED/logs"

# Cancel any previous efs Slurm jobs from this harness.
PREV_JOBS=$(squeue -h -o %i -n efs-s1,efs-s2,efs-s3,efs-client1,efs-client2 2>/dev/null || true)
if [ -n "$PREV_JOBS" ]; then
    echo "Cancelling previous jobs: $PREV_JOBS"
    echo "$PREV_JOBS" | xargs -r scancel || true
    sleep 5
fi

# Submit servers onto three distinct nodes via --exclude.
echo "Submitting server jobs on distinct nodes..."
JOB1=$(sbatch --parsable slurm-jobs/server1.sh)
echo "  s1 job $JOB1"
efs_wait_addr 1 300
N1=$(efs_slurm_node "$(cat "$SHARED/state/s1.host")")
echo "  s1 on $N1"

JOB2=$(sbatch --parsable --exclude="$N1" slurm-jobs/server2.sh)
echo "  s2 job $JOB2 (exclude=$N1)"
efs_wait_addr 2 300
N2=$(efs_slurm_node "$(cat "$SHARED/state/s2.host")")
echo "  s2 on $N2"

JOB3=$(sbatch --parsable --exclude="${N1},${N2}" slurm-jobs/server3.sh)
echo "  s3 job $JOB3 (exclude=${N1},${N2})"
efs_wait_addr 3 300
N3=$(efs_slurm_node "$(cat "$SHARED/state/s3.host")")
echo "  s3 on $N3"

efs_assert_distinct_servers
for i in 1 2 3; do
    echo "  server $i: $(cat "$SHARED/state/s${i}.addr")"
done

EXCLUDE_SERVERS="${N1},${N2},${N3}"
echo "Submitting client jobs (exclude=$EXCLUDE_SERVERS)..."
CLIENT1=$(sbatch --parsable --exclude="$EXCLUDE_SERVERS" slurm-jobs/client1.sh)
CLIENT2=$(sbatch --parsable --exclude="$EXCLUDE_SERVERS" slurm-jobs/client2.sh)
echo "Client jobs: $CLIENT1 $CLIENT2"

# Wait for client jobs to finish, polling squeue no faster than once per minute.
echo "Waiting for client jobs to finish..."
for job in "$CLIENT1" "$CLIENT2"; do
    while squeue -h -j "$job" 2>/dev/null | grep -q .; do
        sleep 60
    done
    echo "  job $job finished"
done

echo "Client jobs completed. Tearing down servers..."
for job in "$JOB1" "$JOB2" "$JOB3"; do
    scancel "$job" 2>/dev/null || true
done

# Make sure no stragglers remain.
sleep 5
REMAINING=$(squeue -h -o %i -n efs-s1,efs-s2,efs-s3,efs-client1,efs-client2 2>/dev/null || true)
if [ -n "$REMAINING" ]; then
    echo "$REMAINING" | xargs -r scancel 2>/dev/null || true
fi

echo "Done. Logs are in $SHARED/logs/"
