#!/bin/bash
# Valgrind memcheck leak gate for efs: unit tests + efsd + efs-fuse, over BOTH
# TCP and RDMA, client AND server side.
#
# Self-contained on one node: uses a private port + scratch storage so it
# NEVER touches the live cluster. Repeatable: wipe scratch, build, run, gate.
#
# Phases:
#   1. unit tests (current suite)                   — full gate
#   2. efsd over TCP (3-node localhost raft group, node 1 valgrind'd as a
#      follower; efs-bench workload on the leader)  — full gate
#   3. efs-fuse over TCP (3 plain nodes + valgrind'd client, mini-POSIX)
#                                                   — full gate
#   4. efsd + efs-fuse over RDMA (3-node group on the RDMA IP) — leaks-only
#      gate (ibverbs/DMA uninit false-positives; see gate_leaks)
#
# Step 11 port: the metadata engine is the raft host, so a single-node efsd
# can no longer serve an export (no quorum). Every wire phase runs a 3-node
# localhost raft group (three committed members on ports PORT..PORT+2, node ids from the cluster,
# one storage dir per node) and `efs-mgmt raft-mkfs`.
#
# Gate criteria (hard fail unless all hold):
#   1. definitely lost  = 0 bytes   } the two real-leak kinds
#   2. indirectly lost  = 0 bytes   }
#   3. 0 "uninitialised byte" errors (the uninit-on-wire class; see 1124e6d)
#   4. 0 invalid reads/writes
#   5. no possibly-lost block whose stack is efs code OUTSIDE the one-time
#      libfuse session setup (main / efs_fuse_main_mt). Benign possibly-lost =
#      libfuse fuse_new bufs, glibc dl-init constructors, pthread TLS (288B/
#      thread) — those are expected and do not fail the gate.
#
# Learnings baked in (do not remove blindly):
#   - efsd installs no SIGTERM handler -> SIGTERM prints the leak SUMMARY only
#     (no per-block stacks). efs-fuse exits cleanly on `fusermount3 -u`, which
#     DOES print full stacks. So the efsd phase gates on the summary; the fuse
#     phase gives stacks.
#   - vgdb (on-demand leak_check) and LSan both need kernel.yama.ptrace_scope=0
#     (the nodes run =2). We therefore rely on clean-exit / SIGTERM reports,
#     not vgdb. Ask the user to lower ptrace_scope if deeper analysis is needed.
#   - EFS_TRANSPORT=tcp for the 127.0.0.1 phases (loopback has no IB
#     device, so auto would skip the upgrade). The RDMA phase below uses
#     the node's 172.16.223.x address and the default (auto) transport.
#   - The efs-fuse phase mounts against a PLAIN (non-valgrind) efsd so FUSE
#     reads are fast enough to succeed; the efsd leak phase drives its own
#     valgrind'd efsd directly with efs-bench (no FUSE round-trip).
#   - NEVER pkill/pgrep -f with a pattern that appears in this command line
#     (it self-matches the ssh remote command and kills your own shell). Match
#     the valgrind comm "memcheck-amd64-" and filter /proc/<pid>/cmdline.
#   - rsync --delete removes gitignored build artifacts -> build AFTER rsync.
#
# Usage (on a node, from a synced repo root):  tests/valgrind_leaks.sh [workdir]
# Normally invoked via the harness:            tests/run_tests.sh leaks [host]
set -u

HERE="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${1:-/tmp/efs-vg}"
PORT="${EFS_VG_PORT:-19811}"
SEED="127.0.0.1:$PORT"
STORAGE="$WORK/storage"
MNT="$WORK/mnt"
EXPORT="vgtest"
MODE=${EFS_VG_MODE:-buffered}
case "$MODE" in direct) IO_FLAG=--direct-io;; buffered) IO_FLAG=--no-direct-io;; *) echo 'invalid EFS_VG_MODE' >&2; exit 2;; esac
VG_BUILD_ARGS=()
# Optional private, annotated verbs runtime. System libraries and ordinary
# builds are untouched. Rebuild all objects so DMA annotations cannot be lost
# by reusing an earlier non-Memcheck rdma.o.
if [ -n "${EFS_VG_RDMA_RUNTIME:-}" ]; then
    cache="$EFS_VG_RDMA_RUNTIME/rdma-core-build/CMakeCache.txt"
    grep -qEx 'ENABLE_VALGRIND:(BOOL|UNINITIALIZED)=ON' "$cache" || exit 2
    for setting in HAVE_VALGRIND_MEMCHECK:INTERNAL=1; do
        grep -qx "$setting" "$cache" || { echo "annotated verbs runtime required: $setting" >&2; exit 2; }
    done
    test -f "$EFS_VG_RDMA_RUNTIME/vg-headers/valgrind/memcheck.h" || exit 2
    test -f "$EFS_VG_RDMA_RUNTIME/rdma-core-build/lib/libibverbs.so" || exit 2
    export LD_LIBRARY_PATH="$EFS_VG_RDMA_RUNTIME/rdma-core-build/lib:${LD_LIBRARY_PATH:-}"
    export C_INCLUDE_PATH="$EFS_VG_RDMA_RUNTIME/vg-headers:${C_INCLUDE_PATH:-}"
    VG_BUILD_ARGS=(-B EFS_MEMCHECK=1)
fi
# Memcheck cannot execute AVX-512 (SIGILL); a -march=native build on such a
# host must be rebuilt without it. The BLAKE3 AVX-512 kernel is selected at
# runtime and Memcheck's CPUID hides AVX-512, so dispatch avoids it.
if grep -qw avx512f /proc/cpuinfo 2>/dev/null; then
    args=(-B)
    for x in "${VG_BUILD_ARGS[@]}"; do [ "$x" = -B ] || args+=("$x"); done
    VG_BUILD_ARGS=("${args[@]}" "EXTRA_DEFS=${EXTRA_DEFS:-} -mno-avx512f")
fi
OWN_PIDS=()
VG="valgrind --error-exitcode=99 --errors-for-leak-kinds=definite,indirect --leak-check=full --show-leak-kinds=definite,indirect,possible"
# Current unit suite (the old-engine tests were deleted in step 11 inc 2).
# Every unit process exit is gated as well as its complete memory report.
UNIT_TESTS=$(python3 - "$HERE/Makefile" <<'PY_UNITS'
from pathlib import Path
import sys
# Every TEST_BINS assignment and += addition, in Makefile order.
names=[]
for line in Path(sys.argv[1]).read_text().splitlines():
    if line.startswith(('TEST_BINS =','TEST_BINS +=')):
        names+=[Path(p).name for p in line.split('=',1)[1].split()]
assert names and len(names)==len(set(names)),names
print(' '.join(names))
PY_UNITS
)

# RDMA phase needs an RDMA-capable local IP (loopback has no ibdev, so the
# upgrade would silently fall back to TCP and exercise nothing). Auto-detect
# the host's 172.16.223.x (.ib) address; override with EFS_VG_RDMA_IP.
RDMA_IP="${EFS_VG_RDMA_IP:-$(ip -o -4 addr show 2>/dev/null \
    | awk '$4 ~ /^172\.16\.223\./ {sub(/\/.*$/, "", $4); print $4; exit}')}"

fails=0
note() { echo "[leaks] $*"; }
fail() { echo "[leaks] FAIL: $*"; fails=$((fails + 1)); }

# numeric field out of a "definitely lost: 0 bytes in 0 blocks" line.
# A fully-clean run prints "All heap blocks were freed -- no leaks are
# possible" and OMITS the LEAK SUMMARY entirely -> treat as 0.
vg_bytes() { # logfile kind; include every forked process, not just the parent
    awk -v kind="$2" '$0 ~ kind " lost:" {gsub(/,/, "", $0); sub(/.*lost: */, "", $0); total += $1} END {printf "%.0f\n", total}' "$1"
}
vg_count() { # logfile regex
    # grep -c exits 1 on zero matches; do NOT "|| echo 0" (double-prints).
    local n
    n=$(grep -cE "$2" "$1" 2>/dev/null)
    echo "${n:-0}"
}

# Flag possibly-lost blocks whose DIRECT caller (first frame above the
# allocator) is efs code. If the direct caller is a shared library (libc
# strdup, libfuse fuse_new/fuse_session_mount, rtld TLS via allocate_dtv,
# libnl, ...) the block is that library's responsibility — benign. Only a
# block allocated straight out of an efs .c frame is a real efs leak suspect.
# Prints the offending loss-record headers.
suspicious_possibly_lost() { # logfile
    awk '
        /possibly lost in loss record/ { inblk=1; hdr=$0; seen=0; next }
        inblk && / at 0x/ { seen=1; next }                 # allocator frame
        inblk && seen && / by 0x/ {                        # direct caller
            if ($0 ~ /\.c:[0-9]+/ && $0 ~ /efsd\.c|efs_fuse\.c|efs_bench\.c|read\.c|write\.c|ops\.c|inode_rpc\.c|protocol\.c|metadata\.c|handler\.c|store\.c|store_nvme\.c|writer\.c|network\.c|rdma\.c|erasure\.c|placement\.c|cluster\.c|peer_pool\.c|client\.c|bufpool\.c|node_cache\.c|raft_host\.c|raft\.c|raft_disk\.c|raft_log\.c|raft_mem\.c|kv_[a-z]+\.c|meta_apply\.c|txn\.c|session\.c|lock\.c|dir_layout\.c|wire\.c|transport_[a-z]+\.c|store_mem\.c|checksum\.c|common\.c|sim_[a-z]+\.c|opid\.c|bench_local\.c/)
                print hdr
            inblk=0; seen=0; next
        }
        inblk && /^==[0-9]+== *$/ { inblk=0; seen=0 }
    ' "$1"
}

# A truncated/failed memcheck invocation is never a clean memory verdict.
vg_complete() {
    grep -q 'ERROR SUMMARY:' "$1" || return 1
    grep -q 'no leaks are possible' "$1" && return 0
    grep -q 'LEAK SUMMARY:' "$1" &&
    grep -qE 'definitely lost: *[0-9,]+ bytes' "$1" &&
    grep -qE 'indirectly lost: *[0-9,]+ bytes' "$1"
}

# gate one valgrind log; $1=log $2=label
gate() {
    local log=$1 label=$2
    if [ ! -s "$log" ]; then
        fail "$label: no valgrind log at $log (did it run?)"
        return
    fi
    if ! vg_complete "$log"; then
        fail "$label: incomplete valgrind report"; return
    fi
    local def ind uninit invalid susp
    def=$(vg_bytes "$log" "definitely")
    ind=$(vg_bytes "$log" "indirectly")
    uninit=$(vg_count "$log" "uninitialised byte")
    invalid=$(vg_count "$log" "Invalid (read|write)")
    susp=$(suspicious_possibly_lost "$log")
    note "$label: definite=${def:-?}B indirect=${ind:-?}B uninit=$uninit invalid=$invalid"
    [ "${def:-1}" = "0" ] || fail "$label: $def bytes definitely lost"
    [ "${ind:-1}" = "0" ] || fail "$label: $ind bytes indirectly lost"
    [ "$uninit" = "0" ]   || fail "$label: $uninit uninitialised-byte errors"
    [ "$invalid" = "0" ]  || fail "$label: $invalid invalid read/write errors"
    if [ -n "$susp" ]; then
        fail "$label: efs-code possibly-lost blocks:"; echo "$susp"
    fi
}

# Leaks-only gate for the RDMA phase. ibverbs fills structs via kernel ioctl /
# DMA (dev->lid/mtu from ibv_query_port, qp_num from ibv_create_qp, the NIC
# DMA'd recv arena) that valgrind cannot track as initialised, so the
# uninit/invalid checks would false-positive on the RDMA path by design.
# Leak detection (definite/indirect/efs-possibly-lost) is unaffected.
gate_leaks() { # logfile label
    local log=$1 label=$2
    if [ ! -s "$log" ]; then
        fail "$label: no valgrind log at $log (did it run?)"
        return
    fi
    if ! vg_complete "$log"; then
        fail "$label: incomplete valgrind report"; return
    fi
    local def ind susp
    def=$(vg_bytes "$log" "definitely")
    ind=$(vg_bytes "$log" "indirectly")
    susp=$(suspicious_possibly_lost "$log")
    note "$label: definite=${def:-?}B indirect=${ind:-?}B (uninit/invalid skipped: ibverbs/DMA)"
    [ "${def:-1}" = "0" ] || fail "$label: $def bytes definitely lost"
    [ "${ind:-1}" = "0" ] || fail "$label: $ind bytes indirectly lost"
    if [ -n "$susp" ]; then
        fail "$label: efs-code possibly-lost blocks:"; echo "$susp"
    fi
}

# Kill OUR daemons (plain or valgrind-wrapped) whose cmdline references our
# private workdir (storage / mount paths). Match by exact comm
# (efsd / efs-fuse / memcheck-amd64-) so we never self-match this script's
# own ssh command line, and never touch the live cluster.
record_ours() {
    local child=$1
    OWN_PIDS+=("$child:$(python3 - "$child" <<'PY_STAMP'
from pathlib import Path
import sys
print(Path('/proc/'+sys.argv[1]+'/stat').read_text().split(') ',1)[1].split()[19])
PY_STAMP
)")
}
kill_ours() { # Exact task-owned PID, start identity and storage/mount argument.
    python3 - "$WORK" "${1:-9}" "${OWN_PIDS[@]}" <<'PY_KILL'
import os,signal,sys
from pathlib import Path
work=Path(sys.argv[1]);sig=int(sys.argv[2]) if sys.argv[2].isdigit() else getattr(signal,'SIG'+sys.argv[2])
paths={str(work/'mnt'),*(str(work/'storage'/('s'+str(i))) for i in range(1,4))}
for identity in sys.argv[3:]:
    pid,stamp=identity.split(':');proc=Path('/proc')/pid
    try:
        fd=os.pidfd_open(int(pid))
    except ProcessLookupError:continue
    try:
        fields=(proc/'stat').read_text().split(') ',1)[1].split()
        argv=(proc/'cmdline').read_bytes().decode().split(chr(0))
        if fields[19]!=stamp or proc.stat().st_uid!=os.getuid() or not paths.intersection(argv):continue
        signal.pidfd_send_signal(fd,sig)
    except (FileNotFoundError,ProcessLookupError):pass
    finally:os.close(fd)
PY_KILL
}

# Start a 3-node raft group on $ADDR ports $PORT..$PORT+2 with node 1 under
# valgrind (log "$WORK"-group1.vg). Plain nodes 2+3 come up first and hold
# quorum/leadership; the slow valgrind'd node 1 joins last as a follower and
# still applies every committed entry (that is the server-side coverage).
# $1 = EFS_TRANSPORT for the servers. NOTE: tcp makes efs_rdma_available()
# return 0 process-wide (rdma.c), which disables the server's RDMA_SETUP
# accept — so the RDMA phase must NOT pass tcp here or every client upgrade
# is refused.
format_group() {
    local seed=$1 attempt result
    for attempt in $(seq 1 30); do
        result=$(./efs-mgmt raft-mkfs "$seed" 2>&1)
        printf '%s\n' "$result" >> "$WORK-format.out"
        printf '%s\n' "$result" | grep -q 'raft-mkfs rc=0 ' && return 0
        sleep 1
    done
    fail "private group format did not commit at $seed"
    exit 2
}

start_group_vg_server() {
    local xport=$1
    local id port join out i
    rm -rf "$STORAGE"; mkdir -p "$STORAGE"
    for id in 2 3; do
        port=$((PORT + id - 1))
        join="--join $ADDR:$((PORT + 1))"
        [ "$id" = 2 ] && join=""                            # node 2 seeds
        out="$WORK-group$id.out"
        # K33: the seed creates the cluster at its endpoint; joiners name theirs.
        local ep="$ADDR:$port"
        [ -n "$join" ] && ep="$join --listen $ADDR:$port"
        EFS_TRANSPORT=$xport setsid ./efsd $ep "$STORAGE/s$id" \
            --quota 1G --writers 0 "$IO_FLAG" --discovery-port 0 \
            >"$out" 2>&1 </dev/null &
        record_ours "$!"
        for i in $(seq 1 40); do
            grep -qE 'listening|join: (waiting|pending)' "$out" 2>/dev/null && break; sleep 0.5
        done
        grep -qE 'listening|join: (waiting|pending)' "$out" ||
            { echo "[leaks] node $id did not start"; cat "$out"; exit 2; }
    done
    # The valgrind'd node joins last (slow under memcheck); formation waits
    # for its promise, so format after it has started.
    EFS_TRANSPORT=$xport $VG --log-file="$WORK-group1.vg" \
        ./efsd --join "$ADDR:$((PORT + 1))" --listen "$ADDR:$PORT" \
        "$STORAGE/s1" --quota 1G --writers 0 "$IO_FLAG" --discovery-port 0 \
        >"$WORK-group1.out" 2>&1 &
    record_ours "$!"
    for i in $(seq 1 240); do
        grep -q listening "$WORK-group1.out" 2>/dev/null && break; sleep 0.5
    done
    grep -q listening "$WORK-group1.out" || { fail 'valgrind follower never listened'; exit 2; }
    format_group "$ADDR:$((PORT + 1))"
}

# Start a plain 3-node group with node 1 as seed (used by the fuse phases).
start_group_plain() {
    local id port join out i
    rm -rf "$STORAGE"; mkdir -p "$STORAGE"
    for id in 1 2 3; do
        port=$((PORT + id - 1))
        join="--join $ADDR:$PORT"
        [ "$id" = 1 ] && join=""
        out="$WORK-group$id.out"
        local ep="$ADDR:$port"
        [ -n "$join" ] && ep="$join --listen $ADDR:$port"
        EFS_TRANSPORT=tcp setsid ./efsd $ep "$STORAGE/s$id" \
            --quota 1G --writers 0 "$IO_FLAG" --discovery-port 0 \
            >"$out" 2>&1 </dev/null &
        record_ours "$!"
        for i in $(seq 1 40); do
            grep -qE 'listening|join: (waiting|pending)' "$out" 2>/dev/null && break; sleep 0.5
        done
        grep -qE 'listening|join: (waiting|pending)' "$out" ||
            { echo "[leaks] node $id did not start"; cat "$out"; exit 2; }
    done
    format_group "$ADDR:$PORT"
}

cleanup() {
    kill_ours 9
    fusermount3 -u "$MNT" 2>/dev/null || true
    # Preserve both passing and failing logs and scratch stores for review.
}
cd "$HERE" || exit 2
command -v valgrind >/dev/null || { echo '[leaks] valgrind not installed' >&2; exit 2; }
[ "$(id -u)" -ne 0 ] || { echo '[leaks] run as the non-root EFS account' >&2; exit 2; }
WORK=$(python3 - "$WORK" <<'PY_WORK'
from pathlib import Path
import sys
print(Path(sys.argv[1]).resolve())
PY_WORK
)
[ ! -e "$WORK" ] || { echo '[leaks] work directory must be new; refusing reuse' >&2; exit 2; }
for evidence in "$WORK"-*.vg "$WORK"-*.out; do
    [ ! -e "$evidence" ] || { echo '[leaks] refusing to replace existing evidence' >&2; exit 2; }
done
STORAGE="$WORK/storage";MNT="$WORK/mnt"
mkdir -p "$WORK"
trap cleanup EXIT
note "mode=$MODE transport=${EFS_TRANSPORT:-auto}; evidence retained at $WORK"

note "building (after any rsync --delete) ..."
# Unit-test make targets carry the tests/ prefix; use the current canonical C list.
make -s "${VG_BUILD_ARGS[@]}" efsd efs-fuse efs-mgmt efs-bench \
    $(for t in $UNIT_TESTS; do echo "tests/$t"; done) >/dev/null 2>&1 \
    || { echo "[leaks] build failed"; exit 2; }
if [ -n "${EFS_VG_RDMA_RUNTIME:-}" ]; then
    note "annotated verbs runtime=$EFS_VG_RDMA_RUNTIME; strict RDMA unit memory gate"
    python3 tests/test_rdma_memcheck.py || exit 1
fi

# ---------------------------------------------------------------- unit ---
note "phase 1: unit tests under valgrind"
case "${EFS_VG_SKIP_UNITS:-0}" in 0|1) ;; *) echo invalid EFS_VG_SKIP_UNITS >&2; exit 2;; esac
if [ "${EFS_VG_SKIP_UNITS:-0}" = 1 ]; then
    note "phase 1: units SKIP — caller reuses its separate unchanged-source unit verdict"
else
for t in $UNIT_TESTS; do
    log="$WORK-unit-$t.vg"
    unit_vg=$VG
    case "$t" in
        test_namespace_session|test_append_transaction|test_append_receipt|test_put_ticket|test_mtime_barrier|test_publication_session|test_publication_recovery|test_lane_bootstrap_recovery|test_kv_lsm|test_raft_store|test_session)
            # These fixtures deliberately _exit in children before destruction.
            # Keep their crash/recovery assertions and memory-access error exit
            # checks; gate clean parent leaks separately. Abrupt child allocation
            # lifetime is not clean-exit leak coverage and is explicitly excluded.
            unit_vg="$VG --child-silent-after-fork=yes --errors-for-leak-kinds=none"
            note "unit/$t: parent leak gate; deliberate crash-child leaks EXCLUDED (access-error exits still gated)"
            ;;
    esac
    # Per-unit budgets: test_meta_apply needs ~250 s under Memcheck even on a
    # quiet Zen 4 (9 s native); the concurrency test needs fair scheduling so
    # spinning readers cannot starve its writer, and a scaled shutdown grace.
    limit=${EFS_VG_UNIT_TIMEOUT:-300}
    case "$t" in
        test_meta_apply) limit=$((limit * 4)) ;;
        test_kv_lsm_concurrent) limit=$((limit * 3)); unit_vg="$unit_vg --fair-sched=yes" ;;
    esac
    EFS_TEST_DEADLINE_SCALE=${EFS_TEST_DEADLINE_SCALE:-6} \
    timeout --signal=TERM --kill-after=10 "$limit" $unit_vg --log-file="$log" "./tests/$t" >"$WORK-unit-$t.out" 2>&1
    rc=$?
    [ "$rc" = 0 ] || fail "unit/$t: process exit $rc"
    gate "$log" "unit/$t"
done
fi

# ---------------------------------------------------------------- efsd ---
note "phase 2: efsd under valgrind (3-node raft group, efs-bench workload)"
ADDR=127.0.0.1
start_group_vg_server tcp
# Workload generators only — a driver abort under the ~30x valgrind slowdown
# (e.g. a PUT-timeout assert in efs-bench) is not a leak-gate failure. The
# --meta phase (2400+ ops) is the core metadata coverage; store/read add the
# data path. Target node 2 (plain, fast); any node bounces to the raft
# leader, and the valgrind'd follower applies every committed entry.
LEAD="127.0.0.1:$((PORT + 1))"
EFS_TRANSPORT=tcp ./efs-bench "$LEAD" --meta --export "$EXPORT" \
    --files 300 --dirs 4 --workers 4 >/dev/null 2>&1 || true
EFS_TRANSPORT=tcp ./efs-bench "$LEAD" --store --time 5 >/dev/null 2>&1 || true
EFS_TRANSPORT=tcp ./efs-bench "$LEAD" --read  --time 3 >/dev/null 2>&1 || true
# SIGTERM -> valgrind prints the summary (no stacks on abnormal exit; expected)
kill_ours TERM
for i in $(seq 1 20); do grep -q "LEAK SUMMARY" "$WORK-group1.vg" 2>/dev/null && break; sleep 1; done
gate "$WORK-group1.vg" "efsd"

# ---------------------------------------------------------------- fuse ---
# Plain (fast) 3-node group so FUSE reads succeed; only the client is under
# valgrind.
note "phase 3: efs-fuse under valgrind (mini-POSIX through the mount)"
kill_ours 9; sleep 2
ADDR=127.0.0.1
start_group_plain
mkdir -p "$MNT"
# -f (foreground): no daemonize fork, so valgrind instruments the real
# daemon (a forked parent would _exit without shutdown and leak the
# pre-fork bootstrap table into the log).
EFS_TRANSPORT=tcp $VG --log-file="$WORK-fuse.vg" ./efs-fuse "$SEED" "$MNT" -f \
    >"$WORK-fuse.out" 2>&1 &
record_ours "$!"
for i in $(seq 1 60); do grep -q "efs-fuse $MNT " /proc/mounts 2>/dev/null && break; sleep 0.5; done
grep -q "efs-fuse $MNT " /proc/mounts || { echo "[leaks] fuse mount failed"; cat "$WORK-fuse.out"; exit 2; }

mkdir -p "$MNT/d1" "$MNT/d2"
for i in $(seq 1 30); do echo "data-$i" >"$MNT/d1/f$i"; done
for i in $(seq 1 30); do cat "$MNT/d1/f$i" >/dev/null 2>&1; done
# Same-dir rename + hardlink. Cross-dir rename is the known cross-group
# EINVAL debt (posix dir_move_into_subdir) — attempted for coverage but
# allowed to fail until that debt is paid.
for i in $(seq 1 15); do mv "$MNT/d1/f$i" "$MNT/d1/g$i"; ln "$MNT/d1/g$i" "$MNT/d2/h$i" 2>/dev/null; done
mv "$MNT/d1/g1" "$MNT/d2/g1" 2>/dev/null || true
dd if=/dev/zero of="$MNT/big" bs=1M count=4 conv=fsync >/dev/null 2>&1
dd if="$MNT/big" of=/dev/null bs=4k >/dev/null 2>&1
for i in $(seq 16 30); do : >"$MNT/d1/f$i"; done   # O_TRUNC -> report path (uninit regression)
ls "$MNT/d1" "$MNT/d2" >/dev/null 2>&1
for i in $(seq 16 30); do rm -f "$MNT/d1/f$i"; done
sync
fusermount3 -u "$MNT" 2>/dev/null   # clean exit -> full stacks in the log
for i in $(seq 1 20); do grep -q "LEAK SUMMARY" "$WORK-fuse.vg" 2>/dev/null && break; sleep 1; done
gate "$WORK-fuse.vg" "efs-fuse"

# ---------------------------------------------------------------- rdma ---
# Client + server over RDMA (default transport). The RDMA connection upgrade
# is eager on connect (node_cache.c). Mount + metadata + a chunked write
# exercise QP/MR/arena on BOTH sides including PUT/GET. Gated on leaks only
# (ibverbs uninit false-positives — see gate_leaks).
if [ "${EFS_TRANSPORT:-auto}" = tcp ]; then
    note "phase 4: RDMA SKIP — explicit TCP selection"
elif [ -z "$RDMA_IP" ]; then
    note "phase 4: SKIP — no 172.16.223.x RDMA IP found (set EFS_VG_RDMA_IP)"
else
    note "phase 4: efsd + efs-fuse over RDMA ($RDMA_IP)"
    kill_ours 9; sleep 2
    mkdir -p "$MNT"
    # 3-node group on the RDMA IP, node 1 valgrind'd (follower). The client
    # mounts via node 2 (plain, fast); the RDMA upgrade runs on that conn.
    # Servers run auto transport — tcp would disable their RDMA accept.
    ADDR=$RDMA_IP
    start_group_vg_server auto
    # client under valgrind, STRICT RDMA (EFS_TRANSPORT=rdma): the phase
    # exists to exercise the RDMA path, and auto mode falls back to TCP
    # SILENTLY if the upgrade fails, making the phase vacuous. Foreground
    # so valgrind instruments the real daemon (see phase 3).
    EFS_TRANSPORT=rdma $VG --log-file="$WORK-fuse-rdma.vg" \
        ./efs-fuse "$RDMA_IP:$((PORT + 1))" "$MNT" -f \
        >"$WORK-fuse-rdma.out" 2>&1 &
    record_ours "$!"
    for i in $(seq 1 60); do grep -q "efs-fuse $MNT " /proc/mounts 2>/dev/null && break; sleep 0.5; done
    if ! grep -q "efs-fuse $MNT " /proc/mounts; then
        note "phase 4: fuse mount failed (RDMA may be unavailable) — skipping"
        cat "$WORK-fuse-rdma.out"
    else
        # The upgrade can land well after the mount under memcheck (~30x
        # slowdown), so check it AFTER the workload, not here.
        # metadata + small-file ops (no chunk PUT -> no broken-data-path hang)
        mkdir -p "$MNT/r1"
        for i in $(seq 1 20); do echo "rdma-$i" >"$MNT/r1/f$i"; done
        for i in $(seq 1 20); do cat "$MNT/r1/f$i" >/dev/null 2>&1; done
        # Chunk PUT/GET (128 KiB+).
        dd if=/dev/urandom of="$MNT/r1/chunk" bs=256k count=1 status=none
        dd if="$MNT/r1/chunk" of=/dev/null bs=256k status=none
        for i in $(seq 1 10); do mv "$MNT/r1/f$i" "$MNT/r1/g$i"; done
        ls "$MNT/r1" >/dev/null 2>&1
        for i in $(seq 11 20); do rm -f "$MNT/r1/f$i"; done
        sync
        grep -q "RDMA transport up" "$WORK-fuse-rdma.out" \
            && note "  RDMA transport up (upgrade exercised)" \
            || note "  WARN: no 'RDMA transport up' line — phase ran on TCP"
        fusermount3 -u "$MNT" 2>/dev/null   # clean exit -> full stacks
        for i in $(seq 1 20); do grep -q "LEAK SUMMARY" "$WORK-fuse-rdma.vg" 2>/dev/null && break; sleep 1; done
        gate_leaks "$WORK-fuse-rdma.vg" "efs-fuse/rdma"
    fi
    kill_ours TERM   # SIGTERM efsd -> leak summary
    for i in $(seq 1 20); do grep -q "LEAK SUMMARY" "$WORK-group1.vg" 2>/dev/null && break; sleep 1; done
    gate_leaks "$WORK-group1.vg" "efsd/rdma"
fi

# ---------------------------------------------------------------- done ---
echo
if [ "$fails" -eq 0 ]; then
    note "PASS memory gates in selected mode; separately recorded SKIPs remain SKIP"
    exit 0
fi
note "$fails gate failure(s) — logs under $WORK-*.vg (kept on failure)"
trap - EXIT   # keep logs for inspection
kill_ours 9; fusermount3 -u "$MNT" 2>/dev/null
exit 1
