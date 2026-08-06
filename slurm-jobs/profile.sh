#!/bin/bash
#SBATCH --job-name=efs-profile
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/profile-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/profile-%j.err
#
# Single-node profiling harness: 3 servers + 1 client with --perf.
# Profiles land under ~/orcd/scratch/efs/profile/<jobid>/.

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
PROF="$SHARED/profile/${SLURM_JOB_ID}"
LOCAL="/scratch/efs/prof-${SLURM_JOB_ID}"
mkdir -p "$PROF" "$SHARED/logs" "$LOCAL"

IP="127.0.0.1"
P1=1991
P2=1992
P3=1993
MNT="$LOCAL/mnt"

cleanup() {
    set +e
    fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
    for pid in ${CPID:-} ${S1:-} ${S2:-} ${S3:-}; do
        kill -INT "$pid" 2>/dev/null || true
    done
    sleep 1
    for pid in ${CPID:-} ${S1:-} ${S2:-} ${S3:-}; do
        kill -KILL "$pid" 2>/dev/null || true
    done
    # Copy node-local perf files to shared if they exist.
    for f in server.perf.data client.perf.data; do
        if [ -f "$LOCAL/$f" ]; then
            cp -f "$LOCAL/$f" "$PROF/$f" 2>/dev/null || true
        fi
    done
    # when the harness finishes, always cleanup after yourself in /scratch.
    if [ -n "${LOCAL:-}" ]; then
        echo "cleaning /scratch: $LOCAL"
        rm -rf "$LOCAL"
    fi
}
trap cleanup EXIT

fusermount -u "$MNT" 2>/dev/null || true
rm -rf "$LOCAL"
mkdir -p "$LOCAL/s1" "$LOCAL/s2" "$LOCAL/s3" "$MNT" "$PROF"

echo "profile job on $(hostname), PROF=$PROF LOCAL=$LOCAL"

# Kill anything leftover on our ports.
for port in "$P1" "$P2" "$P3"; do
    fuser -k "${port}/tcp" 2>/dev/null || true
done
sleep 1

# Start server 1 with perf; write perf.data to node-local scratch (fast).
EFS_PERF_PATH="$LOCAL/server.perf.data" \
    "$REPO/efsd" --node-id 1 --addr "$IP" --port "$P1" --storage "$LOCAL/s1" --perf \
    > "$PROF/s1.stdout" 2>&1 &
S1=$!

# Wait until s1 is listening (and has printed its banner).
for i in $(seq 1 40); do
    if grep -q "listening on" "$PROF/s1.stdout" 2>/dev/null; then break; fi
    if ! kill -0 "$S1" 2>/dev/null; then
        echo "server 1 died during startup:"; cat "$PROF/s1.stdout"; exit 1
    fi
    sleep 0.5
done
if ! grep -q "listening on" "$PROF/s1.stdout" 2>/dev/null; then
    echo "server 1 did not become ready:"; cat "$PROF/s1.stdout"; exit 1
fi
echo "server 1 ready (pid $S1)"

"$REPO/efsd" --node-id 2 --addr "$IP" --port "$P2" --storage "$LOCAL/s2" --join "$IP:$P1" \
    > "$PROF/s2.stdout" 2>&1 &
S2=$!
"$REPO/efsd" --node-id 3 --addr "$IP" --port "$P3" --storage "$LOCAL/s3" --join "$IP:$P1" \
    > "$PROF/s3.stdout" 2>&1 &
S3=$!

for port in "$P2" "$P3"; do
    READY=0
    for i in $(seq 1 40); do
        if timeout 1 bash -c "exec 3<>/dev/tcp/$IP/$port" 2>/dev/null; then READY=1; break; fi
        sleep 0.5
    done
    [ "$READY" = "1" ] || { echo "server on :$port did not start"; cat "$PROF"/s*.stdout; exit 1; }
done
sleep 1
echo "cluster up"

echo "mkfs: $("$REPO/efs-mgmt" mkfs "$IP:$P1" profexport 2>&1)"

EFS_PERF_PATH="$LOCAL/client.perf.data" \
    "$REPO/efs-fuse" "$IP:$P1" "$IP:$P2" "$IP:$P3" profexport "$MNT" -f --perf \
    > "$PROF/client.stdout" 2>&1 &
CPID=$!

MOUNTED=0
for i in $(seq 1 40); do
    if mountpoint -q "$MNT" 2>/dev/null; then MOUNTED=1; break; fi
    if ! kill -0 "$CPID" 2>/dev/null; then
        echo "client died:"; cat "$PROF/client.stdout"; exit 1
    fi
    sleep 0.5
done
[ "$MOUNTED" = "1" ] || { echo "mount failed"; cat "$PROF/client.stdout"; exit 1; }
echo "mounted"

echo "=== load: write 16MB ==="
/usr/bin/time -f 'write_real=%e' dd if=/dev/zero of="$MNT/big.bin" bs=1M count=16 status=none
echo "=== load: read 16MB ==="
/usr/bin/time -f 'read_real=%e' dd if="$MNT/big.bin" of=/dev/null bs=1M status=none
echo "=== load: 120 small files ==="
mkdir -p "$MNT/many"
/usr/bin/time -f 'meta_real=%e' bash -c 'for i in $(seq 1 120); do echo "file $i" > "'"$MNT"'/many/f$i.txt"; done'
ls "$MNT/many" | wc -l
echo "=== load: rewrite 8MB + reread ==="
/usr/bin/time -f 'rewrite_real=%e' dd if=/dev/urandom of="$MNT/big.bin" bs=1M count=8 status=none
/usr/bin/time -f 'reread_real=%e' dd if="$MNT/big.bin" of=/dev/null bs=1M status=none
echo "=== load done ==="

# Unmount and let the client exit on its own so stop_perf_recorder can flush
# client.perf.data. Only SIGKILL as a last resort.
fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
for i in $(seq 1 40); do
    kill -0 "$CPID" 2>/dev/null || break
    sleep 0.25
done
if kill -0 "$CPID" 2>/dev/null; then
    kill -TERM "$CPID" 2>/dev/null || true
    sleep 2
fi
kill -KILL "$CPID" 2>/dev/null || true
wait "$CPID" 2>/dev/null || true
CPID=""

kill -INT "$S1" "$S2" "$S3" 2>/dev/null || true
for i in $(seq 1 20); do
    alive=0
    for pid in "$S1" "$S2" "$S3"; do
        kill -0 "$pid" 2>/dev/null && alive=1
    done
    [ "$alive" = "0" ] && break
    sleep 0.5
done
kill -KILL "$S1" "$S2" "$S3" 2>/dev/null || true
wait "$S1" 2>/dev/null || true
wait "$S2" 2>/dev/null || true
wait "$S3" 2>/dev/null || true
S1=""; S2=""; S3=""

sleep 1
cp -f "$LOCAL/server.perf.data" "$PROF/server.perf.data" 2>/dev/null || true
cp -f "$LOCAL/client.perf.data" "$PROF/client.perf.data" 2>/dev/null || true
ls -la "$PROF"

perf report --stdio --no-children --percent-limit 0.5 -i "$PROF/client.perf.data" \
    > "$PROF/client.report.txt" 2>"$PROF/client.report.err" || echo "client report failed"
perf report --stdio --no-children --percent-limit 0.5 -i "$PROF/server.perf.data" \
    > "$PROF/server.report.txt" 2>"$PROF/server.report.err" || echo "server report failed"
perf report --stdio --percent-limit 1 -i "$PROF/client.perf.data" \
    > "$PROF/client.callers.txt" 2>/dev/null || true
perf report --stdio --percent-limit 1 -i "$PROF/server.perf.data" \
    > "$PROF/server.callers.txt" 2>/dev/null || true

ln -sfn "$PROF" "$SHARED/profile/latest"

echo "=== timings (from above) ==="
echo "=== client report (top 40) ==="
grep -E '^\s+[0-9]+\.[0-9]+%|Overhead' "$PROF/client.report.txt" 2>/dev/null | head -40 || true
echo "=== server report (top 40) ==="
grep -E '^\s+[0-9]+\.[0-9]+%|Overhead' "$PROF/server.report.txt" 2>/dev/null | head -40 || true

echo "Profile data and reports are in $PROF"
