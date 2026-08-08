#!/bin/bash
#SBATCH --job-name=efs-parity
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=8G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/parity-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/parity-%j.err
#
# Parity / fault-tolerance suite (smoke-scale):
#   A) multi-path stripe: write spreads across 4 disks; intact read OK
#   B) cluster 2+1: write, delete fragment tree on one node, still read
#   C) kill one server mid-FUSE write; no I/O error + checksum OK
#   D) rejoin killed node; new writes land on it again
#   E) kill one server after write; remount via remaining 2; read OK
#
set -euo pipefail

REPO=/home/erbmi1/git/efs
SHARED=/orcd/scratch/orcd/001/erbmi1/efs
SCRATCH=/scratch/efs-testing/${SLURM_JOB_ID}
OUT="$SHARED/logs/parity-${SLURM_JOB_ID}"
IP=127.0.0.1
FAIL=0
PASSN=0

pass() { echo "PASS: $*"; PASSN=$((PASSN + 1)); }
fail() { echo "FAIL: $*"; FAIL=$((FAIL + 1)); }

mkdir -p "$SHARED/logs" "$OUT" "$SCRATCH"
cd "$REPO"

cleanup() {
    set +e
    fusermount -u "$SCRATCH"/mnt* 2>/dev/null || true
    for p in "$SCRATCH"/s*/log/efsd.pid "$SCRATCH"/fuse*.pid; do
        [ -f "$p" ] && kill "$(cat "$p")" 2>/dev/null || true
    done
    sleep 1
    pkill -9 -f "$SCRATCH" 2>/dev/null || true
    echo "cleaning /scratch: $SCRATCH"
    rm -rf "$SCRATCH"
}
trap cleanup EXIT

echo "=== parity-suite on $(hostname) SCRATCH=$SCRATCH ==="
make -j"$(nproc)" efsd efs-fuse efs-mgmt tests/test_erasure 2>&1 | tail -20

echo "=== unit: erasure ==="
./tests/test_erasure && pass "test_erasure" || fail "test_erasure"

wait_listen() {
    local log=$1 pid=$2
    for _ in $(seq 1 60); do
        grep -q "listening on" "$log" 2>/dev/null && return 0
        kill -0 "$pid" 2>/dev/null || return 1
        sleep 0.25
    done
    return 1
}

mount_fuse() {
    local seed=$1 export=$2 mnt=$3 log=$4
    mkdir -p "$mnt"
    # Ensure mountpoint is empty (no nested dirs left from prior mkdirs)
    find "$mnt" -mindepth 1 -delete 2>/dev/null || true
    ./efs-fuse "$seed" "$export" "$mnt" -f >"$log" 2>&1 &
    echo $! >"${log}.pid"
    for _ in $(seq 1 50); do
        mountpoint -q "$mnt" 2>/dev/null && return 0
        sleep 0.1
    done
    if ! mountpoint -q "$mnt"; then
        echo "mount_fuse failed seed=$seed export=$export mnt=$mnt" >&2
        tail -20 "$log" >&2 || true
        return 1
    fi
}

unmount_fuse() {
    local mnt=$1 pidfile=$2
    fusermount -u "$mnt" 2>/dev/null || umount -l "$mnt" 2>/dev/null || true
    [ -f "$pidfile" ] && kill "$(cat "$pidfile")" 2>/dev/null || true
    sleep 0.3
    [ -f "$pidfile" ] && kill -9 "$(cat "$pidfile")" 2>/dev/null || true
    rm -f "$pidfile"
}

node_used() {
    local seed=$1 nid=$2
    ./efs-mgmt status "$seed" | python3 -c '
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

# ---------------------------------------------------------------------------
# A) Multi-path stripe 4-disk: write 4MiB, data on ≥2 disks, intact checksum
# ---------------------------------------------------------------------------
echo ""
echo "=== A: multi-path stripe 4-disk ==="
A="$SCRATCH/A"
mkdir -p "$A"/{d1,d2,d3,d4,s2,s3,mnt}
PA=19101; PB=19102; PC=19103
./efsd --node-id 1 --addr "$IP" --port "$PA" \
    --storage "$A/d1,$A/d2,$A/d3,$A/d4" --writers 4 --no-direct-io \
    >"$OUT/A-s1.log" 2>&1 &
echo $! >"$A/s1.pid"
wait_listen "$OUT/A-s1.log" "$(cat "$A/s1.pid")" || { fail "A s1 start"; cat "$OUT/A-s1.log"; }
grep -q "stripe=leastq" "$OUT/A-s1.log" && pass "A stripe=leastq banner" || fail "A stripe banner"
./efsd --node-id 2 --addr "$IP" --port "$PB" --storage "$A/s2" --join "$IP:$PA" \
    --writers 2 --no-direct-io >"$OUT/A-s2.log" 2>&1 &
echo $! >"$A/s2.pid"
./efsd --node-id 3 --addr "$IP" --port "$PC" --storage "$A/s3" --join "$IP:$PA" \
    --writers 2 --no-direct-io >"$OUT/A-s3.log" 2>&1 &
echo $! >"$A/s3.pid"
sleep 2
./efs-mgmt mkfs "$IP:$PA" parityA
mount_fuse "$IP:$PA" parityA "$A/mnt" "$OUT/A-fuse.log"
dd if=/dev/urandom of="$A/payload.bin" bs=1M count=4 status=none
SUM_A=$(sha256sum "$A/payload.bin" | awk '{print $1}')
timeout 30 cp "$A/payload.bin" "$A/mnt/big.bin"
sync "$A/mnt/big.bin" 2>/dev/null || true
unmount_fuse "$A/mnt" "$OUT/A-fuse.log.pid"
NONEMPTY=0
for d in "$A"/d{1,2,3,4}; do
    bytes=$(du -sb "$d/data" 2>/dev/null | awk '{print $1}')
    [ "${bytes:-0}" -gt 0 ] && NONEMPTY=$((NONEMPTY + 1))
done
[ "$NONEMPTY" -ge 2 ] && pass "A data on $NONEMPTY/4 stripe disks" \
    || fail "A stripe distribution ($NONEMPTY nonempty)"
mount_fuse "$IP:$PA" parityA "$A/mnt" "$OUT/A-fuse2.log"
SUM_A2=$(timeout 30 sha256sum "$A/mnt/big.bin" | awk '{print $1}')
if [ "$SUM_A" = "$SUM_A2" ]; then
    pass "A read 4MiB intact on striped server"
else
    fail "A checksum mismatch ($SUM_A vs $SUM_A2)"
    tail -30 "$OUT/A-fuse2.log" || true
fi
unmount_fuse "$A/mnt" "$OUT/A-fuse2.log.pid"
kill "$(cat "$A/s1.pid")" "$(cat "$A/s2.pid")" "$(cat "$A/s3.pid")" 2>/dev/null || true
sleep 1
kill -9 "$(cat "$A/s1.pid")" "$(cat "$A/s2.pid")" "$(cat "$A/s3.pid")" 2>/dev/null || true

# ---------------------------------------------------------------------------
# B) Cluster 2+1: write, rm -rf one node's data/exports, still read
# ---------------------------------------------------------------------------
echo ""
echo "=== B: cluster fragment loss on one node ==="
B="$SCRATCH/B"
mkdir -p "$B"/{s1,s2,s3}/log "$B/mnt"
P1=19201; P2=19202; P3=19203
./efsd --node-id 1 --addr "$IP" --port "$P1" --storage "$B/s1" --writers 4 \
    >"$OUT/B-s1.log" 2>&1 &
echo $! >"$B/s1/log/efsd.pid"
wait_listen "$OUT/B-s1.log" "$(cat "$B/s1/log/efsd.pid")"
./efsd --node-id 2 --addr "$IP" --port "$P2" --storage "$B/s2" --join "$IP:$P1" \
    --writers 4 >"$OUT/B-s2.log" 2>&1 &
echo $! >"$B/s2/log/efsd.pid"
./efsd --node-id 3 --addr "$IP" --port "$P3" --storage "$B/s3" --join "$IP:$P1" \
    --writers 4 >"$OUT/B-s3.log" 2>&1 &
echo $! >"$B/s3/log/efsd.pid"
sleep 2
./efs-mgmt mkfs "$IP:$P1" parityB
mount_fuse "$IP:$P1" parityB "$B/mnt" "$OUT/B-fuse.log"
dd if=/dev/urandom of="$B/payload.bin" bs=1M count=4 status=none
SUM_B=$(sha256sum "$B/payload.bin" | awk '{print $1}')
timeout 30 cp "$B/payload.bin" "$B/mnt/f.bin"
sync "$B/mnt/f.bin" 2>/dev/null || true
unmount_fuse "$B/mnt" "$OUT/B-fuse.log.pid"
# Delete all data fragments on s2 (cluster 2+1 must reconstruct from s1+s3)
echo "B: wiping s2 data/exports"
rm -rf "$B/s2/data/exports"
mkdir -p "$B/s2/data/exports"
mount_fuse "$IP:$P1" parityB "$B/mnt" "$OUT/B-fuse2.log"
SUM_B2=$(timeout 30 sha256sum "$B/mnt/f.bin" | awk '{print $1}')
if [ "$SUM_B" = "$SUM_B2" ]; then
    pass "B read 4MiB after wiping s2 fragment store"
else
    fail "B checksum mismatch after s2 wipe ($SUM_B vs $SUM_B2)"
    tail -40 "$OUT/B-fuse2.log" || true
fi
unmount_fuse "$B/mnt" "$OUT/B-fuse2.log.pid"

# ---------------------------------------------------------------------------
# C) Kill s3 mid-FUSE write; expect success + matching checksum
# ---------------------------------------------------------------------------
echo ""
echo "=== C: kill s3 mid-write ==="
# reuse B cluster (s1-s3 still up); remount
mount_fuse "$IP:$P1" parityB "$B/mnt" "$OUT/C-fuse.log"
dd if=/dev/urandom of="$B/c_payload.bin" bs=1M count=8 status=none
SUM_C=$(sha256sum "$B/c_payload.bin" | awk '{print $1}')
(
    sleep 0.15
    echo "C: killing s3 pid=$(cat "$B/s3/log/efsd.pid")"
    kill -9 "$(cat "$B/s3/log/efsd.pid")" || true
    rm -f "$B/s3/log/efsd.pid"
) &
KILLER=$!
set +e
timeout 45 cp "$B/c_payload.bin" "$B/mnt/c.bin"
CP_RC=$?
sync "$B/mnt/c.bin" 2>/dev/null
set -e
wait "$KILLER" 2>/dev/null || true
if [ "$CP_RC" -ne 0 ]; then
    fail "C cp failed during s3 kill (rc=$CP_RC)"
    grep -E 'no quorum|EIO|error|fatal' "$OUT/C-fuse.log" | tail -20 || true
else
    SUM_C2=$(sha256sum "$B/mnt/c.bin" | awk '{print $1}')
    if [ "$SUM_C" = "$SUM_C2" ]; then
        pass "C mid-kill write 8MiB ok (no I/O error, checksum match)"
    else
        fail "C checksum mismatch after mid-kill"
    fi
fi
# count hard fuse errors during C
IOERR=$(grep -cE 'Input/output error|no quorum' "$OUT/C-fuse.log" 2>/dev/null || true)
IOERR=${IOERR:-0}
echo "C: fuse error lines=$IOERR"
unmount_fuse "$B/mnt" "$OUT/C-fuse.log.pid"

# ---------------------------------------------------------------------------
# D) Rejoin s3; wait cooldown; new write must increase s3 used
# ---------------------------------------------------------------------------
echo ""
echo "=== D: rejoin s3 and place new data ==="
./efsd --node-id 3 --addr "$IP" --port "$P3" --storage "$B/s3" --join "$IP:$P1" \
    --writers 4 >"$OUT/D-s3b.log" 2>&1 &
echo $! >"$B/s3/log/efsd.pid"
sleep 2
./efs-mgmt status "$IP:$P1" | tee "$OUT/D-status0.txt"
grep -q 'Cluster state: OK' "$OUT/D-status0.txt" && pass "D cluster OK after rejoin" \
    || fail "D cluster not OK after rejoin"
mount_fuse "$IP:$P1" parityB "$B/mnt" "$OUT/D-fuse.log"
# cooldown > EFS_NODE_DOWN_MS (10s)
sleep 12
S3_BEFORE=$(node_used "$IP:$P1" 3)
echo "D: s3_used_before=$S3_BEFORE"
dd if=/dev/urandom of="$B/mnt/d.bin" bs=1M count=4 status=none
sync "$B/mnt/d.bin" 2>/dev/null || true
S3_AFTER=$(node_used "$IP:$P1" 3)
echo "D: s3_used_after=$S3_AFTER"
if [ "$S3_AFTER" -gt "$S3_BEFORE" ] && [ $((S3_AFTER - S3_BEFORE)) -ge $((256 * 1024)) ]; then
    pass "D healed placement: s3 gained $((S3_AFTER - S3_BEFORE)) bytes"
else
    fail "D s3 did not receive new fragments (before=$S3_BEFORE after=$S3_AFTER)"
    ./efs-mgmt status "$IP:$P1" || true
fi
# also verify earlier files still readable after heal
SUM_B3=$(timeout 30 sha256sum "$B/mnt/f.bin" | awk '{print $1}')
[ "$SUM_B" = "$SUM_B3" ] && pass "D prior file still readable after rejoin" \
    || fail "D prior file unreadable after rejoin"
unmount_fuse "$B/mnt" "$OUT/D-fuse.log.pid"

# ---------------------------------------------------------------------------
# E) Kill s2 after durable write; remount via s1+s3 only; read OK
# ---------------------------------------------------------------------------
echo ""
echo "=== E: kill s2 after write; remount on remaining nodes ==="
mount_fuse "$IP:$P1" parityB "$B/mnt" "$OUT/E-fuse.log"
dd if=/dev/urandom of="$B/e_payload.bin" bs=1M count=4 status=none
SUM_E=$(sha256sum "$B/e_payload.bin" | awk '{print $1}')
timeout 30 cp "$B/e_payload.bin" "$B/mnt/e.bin"
sync "$B/mnt/e.bin" 2>/dev/null || true
unmount_fuse "$B/mnt" "$OUT/E-fuse.log.pid"
kill -9 "$(cat "$B/s2/log/efsd.pid")" 2>/dev/null || true
rm -f "$B/s2/log/efsd.pid"
echo "E: s2 killed"
# Mount naming only s1 (client discovers peers; s2 is down)
mount_fuse "$IP:$P1" parityB "$B/mnt" "$OUT/E-fuse2.log"
SUM_E2=$(timeout 30 sha256sum "$B/mnt/e.bin" | awk '{print $1}')
if [ "$SUM_E" = "$SUM_E2" ]; then
    pass "E read after killing one server (2+1 quorum)"
else
    fail "E checksum mismatch with s2 down"
    tail -30 "$OUT/E-fuse2.log" || true
fi
unmount_fuse "$B/mnt" "$OUT/E-fuse2.log.pid"

# teardown B cluster
for p in "$B"/s*/log/efsd.pid; do
    [ -f "$p" ] && kill "$(cat "$p")" 2>/dev/null || true
done
sleep 1

echo ""
echo "=== SUMMARY pass=$PASSN fail=$FAIL ==="
if [ "$FAIL" -eq 0 ]; then
    echo "PARITY_SUITE_OK"
    echo "PARITY_SUITE_OK" >"$OUT/RESULT"
    exit 0
fi
echo "PARITY_SUITE_FAIL"
echo "PARITY_SUITE_FAIL" >"$OUT/RESULT"
exit 1
