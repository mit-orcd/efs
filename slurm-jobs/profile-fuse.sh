#!/bin/bash
#SBATCH --job-name=efs-prof-fuse
#SBATCH --partition=mit_quicktest,mit_normal
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=8G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/profile-fuse-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/profile-fuse-%j.err
#
# Brief single-node cluster profile focused on FUSE / metadata paths
# (many small files, getattr/readdir, rsync), not bulk Blake3 streaming.

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
PROF="$SHARED/profile/fuse-${SLURM_JOB_ID}"
LOCAL="/scratch/efs-testing/${SLURM_JOB_ID}"
mkdir -p "$PROF" "$SHARED/logs" "$LOCAL"

IP="127.0.0.1"
P1=1981
P2=1982
P3=1983
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
    for f in client.perf.data server.perf.data s1.stdout s2.stdout s3.stdout client.stdout; do
        if [ -f "$LOCAL/$f" ]; then
            cp -f "$LOCAL/$f" "$PROF/$f" 2>/dev/null || true
        fi
    done
    if [ -n "${LOCAL:-}" ]; then
        echo "cleaning /scratch: $LOCAL"
        rm -rf "$LOCAL"
    fi
}
trap cleanup EXIT

fusermount -u "$MNT" 2>/dev/null || true
rm -rf "$LOCAL"
mkdir -p "$LOCAL/s1" "$LOCAL/s2" "$LOCAL/s3" "$LOCAL/src" "$MNT" "$PROF"

echo "=== fuse profile on $(hostname) ==="
echo "PROF=$PROF LOCAL=$LOCAL"

for port in "$P1" "$P2" "$P3"; do
    fuser -k "${port}/tcp" 2>/dev/null || true
done
sleep 1

# Client-only --perf for FUSE focus; servers stay quiet.
# Log to node-local scratch first (shared NFS can lag / confuse readiness checks).
"$REPO/efsd" --node-id 1 --addr "$IP" --port "$P1" --storage "$LOCAL/s1" \
    > "$LOCAL/s1.stdout" 2>&1 &
S1=$!
for i in $(seq 1 40); do
    if grep -q "listening on" "$LOCAL/s1.stdout" 2>/dev/null; then break; fi
    if ! kill -0 "$S1" 2>/dev/null; then
        echo "server 1 died:"; cat "$LOCAL/s1.stdout"; exit 1
    fi
    sleep 0.25
done
grep -q "listening on" "$LOCAL/s1.stdout" || { echo "s1 not ready"; cat "$LOCAL/s1.stdout"; exit 1; }

"$REPO/efsd" --node-id 2 --addr "$IP" --port "$P2" --storage "$LOCAL/s2" --join "$IP:$P1" \
    > "$LOCAL/s2.stdout" 2>&1 &
S2=$!
"$REPO/efsd" --node-id 3 --addr "$IP" --port "$P3" --storage "$LOCAL/s3" --join "$IP:$P1" \
    > "$LOCAL/s3.stdout" 2>&1 &
S3=$!

for port in "$P2" "$P3"; do
    READY=0
    for i in $(seq 1 40); do
        if timeout 1 bash -c "exec 3<>/dev/tcp/$IP/$port" 2>/dev/null; then READY=1; break; fi
        sleep 0.25
    done
    [ "$READY" = "1" ] || { echo "server :$port failed"; cat "$PROF"/s*.stdout; exit 1; }
done
sleep 0.5
echo "cluster up"

echo "mkfs: $("$REPO/efs-mgmt" mkfs "$IP:$P1" fuseprof 2>&1)"

EFS_PERF_PATH="$LOCAL/client.perf.data" \
    "$REPO/efs-fuse" "$IP:$P1" "$IP:$P2" "$IP:$P3" fuseprof "$MNT" -f --perf \
    > "$LOCAL/client.stdout" 2>&1 &
CPID=$!

MOUNTED=0
for i in $(seq 1 40); do
    if mountpoint -q "$MNT" 2>/dev/null; then MOUNTED=1; break; fi
    if ! kill -0 "$CPID" 2>/dev/null; then
        echo "client died:"; cat "$LOCAL/client.stdout"; exit 1
    fi
    sleep 0.25
done
[ "$MOUNTED" = "1" ] || { echo "mount failed"; cat "$LOCAL/client.stdout"; exit 1; }
echo "mounted"

# --- FUSE-centric load (small I/O + meta + getattr storms) ---
{
    echo "=== 1) tree create: 40 dirs × 25 files (~1 KiB) ==="
    /usr/bin/time -f 'create_real=%e' bash -c '
        payload=$(printf "x%.0s" {1..1000})
        for d in $(seq 1 40); do
            mkdir -p "'"$MNT"'/tree/d$d"
            for f in $(seq 1 25); do
                printf "d%s-f%s-%s\n" "$d" "$f" "$payload" > "'"$MNT"'/tree/d$d/f$f.txt"
            done
        done
    '

    echo "=== 2) getattr storm: find + stat ==="
    /usr/bin/time -f 'find_real=%e' bash -c 'find "'"$MNT"'/tree" -type f | wc -l'
    /usr/bin/time -f 'stat_real=%e' bash -c '
        find "'"$MNT"'/tree" -type f -print0 | xargs -0 -n 50 stat -c "%n %s %Y" >/dev/null
    '

    echo "=== 3) readdir / ls -R ==="
    /usr/bin/time -f 'ls_real=%e' bash -c 'ls -R "'"$MNT"'/tree" >/dev/null'

    echo "=== 4) chmod / touch (utimens) batch ==="
    /usr/bin/time -f 'chmod_real=%e' bash -c '
        find "'"$MNT"'/tree" -type f | head -200 | xargs -r chmod 640
        find "'"$MNT"'/tree" -type f | head -200 | xargs -r touch -d "2022-03-04 05:06:07.123456789"
    '

    echo "=== 5) rsync into mount + second pass ==="
    mkdir -p "$LOCAL/src/a/b"
    for i in $(seq 1 80); do
        dd if=/dev/urandom of="$LOCAL/src/a/f$i.bin" bs=4K count=1 status=none 2>/dev/null
    done
    printf 'ignore\n' > "$LOCAL/src/.gitignore"
    /usr/bin/time -f 'rsync1_real=%e' rsync -a "$LOCAL/src/" "$MNT/rsync-dst/"
    /usr/bin/time -f 'rsync2_real=%e' rsync -a --info=stats2 "$LOCAL/src/" "$MNT/rsync-dst/" \
        | tee "$PROF/rsync2.stats"

    echo "=== 6) modest sequential write (contrast) 4 MiB ==="
    /usr/bin/time -f 'dd_write_real=%e' dd if=/dev/zero of="$MNT/bulk.bin" bs=1M count=4 status=none
    /usr/bin/time -f 'dd_read_real=%e' dd if="$MNT/bulk.bin" of=/dev/null bs=1M status=none

    echo "=== load done ==="
} | tee "$PROF/load.txt"

# Unmount; let client flush perf.data
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
cp -f "$LOCAL/client.perf.data" "$PROF/client.perf.data" 2>/dev/null || true
ls -la "$PROF"

if [ -f "$PROF/client.perf.data" ]; then
    perf report --stdio --no-children --percent-limit 0.3 -i "$PROF/client.perf.data" \
        > "$PROF/client.report.txt" 2>"$PROF/client.report.err" || echo "client report failed"
    perf report --stdio --percent-limit 0.8 -i "$PROF/client.perf.data" \
        > "$PROF/client.callers.txt" 2>/dev/null || true
    # FUSE / our ops focused extract
    {
        echo "=== symbols matching fuse / efs_fuse / meta / lookup ==="
        grep -E 'fuse|efs_fuse|efs_client_|lookup|readdir|getattr|utimens|chmod|note_meta|replicate|blake3|hash' \
            "$PROF/client.report.txt" || true
    } | tee "$PROF/client.fuse-focus.txt"
else
    echo "ERROR: no client.perf.data"
fi

ln -sfn "$PROF" "$SHARED/profile/fuse-latest"
ln -sfn "$PROF" "$SHARED/profile/latest"

echo "=== timings ==="
grep -E '_real=' "$PROF/load.txt" || true
echo "=== client top (no-children) ==="
grep -E '^\s+[0-9]+\.[0-9]+%|Overhead' "$PROF/client.report.txt" 2>/dev/null | head -45 || true
echo "Profile: $PROF"
