#!/bin/bash
#SBATCH --job-name=efs-chaos-ddmix
#SBATCH --partition=mit_normal
#SBATCH --time=01:30:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=16
#SBATCH --mem=32G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/chaos-ddmix-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/chaos-ddmix-%j.err
#
# 5 rounds: large + small dd in parallel on a 3-server local cluster.
# Each round injects a fault on one node and repairs (restart/join/heal).
#
# Per round parallel load (defaults, override via env):
#   SMALL: 8 workers × 80 files × 64 KiB
#   LARGE: 2 workers × 512 MiB
# Chaos rotation: wipe-meta | corrupt-meta | kill-downtime | wipe-root+frags

set -euo pipefail
if [ -z "${EFS_STREAM_STDBUF:-}" ] && command -v stdbuf >/dev/null 2>&1; then
    export EFS_STREAM_STDBUF=1
    exec stdbuf -oL -eL bash "$0" "$@"
fi

REPO="/home/erbmi1/git/efs"
SHARED="/orcd/scratch/orcd/001/erbmi1/efs"
OUT="$SHARED/logs/chaos-ddmix-${SLURM_JOB_ID}"
LOCAL="/scratch/efs-testing/${SLURM_JOB_ID}"
mkdir -p "$OUT" "$SHARED/logs"

IP="127.0.0.1"
P1=1971
P2=1972
P3=1973
MNT="$LOCAL/mnt"
EXPORT="chaosmix"
META_SHARD="0922/3372/0368/5477/5810"
ROUNDS="${ROUNDS:-5}"
SMALL_WORKERS="${SMALL_WORKERS:-8}"
SMALL_FILES="${SMALL_FILES:-80}"
SMALL_BS="${SMALL_BS:-65536}"          # 64 KiB
LARGE_WORKERS="${LARGE_WORKERS:-2}"
LARGE_MIB="${LARGE_MIB:-512}"          # 512 MiB each
FAIL=0

pass() { echo "PASS: $*"; echo "PASS: $*" >>"$OUT/REVIEW.txt"; }
fail() { echo "FAIL: $*"; echo "FAIL: $*" >>"$OUT/REVIEW.txt"; FAIL=1; }
note() { echo "NOTE: $*"; echo "NOTE: $*" >>"$OUT/REVIEW.txt"; }

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
    if [ -n "${LOCAL:-}" ]; then
        echo "cleaning /scratch: $LOCAL"
        rm -rf "$LOCAL"
    fi
}
trap cleanup EXIT

wait_port() {
    local port=$1
    for _ in $(seq 1 60); do
        timeout 1 bash -c "exec 3<>/dev/tcp/$IP/$port" 2>/dev/null && return 0
        sleep 0.25
    done
    return 1
}

count_meta_frags() {
    find "$1/data/exports" -type f ! -name '*.sum' 2>/dev/null | grep -c "$META_SHARD" || true
}

wipe_meta_frags() {
    find "$1/data/exports" -type f 2>/dev/null | grep "$META_SHARD" \
        | while read -r f; do rm -f "$f"; done
}

corrupt_meta_frags() {
    find "$1/data/exports" -type f ! -name '*.sum' 2>/dev/null | grep "$META_SHARD" \
        | while read -r f; do
            dd if=/dev/urandom of="$f" bs=4096 count=2 conv=notrunc status=none 2>/dev/null || true
        done
}

start_server() {
    local id=$1 port=$2 stor=$3 log=$4
    local join=()
    if [ "$id" -gt 1 ]; then
        join=(--join "$IP:$P1")
    fi
    : >"$log"
    ./efsd --node-id "$id" --addr "$IP" --port "$port" --storage "$stor" \
        --writers 8 --no-direct-io "${join[@]}" >"$log" 2>&1 &
    echo $!
}

stop_pid() {
    local pid=${1:-}
    [ -n "$pid" ] || return 0
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

mount_fuse() {
    local log=$1
    EFS_META_BATCH_OPS="${EFS_META_BATCH_OPS:-65536}" \
        ./efs-fuse "$IP:$P1" "$IP:$P2" "$IP:$P3" "$EXPORT" "$MNT" -f \
        >"$log" 2>&1 &
    CPID=$!
    for _ in $(seq 1 80); do
        mountpoint -q "$MNT" && return 0
        kill -0 "$CPID" 2>/dev/null || { cat "$log"; return 1; }
        sleep 0.25
    done
    return 1
}

unmount_fuse() {
    fusermount -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
    if [ -n "${CPID:-}" ]; then
        kill -TERM "$CPID" 2>/dev/null || true
        sleep 1
        kill -KILL "$CPID" 2>/dev/null || true
        wait "$CPID" 2>/dev/null || true
        CPID=
    fi
    sleep 0.3
}

# Parallel large + small. Sets ROUND_WALL, ROUND_OK (0/1).
run_parallel_mix() {
    local round=$1
    local dir="$MNT/r${round}"
    mkdir -p "$dir/small" "$dir/large"
    local pids=()
    local t0 t1
    t0=$(date +%s.%N)

    local w
    for w in $(seq 0 $((SMALL_WORKERS - 1))); do
        (
            set -euo pipefail
            d="$dir/small/w$(printf '%02d' "$w")"
            mkdir -p "$d"
            for i in $(seq -w 0 $((SMALL_FILES - 1))); do
                dd if=/dev/zero of="$d/f$i" bs="$SMALL_BS" count=1 status=none
            done
            sync "$d" 2>/dev/null || true
        ) &
        pids+=($!)
    done
    for w in $(seq 0 $((LARGE_WORKERS - 1))); do
        (
            set -euo pipefail
            dd if=/dev/zero of="$dir/large/w${w}.bin" bs=1M count="$LARGE_MIB" \
                status=none conv=fsync
        ) &
        pids+=($!)
    done

    ROUND_OK=1
    for pid in "${pids[@]}"; do
        if ! wait "$pid"; then
            ROUND_OK=0
        fi
    done
    sync "$dir" 2>/dev/null || true
    t1=$(date +%s.%N)
    ROUND_WALL=$(awk -v s="$t0" -v e="$t1" 'BEGIN{printf "%.3f", e-s}')
}

verify_round() {
    local round=$1
    local dir="$MNT/r${round}"
    local bad=0
    local w
    for w in $(seq 0 $((LARGE_WORKERS - 1))); do
        local sz
        sz=$(stat -c %s "$dir/large/w${w}.bin" 2>/dev/null || echo 0)
        local want=$((LARGE_MIB * 1024 * 1024))
        if [ "$sz" != "$want" ]; then
            note "r$round large w$w size=$sz want=$want"
            bad=1
        fi
    done
    # Spot-check a few small files (seq -w width matches max index digits)
    local sample
    sample=$(seq -w 0 $((SMALL_FILES - 1)) | head -1)
    for w in 0 $((SMALL_WORKERS / 2)); do
        local f="$dir/small/w$(printf '%02d' "$w")/f${sample}"
        local sz
        sz=$(stat -c %s "$f" 2>/dev/null || echo 0)
        if [ "$sz" != "$SMALL_BS" ]; then
            note "r$round small missing/bad $f sz=$sz (ls: $(ls "$dir/small/w$(printf '%02d' "$w")" 2>/dev/null | head -3 | tr '\n' ' '))"
            bad=1
        fi
    done
    # Prior rounds still readable
    if [ "$round" -gt 1 ]; then
        local prev=$((round - 1))
        local psz
        psz=$(stat -c %s "$MNT/r${prev}/large/w0.bin" 2>/dev/null || echo 0)
        if [ "$psz" != "$((LARGE_MIB * 1024 * 1024))" ]; then
            note "r$round prior r$prev large unreadable sz=$psz"
            bad=1
        fi
    fi
    return "$bad"
}

repair_victim() {
    local vid=$1
    local port stor log pidvar
    case "$vid" in
        1) port=$P1; stor="$LOCAL/s1"; log="$OUT/s1.stdout"; pidvar=S1 ;;
        2) port=$P2; stor="$LOCAL/s2"; log="$OUT/s2.stdout"; pidvar=S2 ;;
        3) port=$P3; stor="$LOCAL/s3"; log="$OUT/s3.stdout"; pidvar=S3 ;;
        *) return 1 ;;
    esac
    local old
    old=${!pidvar:-}
    stop_pid "$old"
    # Always rejoin via s1 (if victim is s1, start s1 first without join)
    if [ "$vid" = "1" ]; then
        # Restart seed; peers still up — they keep cluster. s1 rejoins via s2.
        S1=$(start_server 1 "$P1" "$stor" "$log")
        # Actually node-id 1 with empty join starts standalone; force join to s2
        stop_pid "$S1"
        : >"$log"
        ./efsd --node-id 1 --addr "$IP" --port "$P1" --storage "$stor" \
            --writers 8 --no-direct-io --join "$IP:$P2" >"$log" 2>&1 &
        S1=$!
    else
        eval "$pidvar=\$(start_server $vid $port \"$stor\" \"$log\")"
    fi
    if ! wait_port "$port"; then
        note "repair: s$vid failed to listen"
        tail -30 "$log" || true
        return 1
    fi
    # Wait for heal / catchup / listening
    local i
    for i in $(seq 1 40); do
        if grep -qE 'meta-heal:|meta-catchup: rebuilt|listening on' "$log" 2>/dev/null; then
            break
        fi
        sleep 0.5
    done
    if timeout 15 ./efs-query "$IP:$port" >"$OUT/query-s${vid}-r${ROUND}.txt" 2>&1; then
        note "repair: efs-query s$vid OK"
        return 0
    fi
    note "repair: efs-query s$vid failed; retry join once"
    stop_pid "${!pidvar}"
    if [ "$vid" = "1" ]; then
        ./efsd --node-id 1 --addr "$IP" --port "$P1" --storage "$stor" \
            --writers 8 --no-direct-io --join "$IP:$P2" >"$log" 2>&1 &
        S1=$!
    else
        eval "$pidvar=\$(start_server $vid $port \"$stor\" \"$log\")"
    fi
    wait_port "$port" || return 1
    timeout 15 ./efs-query "$IP:$port" >"$OUT/query-s${vid}-r${ROUND}-retry.txt" 2>&1
}

inject_fault() {
    local round=$1
    # Rotate victim 2,3,2,3,1 across rounds (prefer non-seed)
    local modes=(wipe_frags corrupt_frags kill_downtime wipe_all kill_downtime)
    local victims=(2 3 2 3 1)
    local mode="${modes[$((round - 1))]}"
    local vid="${victims[$((round - 1))]}"
    VICTIM=$vid
    FAULT=$mode
    local stor="$LOCAL/s${vid}"
    local port
    case "$vid" in
        1) port=$P1 ;;
        2) port=$P2 ;;
        3) port=$P3 ;;
    esac
    note "r$round FAULT=$mode victim=s$vid"

    case "$mode" in
        wipe_frags)
            # Live wipe while load runs — frags disappear mid-write
            wipe_meta_frags "$stor"
            note "r$round wiped live meta frags on s$vid (count=$(count_meta_frags "$stor"))"
            ;;
        corrupt_frags)
            corrupt_meta_frags "$stor"
            note "r$round corrupted live meta frags on s$vid"
            ;;
        kill_downtime)
            case "$vid" in
                1) stop_pid "$S1"; S1= ;;
                2) stop_pid "$S2"; S2= ;;
                3) stop_pid "$S3"; S3= ;;
            esac
            note "r$round killed s$vid for downtime during load"
            sleep 2
            ;;
        wipe_all)
            case "$vid" in
                1) stop_pid "$S1"; S1= ;;
                2) stop_pid "$S2"; S2= ;;
                3) stop_pid "$S3"; S3= ;;
            esac
            wipe_meta_frags "$stor"
            rm -f "$stor/meta/exports/$EXPORT/metadata.bin"
            note "r$round stopped s$vid, wiped meta root+frags"
            ;;
    esac
}

rm -rf "$LOCAL"
mkdir -p "$LOCAL/s1" "$LOCAL/s2" "$LOCAL/s3" "$MNT" "$OUT"
: >"$OUT/REVIEW.txt"
cd "$REPO"

echo "chaos-dd-mix-5 on $(hostname) LOCAL=$LOCAL rounds=$ROUNDS" | tee -a "$OUT/REVIEW.txt"
note "load: ${SMALL_WORKERS}x${SMALL_FILES}x$((SMALL_BS/1024))KiB + ${LARGE_WORKERS}x${LARGE_MIB}MiB parallel"

for port in "$P1" "$P2" "$P3"; do
    fuser -k "${port}/tcp" 2>/dev/null || true
done
sleep 1

echo "=== build ==="
make -j"$(nproc)" efsd efs-fuse efs-mgmt efs-query

echo "=== start cluster ==="
S1=$(start_server 1 "$P1" "$LOCAL/s1" "$OUT/s1.stdout")
wait_port "$P1" || { cat "$OUT/s1.stdout"; exit 1; }
S2=$(start_server 2 "$P2" "$LOCAL/s2" "$OUT/s2.stdout")
S3=$(start_server 3 "$P3" "$LOCAL/s3" "$OUT/s3.stdout")
wait_port "$P2" && wait_port "$P3" || { cat "$OUT"/s*.stdout; exit 1; }
sleep 1
./efs-mgmt mkfs "$IP:$P1" "$EXPORT"
./efs-mgmt status "$IP:$P1" | tee "$OUT/status0.txt" | tail -20

BYTES_ROUND=$(( SMALL_WORKERS * SMALL_FILES * SMALL_BS + LARGE_WORKERS * LARGE_MIB * 1024 * 1024 ))
note "bytes/round≈$BYTES_ROUND"

for ROUND in $(seq 1 "$ROUNDS"); do
    echo "======== ROUND $ROUND / $ROUNDS ========" | tee -a "$OUT/REVIEW.txt"
    mount_fuse "$OUT/fuse-r${ROUND}.stdout" || {
        fail "r$ROUND mount failed"
        # Repair all servers and retry mount once
        for v in 1 2 3; do repair_victim "$v" || true; done
        mount_fuse "$OUT/fuse-r${ROUND}-retry.stdout" || {
            fail "r$ROUND remount failed; aborting remaining rounds"
            break
        }
    }

    # Start load in background so we can inject fault mid-flight
    MIX_STATUS="$OUT/mix-r${ROUND}.status"
    rm -f "$MIX_STATUS"
    (
        set +e
        run_parallel_mix "$ROUND"
        {
            echo "MIX_OK=$ROUND_OK"
            echo "MIX_WALL=$ROUND_WALL"
        } >"$MIX_STATUS"
        # exit 0 even if some workers failed — status file carries MIX_OK
        exit 0
    ) &
    MIX_PID=$!

    # Let writers get going, then break something
    sleep 1
    inject_fault "$ROUND"

    # Wait for mix to finish (may fail if victim was critical mid-write)
    set +e
    wait "$MIX_PID"
    MIX_RC=$?
    set -e
    MIX_OK=0
    MIX_WALL=0
    if [ -f "$MIX_STATUS" ]; then
        # shellcheck disable=SC1090
        source "$MIX_STATUS"
        note "r$ROUND mix status: MIX_OK=$MIX_OK MIX_WALL=$MIX_WALL"
    else
        note "r$ROUND mix status missing rc=$MIX_RC"
    fi

    # Repair victim (and ensure cluster OK)
    if [ "${FAULT:-}" = "wipe_frags" ] || [ "${FAULT:-}" = "corrupt_frags" ]; then
        # Live damage: force catchup via query + optional restart if frags still gone
        timeout 15 ./efs-query "$IP:$P1" >"$OUT/query-r${ROUND}-s1.txt" 2>&1 || true
        timeout 15 ./efs-query "$IP:$P2" >"$OUT/query-r${ROUND}-s2.txt" 2>&1 || true
        timeout 15 ./efs-query "$IP:$P3" >"$OUT/query-r${ROUND}-s3.txt" 2>&1 || true
        local_frags=$(count_meta_frags "$LOCAL/s${VICTIM}")
        if [ "${local_frags:-0}" -lt 1 ]; then
            note "r$ROUND live frags still 0 on s$VICTIM → restart+heal"
            repair_victim "$VICTIM" || fail "r$ROUND repair s$VICTIM"
        else
            note "r$ROUND s$VICTIM meta frags=$local_frags (filled by PUT or heal)"
            # Still bounce victim once to exercise rejoin heal path
            repair_victim "$VICTIM" || fail "r$ROUND bounce-repair s$VICTIM"
        fi
    else
        repair_victim "$VICTIM" || fail "r$ROUND repair s$VICTIM"
    fi

    # Remount if fuse died during chaos
    if ! mountpoint -q "$MNT" 2>/dev/null || ! kill -0 "${CPID:-}" 2>/dev/null; then
        note "r$ROUND fuse dead after fault; remounting"
        unmount_fuse
        mount_fuse "$OUT/fuse-r${ROUND}-after.stdout" || {
            fail "r$ROUND remount after repair failed"
            continue
        }
    fi

    if verify_round "$ROUND"; then
        pass "r$ROUND verify OK wall=${MIX_WALL:-?}s fault=$FAULT victim=s$VICTIM"
    else
        # One more hard repair cycle then recheck
        note "r$ROUND verify failed; full repair cycle"
        unmount_fuse
        for v in 1 2 3; do repair_victim "$v" || true; done
        mount_fuse "$OUT/fuse-r${ROUND}-final.stdout" || {
            fail "r$ROUND final remount failed"
            continue
        }
        if verify_round "$ROUND"; then
            pass "r$ROUND verify OK after full repair wall=${MIX_WALL:-?}s"
        else
            fail "r$ROUND verify still bad after repair"
        fi
    fi

    # Capture victim heal lines
    grep -E 'meta-heal:|meta-catchup:|checksum mismatch|Joined' \
        "$OUT/s${VICTIM}.stdout" 2>/dev/null | tail -8 | tee -a "$OUT/REVIEW.txt" || true

    unmount_fuse
    ./efs-mgmt status "$IP:$P1" >"$OUT/status-r${ROUND}.txt" 2>&1 || true
    if grep -q 'Cluster state: OK' "$OUT/status-r${ROUND}.txt" 2>/dev/null; then
        pass "r$ROUND cluster OK"
    else
        note "r$ROUND cluster status not OK; attempting peer repairs"
        for v in 1 2 3; do repair_victim "$v" || true; done
        ./efs-mgmt status "$IP:$P1" >"$OUT/status-r${ROUND}-retry.txt" 2>&1 || true
        if grep -q 'Cluster state: OK' "$OUT/status-r${ROUND}-retry.txt" 2>/dev/null; then
            pass "r$ROUND cluster OK after repair"
        else
            fail "r$ROUND cluster not OK"
            cat "$OUT/status-r${ROUND}-retry.txt" | tee -a "$OUT/REVIEW.txt" || true
        fi
    fi
done

# Final rollup
{
    echo "==== FINAL ===="
    echo "rounds=$ROUNDS fail=$FAIL"
    echo "bytes_per_round≈$BYTES_ROUND"
    ./efs-mgmt status "$IP:$P1" 2>&1 || true
    timeout 15 ./efs-query "$IP:$P1" 2>&1 || true
} | tee -a "$OUT/REVIEW.txt" | tee "$OUT/SUMMARY.txt"

if [ "$FAIL" -ne 0 ]; then
    echo "CHAOS_DD_MIX_FAIL"
    exit 1
fi
echo "CHAOS_DD_MIX_OK"
exit 0
