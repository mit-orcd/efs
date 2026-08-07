#!/bin/bash
#SBATCH --job-name=efs-usage-cache
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=4G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/usage-cache-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/usage-cache-%j.err
#
# Smoke: cached usage.bin + fast status + no double-charge on overwrite.

set -euo pipefail

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
SCRATCH="/scratch/efs-testing/${SLURM_JOB_ID}"
IP=127.0.0.1
P1=19101
P2=19102
P3=19103

mkdir -p "$SHARED/logs" "$SCRATCH"/{s1,s2,s3}/log
cd "$REPO"

cleanup() {
    set +e
    for p in "$SCRATCH"/s*/log/efsd.pid; do
        [ -f "$p" ] && kill "$(cat "$p")" 2>/dev/null || true
    done
    sleep 1
    pkill -f "efsd --node-id .*--storage $SCRATCH" 2>/dev/null || true
    rm -rf "$SCRATCH"
}
trap cleanup EXIT

echo "=== build ==="
make -j"$(nproc)"

echo "=== start 3 servers ==="
./efsd --node-id 1 --addr "$IP" --port "$P1" --storage "$SCRATCH/s1" --writers 2 \
    >"$SCRATCH/s1.log" 2>&1 &
echo $! >"$SCRATCH/s1/log/efsd.pid"
sleep 1
./efsd --node-id 2 --addr "$IP" --port "$P2" --storage "$SCRATCH/s2" --writers 2 \
    --join "$IP:$P1" >"$SCRATCH/s2.log" 2>&1 &
echo $! >"$SCRATCH/s2/log/efsd.pid"
./efsd --node-id 3 --addr "$IP" --port "$P3" --storage "$SCRATCH/s3" --writers 2 \
    --join "$IP:$P1" >"$SCRATCH/s3.log" 2>&1 &
echo $! >"$SCRATCH/s3/log/efsd.pid"
sleep 2

./efs-mgmt status "$IP:$P1" | tee "$SCRATCH/status0.txt"
grep -q 'Cluster state: OK' "$SCRATCH/status0.txt"

echo "=== store-bench 64M ==="
./efs-bench "$IP:$P1" --size 64M | tee "$SCRATCH/bench1.txt"

echo "=== status after write (timed) ==="
/usr/bin/time -f 'STATUS1_REAL=%e' -o "$SCRATCH/time1.txt" \
    ./efs-mgmt status "$IP:$P1" | tee "$SCRATCH/status1.txt"
cat "$SCRATCH/time1.txt"
# 64 MiB logical → 512 chunks × 64 KiB frag/node ≈ 32 MiB used per node.
python3 - <<'PY' "$SCRATCH/status1.txt" "$SCRATCH/time1.txt"
import re, sys
text = open(sys.argv[1]).read()
m = re.search(r'node 1:.*?used=([0-9.]+)\s*([KMGT]i?B)', text)
if not m:
    m = re.search(r'used=([0-9.]+)\s*([KMGT]i?B)', text)
assert m, "no used= in status"
val, unit = float(m.group(1)), m.group(2)
mult = {'B':1,'KiB':1024,'MiB':1024**2,'GiB':1024**3}.get(unit, 1024**2)
bytes_used = val * mult
print(f"parsed_used_bytes={bytes_used:.0f}")
# Expect ~32 MiB fragment charge per node for 64 MiB logical store-bench.
assert 28 * 1024**2 <= bytes_used <= 40 * 1024**2, bytes_used
t = open(sys.argv[2]).read()
m = re.search(r'STATUS1_REAL=([0-9.]+)', t)
assert m, t
elapsed = float(m.group(1))
print(f"status_elapsed_s={elapsed}")
assert elapsed < 2.0, f"status too slow: {elapsed}s"
print("STATUS_FAST_OK")
PY

echo "=== overwrite same size (should not inflate used) ==="
./efs-bench "$IP:$P1" --size 64M | tee "$SCRATCH/bench2.txt"
./efs-mgmt status "$IP:$P1" | tee "$SCRATCH/status2.txt"
python3 - <<'PY' "$SCRATCH/status1.txt" "$SCRATCH/status2.txt"
import re, sys
def used_bytes(path):
    text = open(path).read()
    m = re.search(r'node 1:.*?used=([0-9.]+)\s*([KMGT]i?B)', text) or \
        re.search(r'used=([0-9.]+)\s*([KMGT]i?B)', text)
    val, unit = float(m.group(1)), m.group(2)
    mult = {'B':1,'KiB':1024,'MiB':1024**2,'GiB':1024**3}.get(unit, 1024**2)
    return val * mult
u1, u2 = used_bytes(sys.argv[1]), used_bytes(sys.argv[2])
print(f"used_before_overwrite={u1:.0f} after={u2:.0f}")
# Overwrite must not roughly double
assert u2 <= u1 * 1.15, (u1, u2)
print("NO_DOUBLE_CHARGE_OK")
PY

test -f "$SCRATCH/s1/meta/usage.bin"
echo "usage.bin present: $(ls -la "$SCRATCH/s1/meta/usage.bin")"

echo "=== restart s1; used should load from usage.bin quickly ==="
kill "$(cat "$SCRATCH/s1/log/efsd.pid")"
sleep 1
# wait until port free
for _ in $(seq 1 30); do
    if ! timeout 1 bash -c "echo >/dev/tcp/$IP/$P1" 2>/dev/null; then
        break
    fi
    sleep 0.2
done
./efsd --node-id 1 --addr "$IP" --port "$P1" --storage "$SCRATCH/s1" --writers 2 \
    >"$SCRATCH/s1b.log" 2>&1 &
echo $! >"$SCRATCH/s1/log/efsd.pid"
sleep 1
/usr/bin/time -f 'STATUS_RESTART_REAL=%e' -o "$SCRATCH/time_restart.txt" \
    ./efs-mgmt status "$IP:$P1" | tee "$SCRATCH/status_restart.txt"
cat "$SCRATCH/time_restart.txt"
python3 - <<'PY' "$SCRATCH/status2.txt" "$SCRATCH/status_restart.txt" "$SCRATCH/time_restart.txt"
import re, sys
def used_bytes(path):
    text = open(path).read()
    m = re.search(r'node 1:.*?used=([0-9.]+)\s*([KMGT]i?B)', text) or \
        re.search(r'used=([0-9.]+)\s*([KMGT]i?B)', text)
    val, unit = float(m.group(1)), m.group(2)
    mult = {'B':1,'KiB':1024,'MiB':1024**2,'GiB':1024**3}.get(unit, 1024**2)
    return val * mult
u2, ur = used_bytes(sys.argv[1]), used_bytes(sys.argv[2])
print(f"used_before_restart={u2:.0f} after={ur:.0f}")
assert abs(ur - u2) / max(u2, 1) < 0.05, (u2, ur)
t = open(sys.argv[3]).read()
elapsed = float(re.search(r'STATUS_RESTART_REAL=([0-9.]+)', t).group(1))
print(f"status_restart_elapsed_s={elapsed}")
assert elapsed < 2.0, elapsed
print("RESTART_USAGE_OK")
PY

echo "USAGE_CACHE_SMOKE_OK"
