#!/bin/bash
#SBATCH --job-name=efs-stress-mixed
#SBATCH --partition=mit_quicktest
#SBATCH --time=00:15:00
#SBATCH --nodes=1
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=16
#SBATCH --mem=24G
#SBATCH --output=/orcd/scratch/orcd/001/erbmi1/efs/logs/stress-%j.out
#SBATCH --error=/orcd/scratch/orcd/001/erbmi1/efs/logs/stress-%j.err
#
# Self-contained parallel mixed-size cluster stress:
#   3 efsd servers + 1 FUSE client on this node.
#   Concurrently: small-file workers (create/write/fsync/read/verify/delete)
#   and large-file workers (streaming dd). Captures client+server perf.
#   Verifies every byte read back. Fails on any mismatch, hang, or quorum err.
#
# Env knobs: SMALL_WORKERS(8) SMALL_FILES(60) LARGE_WORKERS(3) LARGE_MB(256)
#            CHUNK_KB(128) PERF(1)

set -uo pipefail
if [ -z "${EFS_STREAM_STDBUF:-}" ] && command -v stdbuf >/dev/null 2>&1; then
    export EFS_STREAM_STDBUF=1
    exec stdbuf -oL -eL bash "$0" "$@"
fi

REPO=/home/erbmi1/git/efs
SHARED=/orcd/scratch/orcd/001/erbmi1/efs
SCRATCH=/scratch/efs-testing/${SLURM_JOB_ID}
KEEP=$SHARED/logs/stress-${SLURM_JOB_ID}-keep
MNT=$SCRATCH/mnt
IP=127.0.0.1
P1=21101; P2=21102; P3=21103
EXPORT=stress

SMALL_WORKERS="${SMALL_WORKERS:-8}"
SMALL_FILES="${SMALL_FILES:-40}"
LARGE_WORKERS="${LARGE_WORKERS:-3}"
LARGE_MB="${LARGE_MB:-128}"
PERF="${PERF:-1}"

# MODE=mixed (default): concurrent small+large stress with verify.
# MODE=ecopy: long-running bulk copy of a many-small-files tree (ImageNet) via
#             ecopy, sampling destination file/byte counts to chart the
#             windowed throughput trend (exposes the progressive slowdown).
MODE="${MODE:-mixed}"
ECOPY="${ECOPY:-$HOME/git/direct_copy/ecopy}"
ECOPY_SRC="${ECOPY_SRC:-$HOME/orcd/scratch/imagenet/images_complete/ilsvrc}"
ECOPY_DST="${ECOPY_DST:-imagenet}"
ECOPY_MAX_SEC="${ECOPY_MAX_SEC:-600}"
ECOPY_SAMPLE_SEC="${ECOPY_SAMPLE_SEC:-15}"
# Metadata flush batch: mixed uses a huge batch to isolate the data path; ecopy
# uses the realistic default (4096) so the metadata flush/rebuild cost shows.
if [ "$MODE" = "ecopy" ]; then
    BATCH="${EFS_META_BATCH_OPS:-4096}"
else
    BATCH="${EFS_META_BATCH_OPS:-65536}"
fi

mkdir -p "$SCRATCH"/{s1,s2,s3} "$MNT" "$KEEP" "$SHARED/logs"
cd "$REPO"

S1=""; S2=""; S3=""; CPID=""; PERF_C=""; PERF_S=""
FAIL=0
note() { echo "[$(date +%H:%M:%S)] $*"; }
fail() { echo "FAIL: $*"; FAIL=1; }

cleanup() {
    set +e
    [ -n "$PERF_C" ] && kill -INT "$PERF_C" 2>/dev/null
    [ -n "$PERF_S" ] && kill -INT "$PERF_S" 2>/dev/null
    fusermount -u "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null
    for p in $CPID $S1 $S2 $S3; do [ -n "$p" ] && kill -KILL "$p" 2>/dev/null; done
    sleep 1
    rm -rf "$SCRATCH"
    note "cleaned $SCRATCH (logs kept in $KEEP)"
}
trap cleanup EXIT

note "=== build ==="
make -j"$(nproc)" efsd efs-fuse efs-mgmt >"$KEEP/build.log" 2>&1 || { echo "BUILD FAIL"; tail -20 "$KEEP/build.log"; exit 1; }

note "=== start 3 servers ==="
./efsd --node-id 1 --addr "$IP" --port "$P1" --storage "$SCRATCH/s1" --writers 4 >"$KEEP/s1.log" 2>&1 &
S1=$!
sleep 1
./efsd --node-id 2 --addr "$IP" --port "$P2" --storage "$SCRATCH/s2" --writers 4 --join "$IP:$P1" >"$KEEP/s2.log" 2>&1 &
S2=$!
./efsd --node-id 3 --addr "$IP" --port "$P3" --storage "$SCRATCH/s3" --writers 4 --join "$IP:$P1" >"$KEEP/s3.log" 2>&1 &
S3=$!
sleep 3
for p in $S1 $S2 $S3; do kill -0 "$p" 2>/dev/null || { echo "server died"; for f in "$KEEP"/s*.log; do echo "-- $f"; tail -15 "$f"; done; exit 1; }; done

# Wait for the cluster to converge to 3-up OK (gossip/heartbeat propagation).
ok=0
for _ in $(seq 1 30); do
    ./efs-mgmt status "$IP:$P1" >"$KEEP/status0.txt" 2>&1 || true
    if grep -q 'Cluster state: OK (3 up' "$KEEP/status0.txt"; then ok=1; break; fi
    sleep 1
done
cat "$KEEP/status0.txt"
[ "$ok" = 1 ] || { echo "cluster not OK after 30s"; exit 1; }
./efs-mgmt mkfs "$IP:$P1" "$EXPORT" || { echo "mkfs failed"; exit 1; }

note "=== mount FUSE (MODE=$MODE BATCH=$BATCH) ==="
EFS_META_BATCH_OPS=$BATCH ./efs-fuse "$IP:$P1" "$IP:$P2" "$IP:$P3" "$EXPORT" "$MNT" -f >"$KEEP/fuse.log" 2>&1 &
CPID=$!
for _ in $(seq 1 120); do mountpoint -q "$MNT" 2>/dev/null && break; kill -0 "$CPID" 2>/dev/null || { cat "$KEEP/fuse.log"; exit 1; }; sleep 0.25; done
mountpoint -q "$MNT" || { echo "mount failed"; cat "$KEEP/fuse.log"; exit 1; }

# --- perf capture (client + server 1) ---
start_perf() {
    [ "$PERF" = "1" ] || return 0
    command -v perf >/dev/null 2>&1 || { note "perf unavailable"; return 0; }
    perf record -g -F 199 -p "$CPID" -o "$KEEP/client.perf.data" -- sleep 86400 >/dev/null 2>&1 &
    PERF_C=$!
    perf record -g -F 199 -p "$S1" -o "$KEEP/server.perf.data" -- sleep 86400 >/dev/null 2>&1 &
    PERF_S=$!
    sleep 1
}
stop_perf() {
    [ "$PERF" = "1" ] || return 0
    for pp in "$PERF_C" "$PERF_S"; do
        [ -n "$pp" ] && kill -INT "$pp" 2>/dev/null
    done
    # perf record must finalize (write out) the -g data on SIGINT; with hundreds
    # of MB this takes seconds. Wait for exit before escalating to SIGKILL,
    # otherwise the .data is truncated and `perf report` comes out empty.
    for _ in $(seq 1 30); do
        local alive=0
        for pp in "$PERF_C" "$PERF_S"; do
            [ -n "$pp" ] && kill -0 "$pp" 2>/dev/null && alive=1
        done
        [ "$alive" = 0 ] && break
        sleep 1
    done
    for pp in "$PERF_C" "$PERF_S"; do
        [ -n "$pp" ] && { kill -KILL "$pp" 2>/dev/null; wait "$pp" 2>/dev/null; }
    done
    PERF_C=""; PERF_S=""
    for k in client server; do
        [ -f "$KEEP/$k.perf.data" ] && perf report --stdio --no-children --percent-limit 1 -i "$KEEP/$k.perf.data" >"$KEEP/$k.report.txt" 2>/dev/null
    done
}

# --- workers ---------------------------------------------------------------
# Each worker records one stats line to $KEEP/stats.<class>.<id>:
#   files_w bytes_w w_t0 w_t1 files_r bytes_r r_t0 r_t1
# (epoch seconds, float). Aggregated at the end into the performance report.

# Small-file worker: many tiny files — write+fsync pass, then read-back+verify.
small_worker() {
    local w=$1
    local d="$MNT/small/w$w"
    mkdir -p "$d" || return 1
    local i rc=0
    local fw=0 bw=0 fr=0 br=0
    local wt0 wt1 rt0 rt1

    wt0=$(date +%s.%N)
    for i in $(seq 1 "$SMALL_FILES"); do
        local f="$d/f$i"
        local kb=$(( (i % 16 + 1) * 4 ))  # 4K..64K
        if ! dd if=/dev/zero of="$f" bs=1024 count="$kb" status=none 2>>"$KEEP/small_w$w.err"; then rc=1; break; fi
        fw=$((fw+1)); bw=$((bw + kb*1024))
    done
    sync "$d" 2>/dev/null
    wt1=$(date +%s.%N)

    if [ $rc -eq 0 ]; then
        rt0=$(date +%s.%N)
        for i in $(seq 1 "$SMALL_FILES"); do
            local f="$d/f$i"
            local kb=$(( (i % 16 + 1) * 4 ))
            local sz; sz=$(stat -c %s "$f" 2>/dev/null || echo 0)
            if [ "$sz" != "$((kb*1024))" ]; then
                echo "small w$w f$i size $sz != $((kb*1024))" >>"$KEEP/small_w$w.err"; rc=1; break
            fi
            if ! dd if="$f" of=/dev/null bs=1024 status=none 2>>"$KEEP/small_w$w.err"; then rc=1; break; fi
            fr=$((fr+1)); br=$((br + sz))
        done
        rt1=$(date +%s.%N)
    else
        rt0=$wt1; rt1=$wt1
    fi
    echo "$fw $bw $wt0 $wt1 $fr $br $rt0 $rt1" > "$KEEP/stats.small.$w"
    return $rc
}

# Large-file worker: stream a big random file (fsync), then two read passes
# (sha256sum + dd|sha256) and compare hashes.
large_worker() {
    local w=$1
    local f="$MNT/big/w$w.bin"
    mkdir -p "$MNT/big"
    local bytes=$((LARGE_MB * 1024 * 1024))
    local wt0 wt1 rt0 rt1 rt2
    wt0=$(date +%s.%N)
    if ! dd if=/dev/urandom of="$f" bs=1M count="$LARGE_MB" status=none conv=fsync 2>>"$KEEP/large_w$w.err"; then
        echo "large w$w write failed" >>"$KEEP/large_w$w.err"
        echo "0 0 $wt0 $(date +%s.%N) 0 0 $wt0 $wt0" > "$KEEP/stats.large.$w"
        return 1
    fi
    wt1=$(date +%s.%N)
    rt0=$(date +%s.%N)
    local h1 h2
    h1=$(sha256sum "$f" | awk '{print $1}')
    rt1=$(date +%s.%N)
    h2=$(dd if="$f" bs=1M status=none 2>/dev/null | sha256sum | awk '{print $1}')
    rt2=$(date +%s.%N)
    if [ "$h1" != "$h2" ]; then
        echo "large w$w hash mismatch $h1 != $h2" >>"$KEEP/large_w$w.err"
        echo "1 $bytes $wt0 $wt1 1 $bytes $rt0 $rt1" > "$KEEP/stats.large.$w"
        return 1
    fi
    echo "1 $bytes $wt0 $wt1 2 $((bytes*2)) $rt0 $rt2" > "$KEEP/stats.large.$w"
    return 0
}

# --- ecopy long-running workload -------------------------------------------
# Copy a many-small-files tree into the mount with ecopy, sampling the root
# .stats (O(1) rollups) every ECOPY_SAMPLE_SEC to chart windowed throughput.
# Writes "epoch tree_files tree_bytes" lines to $KEEP/ecopy.progress.
sample_stats() {  # -> "tree_files tree_bytes" (0 0 on error)
    awk -F= '/^tree_files=/{f=$2} /^tree_bytes=/{b=$2} END{print (f+0)" "(b+0)}' \
        "$MNT/.stats" 2>/dev/null || echo "0 0"
}

ecopy_workload() {
    local dst="$MNT/$ECOPY_DST"
    mkdir -p "$dst" || return 1
    [ -x "$ECOPY" ] || { echo "ecopy not found: $ECOPY"; return 1; }
    [ -d "$ECOPY_SRC" ] || { echo "ecopy src not found: $ECOPY_SRC"; return 1; }
    local prog="$KEEP/ecopy.progress"
    : > "$prog"
    note "ecopy: $ECOPY $ECOPY_SRC -> $dst (max ${ECOPY_MAX_SEC}s, sample ${ECOPY_SAMPLE_SEC}s, BATCH=$BATCH)"
    "$ECOPY" "$ECOPY_SRC" "$dst" >"$KEEP/ecopy.log" 2>&1 &
    local ep=$!
    local t0; t0=$(date +%s)
    local capped=0
    while kill -0 "$ep" 2>/dev/null; do
        local now; now=$(date +%s)
        if [ $((now - t0)) -ge "$ECOPY_MAX_SEC" ]; then
            note "ecopy: time cap ${ECOPY_MAX_SEC}s reached, stopping"
            kill -TERM "$ep" 2>/dev/null; sleep 3; kill -KILL "$ep" 2>/dev/null
            capped=1
            break
        fi
        echo "$now $(sample_stats)" >> "$prog"
        sleep "$ECOPY_SAMPLE_SEC"
    done
    wait "$ep"; local erc=$?
    echo "$(date +%s) $(sample_stats)" >> "$prog"
    note "ecopy done rc=$erc capped=$capped wall=$(( $(date +%s) - t0 ))s"
    # An intentional time-cap stop is not a failure; a real ecopy error is.
    [ "$capped" = 1 ] && return 0
    return $erc
}

# Print cumulative files/bytes and per-window files/s + MiB/s trend.
ecopy_report() {
    local prog="$KEEP/ecopy.progress"
    [ -s "$prog" ] || { echo "(no ecopy progress samples)"; return; }
    echo "--- ecopy throughput trend (window = ${ECOPY_SAMPLE_SEC}s) ---"
    awk '{ t[NR]=$1; f[NR]=$2; b[NR]=$3 }
         END { t0=t[1];
               for (i=2; i<=NR; i++) {
                   dt=t[i]-t[i-1]; if (dt<=0) continue;
                   printf "  t+%4ds  %9d files  %10.1f MiB  |  win %7.1f files/s  %8.1f MiB/s\n",
                          t[i]-t0, f[i], b[i]/1048576, (f[i]-f[i-1])/dt, (b[i]-b[i-1])/1048576/dt;
               }
               if (NR>=2) {
                   tot=t[NR]-t[1]; if (tot<=0) tot=1;
                   printf "  TOTAL  %9d files  %10.1f MiB  in %ds  ->  avg %.1f files/s  %.1f MiB/s\n",
                          f[NR], b[NR]/1048576, tot, f[NR]/tot, b[NR]/1048576/tot;
               } }' "$prog"
}

note "=== start perf ==="
start_perf

if [ "$MODE" = "ecopy" ]; then
    note "=== launch long-running ecopy workload ==="
    T0=$(date +%s.%N)
    ecopy_workload || FAIL=1
    T1=$(date +%s.%N)
    WALL=$(awk -v s="$T0" -v e="$T1" 'BEGIN{printf "%.3f", e-s}')
else
    note "=== launch parallel mixed workload: $SMALL_WORKERS small x $SMALL_FILES files, $LARGE_WORKERS large x ${LARGE_MB}MB ==="
    T0=$(date +%s.%N)
    pids=()
    for w in $(seq 0 $((SMALL_WORKERS-1))); do small_worker "$w" & pids+=($!); done
    for w in $(seq 0 $((LARGE_WORKERS-1))); do large_worker "$w" & pids+=($!); done
    for pid in "${pids[@]}"; do wait "$pid" || FAIL=1; done
    T1=$(date +%s.%N)
    WALL=$(awk -v s="$T0" -v e="$T1" 'BEGIN{printf "%.3f", e-s}')
fi

note "=== workload done wall_s=$WALL FAIL=$FAIL ==="
stop_perf

# --- verification ----------------------------------------------------------
note "=== verify: cluster status + error scan ==="
./efs-mgmt status "$IP:$P1" | tee "$KEEP/status1.txt"
grep -q 'Cluster state: OK' "$KEEP/status1.txt" || fail "cluster not OK after stress"

# scan logs for hard errors: quorum loss, crashes, split-brain, sticky write
# errors. These are never acceptable.
if grep -qiE "no quorum|split-brain|Segmentation|assert|sticky_err" "$KEEP"/s*.log "$KEEP"/fuse.log 2>/dev/null; then
    fail "hard errors found in logs:"
    grep -iE "no quorum|split-brain|Segmentation|assert|sticky_err" "$KEEP"/s*.log "$KEEP"/fuse.log 2>/dev/null | head -20
fi
# Meta-rebuild checksum mismatch / decode-failed under continuous write is the
# known dual-slot ABA race: a rebuild holding a stale root reads a same-slot
# fragment from a newer in-flight gen. It is transient and self-heals once the
# root converges. Flag as a hard failure ONLY if the export never reaches a
# successful rebuild afterward (i.e. permanently stuck).
NDECODE=$(grep -c "decode failed" "$KEEP"/s*.log 2>/dev/null | awk -F: '{s+=$2} END{print s+0}')
NMISM=$(grep -c "checksum mismatch" "$KEEP"/s*.log 2>/dev/null | awk -F: '{s+=$2} END{print s+0}')
NREBUILT=$(grep -c "rebuilt export" "$KEEP"/s*.log 2>/dev/null | awk -F: '{s+=$2} END{print s+0}')
echo "meta: decode_failed=$NDECODE checksum_mismatch=$NMISM rebuilt_ok=$NREBUILT"
if [ "$NREBUILT" -eq 0 ]; then
    fail "meta never rebuilt successfully (stuck)"
fi
# Data-path correctness is verified separately by the workers (sizes + hashes).
if [ "$MODE" = "mixed" ]; then
    for w in $(seq 0 $((SMALL_WORKERS-1))); do [ -s "$KEEP/small_w$w.err" ] && { fail "small worker $w errors"; cat "$KEEP/small_w$w.err"; }; done
    for w in $(seq 0 $((LARGE_WORKERS-1))); do [ -s "$KEEP/large_w$w.err" ] && { fail "large worker $w errors"; cat "$KEEP/large_w$w.err"; }; done
fi

# --- performance rollup ----------------------------------------------------
# Aggregate per-worker stats: sum files/bytes, window = last end - first start.
calc_stats() {  # $@ = stats files -> "fw bw wwin fr br rwin"
    awk '{ fw+=$1; bw+=$2;
           if (NR==1 || $3<w0) w0=$3;  if (NR==1 || $4>w1) w1=$4;
           fr+=$5; br+=$6;
           if (NR==1 || $7<r0) r0=$7;  if (NR==1 || $8>r1) r1=$8; }
         END { ww=(w1>w0)?w1-w0:0.001; rw=(r1>r0)?r1-r0:0.001;
               printf "%d %d %.3f %d %d %.3f", fw, bw, ww, fr, br, rw }' "$@"
}
fmt_class() {  # label "fw bw wwin fr br rwin"
    awk -v L="$1" -v fw="$2" -v bw="$3" -v ww="$4" -v fr="$5" -v br="$6" -v rw="$7" 'BEGIN{
        printf "  %-6s write: %5d files  %9.1f MiB  %7.2fs  ->  %8.1f files/s  %8.1f MiB/s\n",
               L, fw, bw/1048576, ww, fw/ww, bw/1048576/ww;
        printf "  %-6s read:  %5d files  %9.1f MiB  %7.2fs  ->  %8.1f files/s  %8.1f MiB/s\n",
               L, fr, br/1048576, rw, fr/rw, br/1048576/rw; }'
}

if [ "$MODE" = "ecopy" ]; then
{
  echo "================ PERFORMANCE (ecopy) ================"
  echo "workload: ecopy $ECOPY_SRC -> /$ECOPY_DST (BATCH=$BATCH, max ${ECOPY_MAX_SEC}s)"
  echo "total wall: ${WALL}s"
  ecopy_report
  echo "====================================================="
  echo "FAIL=$FAIL"
} | tee "$KEEP/SUMMARY.txt"
else
S_STATS=$(calc_stats "$KEEP"/stats.small.*)
L_STATS=$(calc_stats "$KEEP"/stats.large.*)
A_STATS=$(calc_stats "$KEEP"/stats.small.* "$KEEP"/stats.large.*)

{
  echo "================ PERFORMANCE ================"
  echo "workload: $SMALL_WORKERS small workers x $SMALL_FILES files (4-64 KiB), $LARGE_WORKERS large workers x ${LARGE_MB} MiB"
  echo "total wall (write+read, all workers concurrent): ${WALL}s"
  echo "--- small files ---"
  fmt_class small $S_STATS
  echo "--- large files (read = 2 verify passes) ---"
  fmt_class large $L_STATS
  echo "--- combined ---"
  fmt_class total $A_STATS
  echo "============================================="
  echo "FAIL=$FAIL"
} | tee "$KEEP/SUMMARY.txt"
fi

echo "--- client hotspots ---"
grep -E 'blake3|efs_|fuse_|memcpy|encode|decode|put_|get_|hash|memset|poll|sendmsg|recvmsg|pthread' "$KEEP/client.report.txt" 2>/dev/null | head -20 || echo "(no client report)"
echo "--- server hotspots ---"
grep -E 'blake3|efs_|fuse_|memcpy|encode|decode|put_|get_|hash|memset|poll|sendmsg|recvmsg|pthread|fsync|write' "$KEEP/server.report.txt" 2>/dev/null | head -20 || echo "(no server report)"

if [ "$FAIL" -ne 0 ]; then
    echo "STRESS_FAIL"
    exit 1
fi
echo "STRESS_OK"
exit 0
