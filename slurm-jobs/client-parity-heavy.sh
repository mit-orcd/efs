#!/bin/bash
#SBATCH --job-name=efs-par-cli
#SBATCH --partition=mit_normal
#SBATCH --time=03:00:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=16G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/par-cli-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/par-cli-%j.err
#
# Heavy parity client: owns a disjoint half of the shared tree.
#   CLIENT_ID=1 → big/00..09 (10×1GiB) + small/d00..d09 (500×1MiB)
#   CLIENT_ID=2 → big/10..19 (10×1GiB) + small/d10..d19 (500×1MiB)
# Writes stream from urandom (no full local staging); sha256 verified after write
# and on full readback.
#
# Env: CLIENT_ID, EXPORT_NAME, PROF_ROOT, NUM_SERVERS, GO file via PROF_ROOT/GO

set -euo pipefail
if [ -z "${EFS_STREAM_STDBUF:-}" ] && command -v stdbuf >/dev/null 2>&1; then
    export EFS_STREAM_STDBUF=1
    exec stdbuf -oL -eL bash "$0" "$@"
fi

REPO=/home/erbmi1/git/efs
SHARED=/orcd/scratch/orcd/001/erbmi1/efs
CLIENT_ID="${CLIENT_ID:?CLIENT_ID required}"
EXPORT_NAME="${EXPORT_NAME:-parityHeavy}"
NUM_SERVERS="${NUM_SERVERS:-3}"
PROF_ROOT="${PROF_ROOT:?PROF_ROOT required}"
PROF="$PROF_ROOT/client${CLIENT_ID}-${SLURM_JOB_ID}"
BIG_BYTES=$((1024 * 1024 * 1024))          # 1 GiB
SMALL_BYTES=$((1024 * 1024))               # 1 MiB
SMALL_PER_DIR=50
PARALLEL_BIG="${PARALLEL_BIG:-2}"
PARALLEL_SMALL="${PARALLEL_SMALL:-8}"

mkdir -p "$PROF" "$SHARED/logs"

if [ ! -d /scratch ] || ! mkdir -p "/scratch/efs-testing/${SLURM_JOB_ID}" 2>/dev/null; then
    echo "ERROR: client needs /scratch"
    exit 1
fi
LOCAL=/scratch/efs-testing/${SLURM_JOB_ID}
MNT=$LOCAL/mnt
HASHES=$PROF/hashes
rm -rf "$LOCAL"
mkdir -p "$MNT" "$HASHES"

# shellcheck source=lib-ib.sh
source "$REPO/slurm-jobs/lib-ib.sh"
read -r CLIENT_IB CLIENT_IP < <(efs_ib_host)
echo "=== parity-heavy client_id=$CLIENT_ID on $(hostname -s) IB=$CLIENT_IB ($CLIENT_IP) ==="

cleanup() {
    set +e
    fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
    for pid in ${CPID:-}; do
        kill -INT "$pid" 2>/dev/null || true
    done
    sleep 2
    for pid in ${CPID:-}; do
        kill -KILL "$pid" 2>/dev/null || true
    done
    echo "cleaning /scratch: $LOCAL"
    rm -rf "$LOCAL"
}
trap cleanup EXIT

if [ "$CLIENT_ID" = "1" ]; then
    BIG_START=0; BIG_END=9
    DIR_START=0; DIR_END=9
elif [ "$CLIENT_ID" = "2" ]; then
    BIG_START=10; BIG_END=19
    DIR_START=10; DIR_END=19
else
    echo "CLIENT_ID must be 1 or 2"; exit 1
fi

SERVER_ADDRS=()
for i in $(seq 1 "$NUM_SERVERS"); do
    WAITED=0
    while [ ! -s "$SHARED/state/s${i}.addr" ]; do
        sleep 2
        WAITED=$((WAITED + 2))
        [ "$WAITED" -ge 600 ] && { echo "timeout s${i}.addr"; exit 1; }
    done
    SERVER_ADDRS+=("$(cat "$SHARED/state/s${i}.addr")")
done
S1="${SERVER_ADDRS[0]}"
echo "servers: ${SERVER_ADDRS[*]}"

for addr in "${SERVER_ADDRS[@]}"; do
    host=${addr%:*}
    port=${addr#*:}
    WAITED=0
    while ! timeout 2 bash -c "exec 3<>/dev/tcp/$host/$port" 2>/dev/null; do
        sleep 2
        WAITED=$((WAITED + 2))
        [ "$WAITED" -ge 600 ] && { echo "timeout $addr"; exit 1; }
    done
done

GO="$PROF_ROOT/GO"
WAITED=0
while [ ! -f "$GO" ]; do
    sleep 2
    WAITED=$((WAITED + 2))
    [ "$WAITED" -ge 600 ] && { echo "timeout GO"; exit 1; }
done
echo "GO received $(date -Is)"

EFS_META_BATCH_OPS="${EFS_META_BATCH_OPS:-65536}" \
    "$REPO/efs-fuse" "$S1" "$EXPORT_NAME" "$MNT" -f \
    >"$PROF/fuse.stdout" 2>&1 &
CPID=$!
for _ in $(seq 1 120); do
    mountpoint -q "$MNT" 2>/dev/null && break
    kill -0 "$CPID" 2>/dev/null || { echo "fuse died"; cat "$PROF/fuse.stdout"; exit 1; }
    sleep 0.5
done
mountpoint -q "$MNT" || { echo "mount failed"; cat "$PROF/fuse.stdout"; exit 1; }

mkdir -p "$MNT/big"
for d in $(seq "$DIR_START" "$DIR_END"); do
    mkdir -p "$MNT/small/d$(printf '%02d' "$d")"
done

# Stream write nbytes of urandom to path; write sha256 hex to hashfile.
write_hashed() {
    local path=$1 nbytes=$2 hashfile=$3
    python3 - "$path" "$nbytes" "$hashfile" <<'PY'
import hashlib, os, sys
path, nbytes, hashfile = sys.argv[1], int(sys.argv[2]), sys.argv[3]
h = hashlib.sha256()
left = nbytes
bs = 4 * 1024 * 1024
with open(path, "wb") as f:
    while left:
        n = min(bs, left)
        chunk = os.urandom(n)
        f.write(chunk)
        h.update(chunk)
        left -= n
    f.flush()
    os.fsync(f.fileno())
open(hashfile, "w").write(h.hexdigest() + "\n")
print(h.hexdigest())
PY
}

# Read path and compare to expected hash; return 0 on match.
verify_hashed() {
    local path=$1 nbytes=$2 hashfile=$3
    python3 - "$path" "$nbytes" "$hashfile" <<'PY'
import hashlib, sys
path, nbytes, hashfile = sys.argv[1], int(sys.argv[2]), sys.argv[3]
expect = open(hashfile).read().strip()
h = hashlib.sha256()
left = nbytes
bs = 4 * 1024 * 1024
with open(path, "rb") as f:
    while left:
        n = min(bs, left)
        chunk = f.read(n)
        if len(chunk) != n:
            print(f"SHORT read got={len(chunk)} want={n} left_before={left}", file=sys.stderr)
            sys.exit(2)
        h.update(chunk)
        left -= n
got = h.hexdigest()
if got != expect:
    print(f"MISMATCH expect={expect} got={got}", file=sys.stderr)
    sys.exit(1)
print(got)
PY
}

FAIL=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; FAIL=1; }

echo "=== WRITE big files ${BIG_START}..${BIG_END} (${PARALLEL_BIG}-way) ==="
W_START=$(date +%s.%N)
write_big_one() {
    local i=$1
    local name
    name=$(printf 'big/%02d.bin' "$i")
    local dst="$MNT/$name"
    local hf="$HASHES/big-$(printf '%02d' "$i").sha"
    local log="$PROF/write-big-$(printf '%02d' "$i").log"
    {
        local t0 t1 elapsed
        t0=$(date +%s.%N)
        write_hashed "$dst" "$BIG_BYTES" "$hf"
        t1=$(date +%s.%N)
        elapsed=$(awk -v s="$t0" -v e="$t1" 'BEGIN{printf "%.3f", e-s}')
        echo "wrote $name wall_s=$elapsed hash=$(cat "$hf")"
    } >"$log" 2>&1
}

# Track writer PIDs only — bare `wait` would hang on the FUSE child (CPID).
BIG_PIDS=()
for i in $(seq "$BIG_START" "$BIG_END"); do
    write_big_one "$i" &
    BIG_PIDS+=($!)
    while [ "${#BIG_PIDS[@]}" -ge "$PARALLEL_BIG" ]; do
        pid=${BIG_PIDS[0]}
        BIG_PIDS=("${BIG_PIDS[@]:1}")
        wait "$pid" || FAIL=1
    done
done
for pid in "${BIG_PIDS[@]:-}"; do
    wait "$pid" || FAIL=1
done
W_BIG_END=$(date +%s.%N)
echo "big_write_wall_s=$(awk -v s="$W_START" -v e="$W_BIG_END" 'BEGIN{printf "%.3f", e-s}')"

echo "=== WRITE small files dirs ${DIR_START}..${DIR_END} (${PARALLEL_SMALL}-way) ==="
W_S_START=$(date +%s.%N)
write_small_one() {
    local d=$1 f=$2
    local dir name dst hf log
    dir=$(printf 'small/d%02d' "$d")
    name=$(printf 'f%03d.bin' "$f")
    dst="$MNT/$dir/$name"
    hf="$HASHES/small-d$(printf '%02d' "$d")-f$(printf '%03d' "$f").sha"
    log="$PROF/write-small-d$(printf '%02d' "$d")-f$(printf '%03d' "$f").log"
    {
        write_hashed "$dst" "$SMALL_BYTES" "$hf"
        echo "wrote $dir/$name hash=$(cat "$hf")"
    } >"$log" 2>&1
}

SMALL_PIDS=()
for d in $(seq "$DIR_START" "$DIR_END"); do
    for f in $(seq 0 $((SMALL_PER_DIR - 1))); do
        write_small_one "$d" "$f" &
        SMALL_PIDS+=($!)
        while [ "${#SMALL_PIDS[@]}" -ge "$PARALLEL_SMALL" ]; do
            pid=${SMALL_PIDS[0]}
            SMALL_PIDS=("${SMALL_PIDS[@]:1}")
            wait "$pid" || FAIL=1
        done
    done
done
for pid in "${SMALL_PIDS[@]:-}"; do
    wait "$pid" || FAIL=1
done
W_S_END=$(date +%s.%N)
echo "small_write_wall_s=$(awk -v s="$W_S_START" -v e="$W_S_END" 'BEGIN{printf "%.3f", e-s}')"

# Count owned objects
BIG_N=$((BIG_END - BIG_START + 1))
SMALL_N=$(( (DIR_END - DIR_START + 1) * SMALL_PER_DIR ))
HASH_N=$(find "$HASHES" -type f -name '*.sha' | wc -l)
echo "expected_hashes=$((BIG_N + SMALL_N)) got_hashes=$HASH_N"
[ "$HASH_N" -eq $((BIG_N + SMALL_N)) ] || fail "hash file count mismatch"

echo "=== VERIFY readback big ==="
V_START=$(date +%s.%N)
for i in $(seq "$BIG_START" "$BIG_END"); do
    name=$(printf 'big/%02d.bin' "$i")
    hf="$HASHES/big-$(printf '%02d' "$i").sha"
    if verify_hashed "$MNT/$name" "$BIG_BYTES" "$hf" >"$PROF/verify-big-$(printf '%02d' "$i").log" 2>&1; then
        pass "verify $name"
    else
        fail "verify $name"
        cat "$PROF/verify-big-$(printf '%02d' "$i").log" || true
    fi
done

echo "=== VERIFY readback small ==="
SMALL_FAIL=0
SMALL_OK=0
for d in $(seq "$DIR_START" "$DIR_END"); do
    for f in $(seq 0 $((SMALL_PER_DIR - 1))); do
        dir=$(printf 'small/d%02d' "$d")
        name=$(printf 'f%03d.bin' "$f")
        hf="$HASHES/small-d$(printf '%02d' "$d")-f$(printf '%03d' "$f").sha"
        log="$PROF/verify-small-d$(printf '%02d' "$d")-f$(printf '%03d' "$f").log"
        if verify_hashed "$MNT/$dir/$name" "$SMALL_BYTES" "$hf" >"$log" 2>&1; then
            SMALL_OK=$((SMALL_OK + 1))
        else
            SMALL_FAIL=$((SMALL_FAIL + 1))
            if [ "$SMALL_FAIL" -le 5 ]; then
                fail "verify $dir/$name"
                cat "$log" || true
            fi
        fi
    done
done
echo "small_verify ok=$SMALL_OK fail=$SMALL_FAIL"
if [ "$SMALL_FAIL" -eq 0 ]; then
    pass "verify all $SMALL_N small files"
else
    FAIL=1
fi

V_END=$(date +%s.%N)
echo "verify_wall_s=$(awk -v s="$V_START" -v e="$V_END" 'BEGIN{printf "%.3f", e-s}')"

# Spot-check peer namespace is visible (other client's files should appear eventually)
echo "=== namespace peek ==="
ls -la "$MNT/big" | head -30 | tee "$PROF/ls-big.txt" || true
ls "$MNT/small" | tee "$PROF/ls-small.txt" || true

{
    echo "client_id=$CLIENT_ID"
    echo "big_range=${BIG_START}-${BIG_END}"
    echo "dir_range=${DIR_START}-${DIR_END}"
    echo "fail=$FAIL"
    if [ "$FAIL" -eq 0 ]; then
        echo "CLIENT_HEAVY_OK"
    else
        echo "CLIENT_HEAVY_FAIL"
    fi
} | tee "$PROF/RESULT"

fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
kill -TERM "$CPID" 2>/dev/null || true
sleep 2
kill -KILL "$CPID" 2>/dev/null || true
wait "$CPID" 2>/dev/null || true
CPID=""

[ "$FAIL" -eq 0 ]
