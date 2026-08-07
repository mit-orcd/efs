#!/bin/bash
#SBATCH --job-name=efs-kill1
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/kill1-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/kill1-%j.err
#
# Kill s2 mid store-bench / FUSE write; expect 2+1 quorum to finish cleanly.

set -euo pipefail

REPO=/home/erbmi1/git/efs
SHARED=/orcd/scratch/orcd/001/erbmi1/efs
SCRATCH=/scratch/efs-testing/${SLURM_JOB_ID}
IP=127.0.0.1
P1=19301
P2=19302
P3=19303
MNT="$SCRATCH/mnt"
EXPORT=kill1

mkdir -p "$SHARED/logs" "$SCRATCH"/{s1,s2,s3}/log "$MNT"
cd "$REPO"

cleanup() {
    set +e
    if [ -f "$SCRATCH/fuse.log" ]; then
        cp -f "$SCRATCH/fuse.log" "$SHARED/logs/kill1-${SLURM_JOB_ID}-fuse.log" 2>/dev/null || true
    fi
    if mountpoint -q "$MNT" 2>/dev/null; then
        fusermount -u "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
    fi
    for p in "$SCRATCH"/s*/log/efsd.pid "$SCRATCH/fuse.pid"; do
        [ -f "$p" ] && kill "$(cat "$p")" 2>/dev/null || true
    done
    sleep 1
    rm -rf "$SCRATCH"
}
trap cleanup EXIT

echo "=== build ==="
make -j"$(nproc)"

echo "=== start 3 servers ==="
./efsd --node-id 1 --addr "$IP" --port "$P1" --storage "$SCRATCH/s1" --writers 4 \
    >"$SCRATCH/s1.log" 2>&1 &
echo $! >"$SCRATCH/s1/log/efsd.pid"
sleep 1
./efsd --node-id 2 --addr "$IP" --port "$P2" --storage "$SCRATCH/s2" --writers 4 \
    --join "$IP:$P1" >"$SCRATCH/s2.log" 2>&1 &
echo $! >"$SCRATCH/s2/log/efsd.pid"
./efsd --node-id 3 --addr "$IP" --port "$P3" --storage "$SCRATCH/s3" --writers 4 \
    --join "$IP:$P1" >"$SCRATCH/s3.log" 2>&1 &
echo $! >"$SCRATCH/s3/log/efsd.pid"
sleep 2

./efs-mgmt status "$IP:$P1" | tee "$SCRATCH/status0.txt"
grep -q 'Cluster state: OK' "$SCRATCH/status0.txt"
./efs-mgmt mkfs "$IP:$P1" "$EXPORT"

echo "=== store-bench with mid-kill of s2 ==="
(
    sleep 0.3
    echo "killing s2 pid=$(cat "$SCRATCH/s2/log/efsd.pid")"
    kill -9 "$(cat "$SCRATCH/s2/log/efsd.pid")" || true
    rm -f "$SCRATCH/s2/log/efsd.pid"
) &
KILLER=$!

set +e
./efs-bench "$IP:$P1" --size 64M >"$SCRATCH/bench.txt" 2>&1
BENCH_RC=$?
set -e
wait "$KILLER" 2>/dev/null || true
cat "$SCRATCH/bench.txt"
# Quorum path: allow a few in-flight fails at the kill instant, but most chunks OK.
python3 - "$SCRATCH/bench.txt" "$BENCH_RC" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
rc = int(sys.argv[2])
m = re.search(r'chunks_ok=(\d+) chunks_fail=(\d+)', text)
assert m and 'BENCH_OK' in text, text
ok, fail = int(m.group(1)), int(m.group(2))
print(f"bench ok={ok} fail={fail} rc={rc}")
assert ok >= 400, (ok, fail)          # of 512
assert fail <= ok // 4, (ok, fail)    # kill blip only
print("STORE_BENCH_KILL_OK")
PY

echo "=== FUSE write with mid-kill of s3 ==="
# Restart s2 so cluster is healthy again before FUSE test
./efsd --node-id 2 --addr "$IP" --port "$P2" --storage "$SCRATCH/s2" --writers 4 \
    --join "$IP:$P1" >"$SCRATCH/s2b.log" 2>&1 &
echo $! >"$SCRATCH/s2/log/efsd.pid"
sleep 2

./efs-fuse "$IP:$P1" "$EXPORT" "$MNT" -f >"$SCRATCH/fuse.log" 2>&1 &
echo $! >"$SCRATCH/fuse.pid"
for _ in $(seq 1 50); do
    if mountpoint -q "$MNT" 2>/dev/null; then
        break
    fi
    sleep 0.1
done
mountpoint -q "$MNT"

# Local file then mv onto mount (same pattern as kill harnesses)
dd if=/dev/urandom of="$SCRATCH/payload.bin" bs=1M count=32 status=none
SUM=$(sha256sum "$SCRATCH/payload.bin" | awk '{print $1}')

(
    sleep 0.2
    echo "killing s3"
    kill -9 "$(cat "$SCRATCH/s3/log/efsd.pid")" || true
    rm -f "$SCRATCH/s3/log/efsd.pid"
) &
KILLER2=$!

set +e
timeout 60 cp "$SCRATCH/payload.bin" "$MNT/payload.bin"
CP_RC=$?
sync "$MNT/payload.bin" 2>/dev/null
set -e
wait "$KILLER2" 2>/dev/null || true

if [ "$CP_RC" -ne 0 ]; then
    echo "FAIL: cp onto FUSE failed during s3 kill (rc=$CP_RC)"
    tail -40 "$SCRATCH/fuse.log" || true
    exit 1
fi

SUM2=$(sha256sum "$MNT/payload.bin" | awk '{print $1}')
echo "sum_orig=$SUM sum_read=$SUM2"
if [ "$SUM" != "$SUM2" ]; then
    echo "FAIL: checksum mismatch after kill"
    exit 1
fi

echo "FUSE_KILL_OK"
echo "KILL_ONE_SMOKE_OK"
