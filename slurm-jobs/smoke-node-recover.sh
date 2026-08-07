#!/bin/bash
#SBATCH --job-name=efs-recover
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:10:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/recover-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/recover-%j.err
#
# Long-lived FUSE client: kill s2 mid-write, bring it back, wait for the
# client down-cooldown, write again, and confirm s2 receives new data.

set -euo pipefail

REPO=/home/erbmi1/git/efs
SHARED=/orcd/scratch/orcd/001/erbmi1/efs
SCRATCH=/scratch/efs-testing/${SLURM_JOB_ID}
IP=127.0.0.1
P1=19511
P2=19512
P3=19513
MNT="$SCRATCH/mnt"
EXPORT=recover1
# Must exceed EFS_NODE_DOWN_MS (10s) in node_cache.c
COOLDOWN_WAIT=12

mkdir -p "$SHARED/logs" "$SCRATCH"/{s1,s2,s3}/log "$MNT"
cd "$REPO"

cleanup() {
    set +e
    if [ -f "$SCRATCH/fuse.log" ]; then
        cp -f "$SCRATCH/fuse.log" "$SHARED/logs/recover-${SLURM_JOB_ID}-fuse.log" 2>/dev/null || true
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
make -j"$(nproc)" efsd efs-fuse efs-mgmt

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

echo "=== mount FUSE (long-lived client) ==="
./efs-fuse "$IP:$P1" "$EXPORT" "$MNT" -f >"$SCRATCH/fuse.log" 2>&1 &
echo $! >"$SCRATCH/fuse.pid"
for _ in $(seq 1 50); do
    mountpoint -q "$MNT" 2>/dev/null && break
    sleep 0.1
done
mountpoint -q "$MNT"

node_used_bytes() {
    # Parse "node N: ... used=<human>" from efs-mgmt status into bytes.
    local nid="$1"
    ./efs-mgmt status "$IP:$P1" | python3 -c '
import sys, re
nid = sys.argv[1]
t = sys.stdin.read()
m = re.search(r"node %s:.*?used=([0-9.]+)\s*([KMGTP]?i?B)" % re.escape(nid), t, re.S)
if not m:
    print(0); raise SystemExit
val = float(m.group(1)); unit = m.group(2)
mult = {"B":1,"KiB":1024,"KB":1000,"MiB":1024**2,"MB":1000**2,
        "GiB":1024**3,"GB":1000**3}.get(unit, 1)
print(int(val * mult))
' "$nid"
}

echo "=== phase0: write while healthy ==="
dd if=/dev/urandom of="$MNT/a.bin" bs=1M count=4 status=none
sync "$MNT/a.bin"
S2_0=$(node_used_bytes 2)
echo "s2_used_after_a=$S2_0"
test "$S2_0" -gt 0

echo "=== phase1: kill s2, write while down ==="
kill -9 "$(cat "$SCRATCH/s2/log/efsd.pid")"
rm -f "$SCRATCH/s2/log/efsd.pid"
echo "s2 killed"
sleep 0.2
dd if=/dev/urandom of="$MNT/b.bin" bs=1M count=8 status=none
sync "$MNT/b.bin"
test -f "$MNT/b.bin"
echo "wrote b.bin under 2/3 quorum"

echo "=== phase2: restart s2, wait cooldown (${COOLDOWN_WAIT}s), write again ==="
./efsd --node-id 2 --addr "$IP" --port "$P2" --storage "$SCRATCH/s2" --writers 4 \
    --join "$IP:$P1" >"$SCRATCH/s2b.log" 2>&1 &
echo $! >"$SCRATCH/s2/log/efsd.pid"
sleep "$COOLDOWN_WAIT"
./efs-mgmt status "$IP:$P1" | tee "$SCRATCH/status1.txt"
grep -q 'Cluster state: OK' "$SCRATCH/status1.txt"

S2_1=$(node_used_bytes 2)
echo "s2_used_before_c=$S2_1"

dd if=/dev/urandom of="$MNT/c.bin" bs=1M count=8 status=none
sync "$MNT/c.bin"
test -f "$MNT/c.bin"

S2_2=$(node_used_bytes 2)
echo "s2_used_after_c=$S2_2"

python3 - "$S2_1" "$S2_2" <<'PY'
import sys
before, after = int(sys.argv[1]), int(sys.argv[2])
print(f"s2 delta={after - before} (before={before} after={after})")
# After cooldown the long-lived FUSE client must place new fragments on s2.
assert after > before, (before, after)
assert after - before >= 1 * 1024 * 1024, (before, after)
print("NODE_RECOVER_OK")
PY

echo "NODE_RECOVER_SMOKE_OK"
