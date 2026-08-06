#!/bin/bash
# 4-server + 2-client harness: kill server 4 while clients mv 10 MiB files
# onto the FUSE mount, then verify sha256 after sync/read-back.
set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
cd "$REPO"

# shellcheck source=lib-harness.sh
source "$REPO/slurm-jobs/lib-harness.sh"

echo "Building efs binaries via Slurm..."
BUILD_JOB=$(sbatch --parsable --wait slurm-jobs/build.sh)
echo "Build job: $BUILD_JOB"
if [ ! -x "$REPO/efsd" ] || [ ! -x "$REPO/efs-fuse" ] || [ ! -x "$REPO/efs-mgmt" ]; then
    echo "Build failed; binaries missing. See $SHARED/logs/build-${BUILD_JOB}.out"
    exit 1
fi

echo "Cleaning previous harness state..."
rm -rf "$SHARED/state"
mkdir -p "$SHARED/state" "$SHARED/logs"

PREV_JOBS=$(squeue -h -o %i -n efs-s1,efs-s2,efs-s3,efs-s4,efs-ckill1,efs-ckill2 2>/dev/null || true)
if [ -n "$PREV_JOBS" ]; then
    echo "Cancelling previous jobs: $PREV_JOBS"
    echo "$PREV_JOBS" | xargs -r scancel || true
    sleep 5
fi

echo "Submitting 4 server jobs on distinct nodes..."
JOB1=$(sbatch --parsable slurm-jobs/server1.sh)
echo "  s1 job $JOB1"
efs_wait_addr 1 300
N1=$(efs_slurm_node "$(cat "$SHARED/state/s1.host")")

JOB2=$(sbatch --parsable --exclude="$N1" slurm-jobs/server2.sh)
echo "  s2 job $JOB2 (exclude=$N1)"
efs_wait_addr 2 300
N2=$(efs_slurm_node "$(cat "$SHARED/state/s2.host")")

JOB3=$(sbatch --parsable --exclude="${N1},${N2}" slurm-jobs/server3.sh)
echo "  s3 job $JOB3 (exclude=${N1},${N2})"
efs_wait_addr 3 300
N3=$(efs_slurm_node "$(cat "$SHARED/state/s3.host")")

JOB4=$(sbatch --parsable --exclude="${N1},${N2},${N3}" slurm-jobs/server4.sh)
echo "  s4 job $JOB4 (exclude=${N1},${N2},${N3})"
efs_wait_addr 4 300
N4=$(efs_slurm_node "$(cat "$SHARED/state/s4.host")")

efs_assert_distinct_servers 4
for i in 1 2 3 4; do
    echo "  server $i: $(cat "$SHARED/state/s${i}.addr")"
done

EXCLUDE_SERVERS="${N1},${N2},${N3},${N4}"
echo "Submitting client jobs (exclude=$EXCLUDE_SERVERS)..."
CLIENT1=$(sbatch --parsable --exclude="$EXCLUDE_SERVERS" slurm-jobs/client-kill4-1.sh)
CLIENT2=$(sbatch --parsable --exclude="$EXCLUDE_SERVERS" slurm-jobs/client-kill4-2.sh)
echo "Client jobs: $CLIENT1 $CLIENT2"

echo "Waiting for both clients to finish writing/checksumming local files..."
WAITED=0
LAST_SQ_CHECK=-999
while [ ! -f "$SHARED/state/c1.ready" ] || [ ! -f "$SHARED/state/c2.ready" ]; do
    sleep 2
    WAITED=$((WAITED + 2))
    if [ "$WAITED" -ge 600 ]; then
        echo "Timed out waiting for client ready flags"
        scancel "$CLIENT1" "$CLIENT2" "$JOB1" "$JOB2" "$JOB3" "$JOB4" 2>/dev/null || true
        exit 1
    fi
    # Poll Slurm at most once per minute.
    if [ $((WAITED - LAST_SQ_CHECK)) -ge 60 ]; then
        LAST_SQ_CHECK=$WAITED
        for job in "$CLIENT1" "$CLIENT2"; do
            if ! squeue -h -j "$job" 2>/dev/null | grep -q .; then
                st=$(sacct -n -X -j "$job" -o State --parsable2 2>/dev/null | head -1 || true)
                if [ -n "$st" ] && [ "$st" != "RUNNING" ] && [ "$st" != "PENDING" ] && [ "$st" != "COMPLETING" ]; then
                    echo "Client job $job ended early ($st) before ready"
                    scancel "$CLIENT1" "$CLIENT2" "$JOB1" "$JOB2" "$JOB3" "$JOB4" 2>/dev/null || true
                    exit 1
                fi
            fi
        done
    fi
done
echo "Both clients ready."

echo "Releasing go.copy and killing s4 while mv is in flight..."
touch "$SHARED/state/go.copy"

# Wait until at least one client has started copying, then kill s4 quickly.
WAITED=0
while [ ! -f "$SHARED/state/c1.copying" ] && [ ! -f "$SHARED/state/c2.copying" ]; do
    sleep 0.5
    WAITED=$((WAITED + 1))
    if [ "$WAITED" -ge 120 ]; then
        echo "Timed out waiting for copying flag"
        break
    fi
done
echo "Killing server 4 job $JOB4 now..."
scancel --signal=KILL "$JOB4" 2>/dev/null || scancel "$JOB4" 2>/dev/null || true
touch "$SHARED/state/s4.killed"
echo "s4 killed at $(date)"

echo "Waiting for client jobs to finish (poll <=1/min)..."
for job in "$CLIENT1" "$CLIENT2"; do
    while squeue -h -j "$job" 2>/dev/null | grep -q .; do
        sleep 60
    done
    echo "  job $job finished"
done

OK=1
for c in 1 2; do
    if [ -f "$SHARED/state/c${c}.ok" ]; then
        echo "client $c: OK"
    else
        echo "client $c: FAILED (no ok flag)"
        OK=0
    fi
done

echo "Tearing down remaining servers..."
for job in "$JOB1" "$JOB2" "$JOB3"; do
    scancel "$job" 2>/dev/null || true
done
sleep 5
REMAINING=$(squeue -h -o %i -n efs-s1,efs-s2,efs-s3,efs-s4,efs-ckill1,efs-ckill2 2>/dev/null || true)
if [ -n "$REMAINING" ]; then
    echo "$REMAINING" | xargs -r scancel 2>/dev/null || true
fi

echo "Logs: $SHARED/logs/"
echo "Checksums:"
for c in 1 2; do
    echo "--- client $c orig ---"
    cat "$SHARED/state/c${c}.sums.orig" 2>/dev/null || echo "(missing)"
    echo "--- client $c new ---"
    cat "$SHARED/state/c${c}.sums.new" 2>/dev/null || echo "(missing)"
done

if [ "$OK" != 1 ]; then
    echo "HARNESS FAILED"
    exit 1
fi
echo "HARNESS OK"
