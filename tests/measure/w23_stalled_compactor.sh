#!/bin/bash
# W23 / performance-plan P2.3 — the stalled-compactor test.
# (docs/backlog/work-items.md "W23 — Server: the apply path blocks on L0
# back-pressure ..." — the Oct 2 2026 correction; the measurement only,
# not a compactor fix.)
#
# Private 3-node cluster on ONE dev-cluster VM (tests/rdma_first_inode.sh
# layout: one host, three efsd, scratch storage), one FUSE client, dd
# bs=1M of a non-zero source to fresh files. One follower runs with
# EFS_FAULT_COMPACT_STALL=1 while /tmp/efs/fault exists, so its compactor
# parks between iterations holding no locks (merges stop; memtable
# flushes to L0 continue). Every 5 s per node the sampler records:
# memtable bytes, n_l0, l0_bytes (the pump's kv-obs line), pump_hold_max
# (raft-obs), apply-sleep and kv-compact: backpressure line counts,
# commit - applied per group (efs-mgmt raft-status), and efsd RSS.
#
# The run stops at the first of (the W23 text's bounded stopping
# condition):
#   1. the follower logs kv-compact: backpressure
#   2. follower RSS exceeds 2x its pre-run RSS
#   3. follower commit - applied exceeds 10000 entries for 30 s
#   4. BYTES_CAP written (default 10 GiB; the disk preflight lowers it)
#   5. 10 minutes
# Then the fault file is removed and the time for the follower to reach
# commit == applied on both groups is recorded (over 2 min is a second
# finding). The SUMMARY names which bound fired and at what size; "no
# bound observed" is a valid result — a finite run cannot prove
# unbounded growth.
#
# Runs on the dev Tart cluster (~/git/cluster): everything executes on
# NODE (default efs1); the Mac side builds the faults binary, deploys the
# driver, and collects the result dir. Never touches the dev cluster's
# own efsd (port 17432) or /efs/data. Kills only its own processes
# (ports 19550-19552, this mount point).
#
# Usage: bash tests/measure/w23_stalled_compactor.sh
# Env:   NODE=efs1 BYTES_CAP=<bytes> TIME_CAP=600 CLUSTER_DIR=~/git/cluster
set -euo pipefail
HERE=$(cd "$(dirname "$0")/../.." && pwd)   # repo root: results live in results/measure/
CLUSTER_DIR=${CLUSTER_DIR:-$HOME/git/cluster}
NODE=${NODE:-efs1}
# shellcheck source=/dev/null
source "$CLUSTER_DIR/env.sh"

STAMP=$(date -u +%Y%m%d-%H%M%S)
OUT=${OUT:-$HERE/results/measure/$STAMP-w23-stalled-compactor}
mkdir -p "$OUT"
say() { echo "[w23] $*"; }
say "results -> $OUT"

# --- preflight: node reachable, deploy build present, scratch disk sized
ssh_node "$NODE" 'test -x /tmp/efs/efsd && test -x /tmp/efs/efs-fuse && test -x /tmp/efs/efs-mgmt' \
    || { say "FAIL: /tmp/efs binaries missing on $NODE — run $CLUSTER_DIR/deploy.sh first"; exit 1; }
ssh_node "$NODE" 'df -k /var/tmp /tmp; free -m; pgrep -x efsd >/dev/null && echo "main-cluster efsd up (untouched)"' \
    > "$OUT/preflight.txt" 2>&1
cat "$OUT/preflight.txt"

# --- build the faults efsd on the node (same tree, same build id; the
# normal /tmp/efs/efsd is rebuilt afterwards so the deploy binary is
# byte-identical to a plain build). Only kv_compact.c carries the hook.
say "building faults efsd on $NODE"
ssh_node "$NODE" 'cd /tmp/efs && set -e &&
    touch src/kv/kv_compact.c &&
    make src/kv/kv_compact.o EXTRA_DEFS=-DEFS_FAULTS=1 >/dev/null &&
    make efsd EXTRA_DEFS=-DEFS_FAULTS=1 >/dev/null &&
    cp efsd /tmp/efs-faults-efsd &&
    touch src/kv/kv_compact.c &&
    make src/kv/kv_compact.o >/dev/null &&
    make efsd >/dev/null &&
    echo FAULTS_STR=$(strings /tmp/efs-faults-efsd | grep -c "kv-fault: compactor parked") &&
    echo NORMAL_STR=$(strings /tmp/efs/efsd | grep -c "kv-fault: compactor parked")' \
    | tee "$OUT/faults-build.txt"
grep -q 'FAULTS_STR=1' "$OUT/faults-build.txt" || { say "FAIL: hook not in the faults binary"; exit 1; }
grep -q 'NORMAL_STR=0' "$OUT/faults-build.txt" || { say "FAIL: hook leaked into the normal binary"; exit 1; }
say "faults binary OK (hook present only in /tmp/efs-faults-efsd)"

# --- the driver, run detached on the node
ssh_node "$NODE" 'mkdir -p /tmp/w23'
cat > /tmp/w23-driver.$$ <<'DRIVER'
#!/bin/bash
# W23 stalled-compactor driver — runs entirely on the one VM.
set -uo pipefail
BIN=/tmp/efs
FBIN=/tmp/efs-faults-efsd
WORK=/tmp/w23
STORE=${W23_STORE:-/var/tmp/w23-scratch}
PORT=19550
MNT=$WORK/mnt
FAULT_FILE=/tmp/efs/fault
BYTES_CAP=${W23_BYTES_CAP:-0}   # 0 = driver computes from free disk
TIME_CAP=${W23_TIME_CAP:-600}
FILE_MIB=256
OUTD=$WORK/out
SAMPLES=$OUTD/samples.tsv

kill_ours() {
    ps -eo pid,args | awk '/[e]fsd/ && / --port 1955[012]( |$)/ {print $1}' | xargs -r kill -9 2>/dev/null
    ps -eo pid,args | awk -v m="$MNT" '$0 ~ "[e]fs-fuse" && index($0, m) {print $1}' | xargs -r kill -9 2>/dev/null
    fusermount3 -uz "$MNT" 2>/dev/null || umount -l "$MNT" 2>/dev/null || true
    return 0
}
trap kill_ours EXIT

say() { echo "[driver $(date -u +%H:%M:%S)] $*"; }

# disk preflight: 2+1 EC puts 1.5x the logical bytes on this one disk
# (all three nodes store here) plus KV/WAL; keep 2 GiB of headroom.
free_kb=$(df -k --output=avail "$(dirname "$STORE")" | tail -1 | tr -dc '0-9')
if [ "$BYTES_CAP" = 0 ]; then
    cap_kb=$(( (free_kb - 2*1024*1024) * 10 / 16 ))
    cap_kb=$(( cap_kb / 262144 * 262144 ))   # whole 256 MiB files
    [ "$cap_kb" -gt 10485760 ] && cap_kb=10485760   # the text's 10 GiB
    BYTES_CAP=$(( cap_kb * 1024 ))
fi
say "free=$(( free_kb / 1024 )) MiB -> BYTES_CAP=$(( BYTES_CAP / 1048576 )) MiB TIME_CAP=${TIME_CAP}s"
if [ "$BYTES_CAP" -lt 268435456 ]; then
    say "FATAL: not enough disk for even one file"
    exit 2
fi

kill_ours
rm -rf "$STORE" "$OUTD"
mkdir -p "$STORE" "$OUTD" "$MNT"
rm -f "$FAULT_FILE"

# non-zero source (all-zero chunks skip the PUT — tests/measure/lib.sh)
python3 - <<'PY'
b = b"\x5a" * (1 << 20)
with open("/tmp/w23/src", "wb") as f:
    for _ in range(256):
        f.write(b)
PY

start_node() {  # start_node <id> [extra env...]
    local id=$1; shift
    local port=$((PORT + id - 1))
    env EFS_MD_RAFT_N=3 EFS_TRANSPORT=tcp EFS_RAFT_OBS=1 "$@" \
        "$FBIN" --node-id "$id" --addr 127.0.0.1 --port "$port" \
        --storage "$STORE/s$id" --quota 4G --no-direct-io \
        ${JOIN:+--join 127.0.0.1:$PORT} \
        >"$WORK/s$id.log" 2>&1 </dev/null &
    local i
    for i in $(seq 1 100); do
        grep -q listening "$WORK/s$id.log" 2>/dev/null && return 0
        sleep 0.2
    done
    say "FATAL: node $id did not start"; tail -20 "$WORK/s$id.log"; exit 2
}

# Nodes 1+2 first; mkfs elects both groups' leaders among them, so node 3
# (the stalled one) joins as a follower of both groups.
start_node 1
JOIN=1
start_node 2
mk=1
for _ in $(seq 1 20); do
    if "$BIN/efs-mgmt" raft-mkfs "127.0.0.1:$PORT" w23 >/dev/null 2>&1; then mk=0; break; fi
    sleep 0.5
done
[ "$mk" = 0 ] || { say "FATAL: raft-mkfs"; exit 2; }
start_node 3 env EFS_FAULT_COMPACT_STALL=1

# wait for node 3 to be a caught-up follower of both groups
ok=0
for _ in $(seq 1 60); do
    st=$("$BIN/efs-mgmt" raft-status 127.0.0.1:$((PORT + 2)) 2>/dev/null || true)
    if [ -n "$st" ] && ! echo "$st" | grep -q 'role=-' && \
       ! echo "$st" | grep -q 'role=LEADER' && \
       [ "$(echo "$st" | awk '/group/{print}' | grep -c 'commit=')" = 2 ]; then
        lag=$(echo "$st" | awk '{for(i=1;i<=NF;i++){if($i~/^commit=/)c=substr($i,8); if($i~/^applied=/)a=substr($i,9)}; print c-a}' | sort -n | tail -1)
        [ "${lag:-1}" = 0 ] && { ok=1; break; }
    fi
    sleep 0.5
done
say "node3 initial raft-status:"
"$BIN/efs-mgmt" raft-status 127.0.0.1:$((PORT + 2)) | tee "$OUTD/node3-join-status.txt"
[ "$ok" = 1 ] || { say "FATAL: node 3 not a caught-up follower"; exit 2; }

# one client (efs-fuse daemonizes; poll for the mount to serve)
rm -f "$WORK/fuse.log"
EFS_TRANSPORT=tcp setsid "$BIN/efs-fuse" "127.0.0.1:$PORT" w23 "$MNT" \
    >"$WORK/fuse.log" 2>&1 </dev/null &
served=0
for _ in $(seq 1 50); do
    timeout 3 stat "$MNT" >/dev/null 2>&1 && { served=1; break; }
    sleep 0.2
done
[ "$served" = 1 ] || { say "FATAL: mount not serving"; tail -20 "$WORK/fuse.log"; exit 2; }
mkdir "$MNT/w23"

pid_of() { pgrep -f "[e]fsd .*--port $((PORT + $1 - 1))( |$)" | head -1; }
kvobs() { grep 'kv-obs:' "$WORK/s$1.log" 2>/dev/null | tail -1 | \
          sed -nE 's/.*mt_bytes=([0-9]+) l0_bytes=([0-9]+) l0=([0-9]+) l1=([0-9]+).*/\1 \2 \3 \4/p'; }
phm() { grep 'raft-obs: wait_timeouts' "$WORK/s$1.log" 2>/dev/null | tail -1 | \
        sed -nE 's/.*pump_hold_max=([0-9]+)us.*/\1/p'; }
lag_of() { "$BIN/efs-mgmt" raft-status 127.0.0.1:$((PORT + $1 - 1)) 2>/dev/null | \
        awk '{c=a=""; for(i=1;i<=NF;i++){if($i~/^commit=/)c=substr($i,8); if($i~/^applied=/)a=substr($i,9)}; if(c!=""&&a!="") print c-a}' | \
        sort -n | tail -1; }
role_of() { "$BIN/efs-mgmt" raft-status 127.0.0.1:$((PORT + $1 - 1)) 2>/dev/null | \
        awk '{for(i=1;i<=NF;i++) if($i~/^role=/) print substr($i,6)}' | sort -u | tr '\n' ',' ; }

# sampler: one TSV row per node per 5 s
sampler() {
    echo -e "ts\tnode\tmt_bytes\tl0_bytes\tn_l0\tn_l1\trss_kb\tlag_max\tpump_hold_max_us\tapply_sleep_n\tbackpressure_n\trole" > "$SAMPLES"
    while [ ! -f "$WORK/sampler.stop" ]; do
        local ts n rss lag p ko as bp ro
        ts=$(date -u +%H:%M:%S)
        for n in 1 2 3; do
            rss=$(awk '/VmRSS/{print $2}' "/proc/$(pid_of "$n")/status" 2>/dev/null || echo 0)
            lag=$(lag_of "$n"); ko=$(kvobs "$n"); p=$(phm "$n")
            as=$(grep -c 'apply-sleep' "$WORK/s$n.log" 2>/dev/null || true)
            bp=$(grep -c 'kv-compact: backpressure' "$WORK/s$n.log" 2>/dev/null || true)
            ro=$(role_of "$n")
            echo -e "$ts\t$n\t${ko:-0 0 0 0}\t${rss:-0}\t${lag:-0}\t${p:-0}\t$as\t$bp\t${ro:-?}" >> "$SAMPLES"
        done
        sleep 5
    done
}
rm -f "$WORK/sampler.stop"
sampler &
SAMPID=$!

# warm-up: with the compactor live, write two files so efsd reaches its
# steady-state RSS and the compactor has cycled — a 2x bound against a
# cold 4 MB RSS fires on ordinary allocations, not on stalled-merge growth.
say "warm-up: 2 x ${FILE_MIB} MiB with the compactor live"
for wf in wa wb; do
    timeout 120 dd if=/tmp/w23/src of="$MNT/w23/$wf" bs=1M count=$FILE_MIB \
            conv=fsync status=none 2>>"$OUTD/dd.log" || \
        { say "FATAL: warm-up dd failed (see dd.log)"; exit 2; }
done
sleep 10

rss0=$(awk '/VmRSS/{print $2}' "/proc/$(pid_of 3)/status")
say "pre-run: node3 RSS ${rss0} kB (post warm-up); arming the fault"
echo stalled > "$FAULT_FILE"
armed=0
for _ in $(seq 1 75); do
    grep -q 'kv-fault: compactor parked' "$WORK/s3.log" 2>/dev/null && { armed=1; break; }
    sleep 0.2
done
[ "$armed" = 1 ] || { say "FATAL: hook did not engage on node 3"; exit 2; }
say "node 3 compactor parked; starting the dd loop"

t0=$(date +%s)
written=0
f=0
bound=""
lag_since=""
while [ "$written" -lt "$BYTES_CAP" ]; do
    now=$(date +%s)
    [ $((now - t0)) -ge "$TIME_CAP" ] && { bound="time-cap-10min"; break; }
    # A dd that fails (EIO from a BUSY-exhausted fsync, ENOSPC) or hangs
    # past 120 s is itself the finding — stop and keep the log.
    if ! timeout 120 dd if=/tmp/w23/src of="$MNT/w23/f$f" bs=1M count=$FILE_MIB \
            conv=fsync status=none 2>>"$OUTD/dd.log"; then
        bound="dd-error"; say "dd rc!=0 on file $f (see dd.log)"; break
    fi
    written=$((written + FILE_MIB * 1048576))
    f=$((f + 1))
    # bounds, from the follower's own counters
    if grep -q 'kv-compact: backpressure' "$WORK/s3.log" 2>/dev/null; then
        bound="backpressure"; break
    fi
    rss=$(awk '/VmRSS/{print $2}' "/proc/$(pid_of 3)/status" 2>/dev/null || echo 0)
    if [ "${rss:-0}" -gt $((2 * rss0)) ]; then bound="rss-2x"; break; fi
    lag=$(lag_of 3)
    if [ "${lag:-0}" -gt 10000 ]; then
        [ -z "$lag_since" ] && lag_since=$now
        [ $((now - lag_since)) -ge 30 ] && { bound="lag-10k-30s"; break; }
    else
        lag_since=""
    fi
done
t_stop=$(date +%s)
[ -z "$bound" ] && bound="bytes-cap"
say "STOP bound=$bound written_mib=$((written / 1048576)) files=$f wall_s=$((t_stop - t0))"

# release and time the catch-up (over 120 s is a second finding)
rm -f "$FAULT_FILE"
rel=0
for _ in $(seq 1 75); do
    grep -q 'kv-fault: compactor released' "$WORK/s3.log" 2>/dev/null && { rel=1; break; }
    sleep 0.2
done
say "compactor released flag=$rel; waiting for commit==applied on node 3"
rc0=$(date +%s)
recovered=0
for _ in $(seq 1 90); do
    [ "$(lag_of 3)" = 0 ] && { recovered=1; break; }
    sleep 2
done
rec_s=$(( $(date +%s) - rc0 ))
say "recovery: recovered=$recovered after ${rec_s}s"

sleep 5   # one last sample
touch "$WORK/sampler.stop"; wait "$SAMPID" || true

# roles at stop + the last kv-obs of every node, for the summary
for n in 1 2 3; do
    { echo "== node $n"; "$BIN/efs-mgmt" raft-status 127.0.0.1:$((PORT + n - 1)); } \
        >> "$OUTD/final-status.txt" 2>&1
done
cp "$WORK"/s*.log "$WORK/fuse.log" "$OUTD/" 2>/dev/null || true

# peaks from the sampler file (nodes 1..3 rows)
awk -F'\t' 'NR>1 {
    if ($3+0 > mt[$2]) mt[$2]=$3+0;
    if ($4+0 > l0b[$2]) l0b[$2]=$4+0;
    if ($5+0 > nl0[$2]) nl0[$2]=$5+0;
    if ($7+0 > rss[$2]) rss[$2]=$7+0;
    if ($8+0 > lag[$2]) lag[$2]=$8+0;
    if ($9+0 > phm[$2]) phm[$2]=$9+0;
    as[$2]=$10+0; bp[$2]=$11+0;
} END {
    for (n=1; n<=3; n++)
        printf "node%d peak_mt_bytes=%d peak_l0_bytes=%d peak_n_l0=%d peak_rss_kb=%d peak_lag=%d max_pump_hold_us=%d apply_sleep_lines=%d backpressure_lines=%d\n", \
               n, mt[n], l0b[n], nl0[n], rss[n], lag[n], phm[n], as[n], bp[n];
}' "$SAMPLES" | tee "$OUTD/peaks.txt"

{
    echo "W23 stalled-compactor run (dev Tart cluster, one VM, private 3-node)"
    echo "bound=$bound"
    echo "written_mib=$(( written / 1048576 )) files=$f wall_s=$((t_stop - t0))"
    echo "node3_prerun_rss_kb=$rss0"
    echo "recovered=$recovered recovery_s=$rec_s"
    echo "bytes_cap_mib=$(( BYTES_CAP / 1048576 )) time_cap_s=$TIME_CAP"
} | tee "$OUTD/run.txt"
say "driver done"
DRIVER
scp_driver() { sshpass -p "$SSH_PASS" scp $SSH_OPTS /tmp/w23-driver.$$ "$SSH_USER@$(ip_of "$NODE"):/tmp/w23/run.sh"; }
scp_driver
rm -f /tmp/w23-driver.$$

say "driver running on $NODE (detached)"
ssh_node "$NODE" 'setsid bash /tmp/w23/run.sh > /tmp/w23/driver.log 2>&1 < /dev/null & echo PID=$!'

# poll until the driver prints its trailer (or 25 min pass)
i=0
while [ $i -lt 150 ]; do
    if ssh_node "$NODE" 'grep -q "driver done\|FATAL" /tmp/w23/driver.log 2>/dev/null'; then break; fi
    sleep 10
    i=$((i + 1))
done
ssh_node "$NODE" 'cat /tmp/w23/driver.log' | tee "$OUT/driver.log"
ssh_node "$NODE" 'grep -q "driver done" /tmp/w23/driver.log' || { say "FAIL: driver did not finish — see $OUT/driver.log"; exit 1; }

# collect
ssh_node "$NODE" 'tar -C /tmp/w23/out -cf - .' | tar -C "$OUT" -xf -
ssh_node "$NODE" 'rm -rf /tmp/w23 /var/tmp/w23-scratch /tmp/efs/fault' || true

# SUMMARY.txt from run.txt + peaks.txt
{
    echo "W23 / P2.3 — stalled-compactor test — $STAMP"
    echo
    echo "Setup: private 3-node cluster on $NODE (tests/rdma_first_inode.sh"
    echo "layout, ports 19550-19552, TCP, scratch storage), one FUSE client,"
    echo "dd bs=1M conv=fsync of a non-zero source to fresh 256 MiB files."
    echo "Node 3 (follower of both groups) ran EFS_FAULT_COMPACT_STALL=1 with"
    echo "/tmp/efs/fault present: its compactor parked between iterations;"
    echo "flushes to L0 continued, L0 merges stopped. Sampled every 5 s per"
    echo "node: kv-obs mt_bytes/l0_bytes/n_l0/n_l1, raft-obs pump_hold_max,"
    echo "apply-sleep and kv-compact: backpressure line counts, raft-status"
    echo "commit-applied, RSS."
    echo
    cat "$OUT/run.txt"
    echo
    cat "$OUT/peaks.txt"
} > "$OUT/SUMMARY.txt"
say "done -> $OUT/SUMMARY.txt"
cat "$OUT/SUMMARY.txt"
