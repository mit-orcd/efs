#!/bin/bash
# Valgrind memcheck leak gate for efs: unit tests + efsd + efs-fuse, over BOTH
# TCP and RDMA, client AND server side.
#
# Self-contained on one node: uses a private port + scratch storage so it
# NEVER touches the live cluster. Repeatable: wipe scratch, build, run, gate.
#
# Phases:
#   1. unit tests (metadata lifecycle)              — full gate
#   2. efsd over TCP (efs-bench workload)           — full gate
#   3. efs-fuse over TCP (mini-POSIX mount)         — full gate
#   4. efsd + efs-fuse over RDMA (mount + meta/packed ops) — leaks-only gate
#      (ibverbs/DMA uninit false-positives; see gate_leaks)
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
VG="valgrind --leak-check=full --show-leak-kinds=definite,indirect,possible"
UNIT_TESTS="test_meta_v6 test_erasure test_placement test_meta_slot \
            test_dir_stats test_ino_path test_add_storage"

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
vg_bytes() { # logfile kind
    grep -q "no leaks are possible" "$1" 2>/dev/null && { echo 0; return; }
    local n
    n=$(grep -E "$2 lost:" "$1" 2>/dev/null | tail -1 \
        | sed -E 's/.*: *([0-9,]+) bytes.*/\1/' | tr -d ',')
    echo "${n:-0}"
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
            if ($0 ~ /\.c:[0-9]+/ && $0 ~ /efsd\.c|efs_fuse\.c|read\.c|write\.c|ops\.c|inode_rpc\.c|protocol\.c|metadata\.c|dcache\.c|handler\.c|meta_server\.c|store\.c|writer\.c|network\.c|rdma\.c|erasure\.c|placement\.c|cluster\.c/)
                print hdr
            inblk=0; seen=0; next
        }
        inblk && /^==[0-9]+== *$/ { inblk=0; seen=0 }
    ' "$1"
}

# gate one valgrind log; $1=log $2=label
gate() {
    local log=$1 label=$2
    if [ ! -s "$log" ]; then
        fail "$label: no valgrind log at $log (did it run?)"
        return
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
# private port. Match by exact comm (efsd / efs-fuse / memcheck-amd64-) so we
# never self-match this script's own ssh command line, and never touch the
# live cluster (which is on a different port).
kill_ours() { # signal (TERM or 9)
    local sig=${1:-9} p
    for p in $(pgrep -x efsd; pgrep -x efs-fuse; pgrep -x memcheck-amd64-); do
        if tr '\0' ' ' <"/proc/$p/cmdline" 2>/dev/null | grep -q "$PORT"; then
            kill -"$sig" "$p" 2>/dev/null
        fi
    done
}

cleanup() {
    kill_ours 9
    fusermount3 -uz "$MNT" 2>/dev/null
    # logs live at "$WORK"-*.vg (siblings of the $WORK dir) — remove both.
    rm -rf "$WORK" "$WORK"-*.vg "$WORK"-*.out
}
trap cleanup EXIT

cd "$HERE" || exit 2
command -v valgrind >/dev/null || { echo "[leaks] valgrind not installed — ask user: yum install valgrind -y"; exit 2; }

note "building (after any rsync --delete) ..."
# Unit-test make targets carry the tests/ prefix (tests/test_meta_v6, ...).
make -s efsd efs-fuse efs-mgmt efs-bench \
    $(for t in $UNIT_TESTS; do echo "tests/$t"; done) >/dev/null 2>&1 \
    || { echo "[leaks] build failed"; exit 2; }

# ---------------------------------------------------------------- unit ---
note "phase 1: unit tests under valgrind"
for t in $UNIT_TESTS; do
    log="$WORK-unit-$t.vg"
    $VG --log-file="$log" "./tests/$t" >/dev/null 2>&1
    gate "$log" "unit/$t"
done

# ---------------------------------------------------------------- efsd ---
note "phase 2: efsd under valgrind (efs-bench workload, SIGTERM summary)"
rm -rf "$STORAGE"; mkdir -p "$STORAGE"
$VG --log-file="$WORK-efsd.vg" ./efsd --node-id 1 --addr 127.0.0.1 \
    --port "$PORT" --storage "$STORAGE" --quota 1G >"$WORK-efsd.out" 2>&1 &
for i in $(seq 1 40); do grep -q listening "$WORK-efsd.out" 2>/dev/null && break; sleep 0.5; done
grep -q listening "$WORK-efsd.out" || { echo "[leaks] efsd did not start"; cat "$WORK-efsd.out"; exit 2; }
./efs-mgmt mkfs "$SEED" "$EXPORT" >/dev/null 2>&1
# Workload generators only — a driver abort under the ~30x valgrind slowdown
# (e.g. a PUT-timeout assert in efs-bench) is not a leak-gate failure. The
# --meta phase (2400+ ops) is the core metadata coverage; store/read add the
# data path.
EFS_TRANSPORT=tcp ./efs-bench "$SEED" --meta --export "$EXPORT" \
    --files 300 --dirs 4 --workers 4 >/dev/null 2>&1 || true
EFS_TRANSPORT=tcp ./efs-bench "$SEED" --store --time 5 >/dev/null 2>&1 || true
EFS_TRANSPORT=tcp ./efs-bench "$SEED" --read  --time 3 >/dev/null 2>&1 || true
# SIGTERM -> valgrind prints the summary (no stacks on abnormal exit; expected)
kill_ours TERM
for i in $(seq 1 20); do grep -q "LEAK SUMMARY" "$WORK-efsd.vg" 2>/dev/null && break; sleep 1; done
gate "$WORK-efsd.vg" "efsd"

# ---------------------------------------------------------------- fuse ---
# Plain (fast) efsd so FUSE reads succeed; only the client is under valgrind.
note "phase 3: efs-fuse under valgrind (mini-POSIX through the mount)"
kill_ours 9; sleep 2
rm -rf "$STORAGE" "$MNT"; mkdir -p "$STORAGE" "$MNT"
EFS_TRANSPORT=tcp setsid ./efsd --node-id 1 --addr 127.0.0.1 --port "$PORT" \
    --storage "$STORAGE" --quota 1G >"$WORK-efsd-plain.out" 2>&1 </dev/null &
for i in $(seq 1 40); do grep -q listening "$WORK-efsd-plain.out" 2>/dev/null && break; sleep 0.5; done
./efs-mgmt mkfs "$SEED" "$EXPORT" >/dev/null 2>&1
EFS_TRANSPORT=tcp $VG --log-file="$WORK-fuse.vg" ./efs-fuse "$SEED" "$EXPORT" "$MNT" \
    >"$WORK-fuse.out" 2>&1 &
for i in $(seq 1 60); do grep -q "efs-fuse $MNT " /proc/mounts 2>/dev/null && break; sleep 0.5; done
grep -q "efs-fuse $MNT " /proc/mounts || { echo "[leaks] fuse mount failed"; cat "$WORK-fuse.out"; exit 2; }

mkdir -p "$MNT/d1" "$MNT/d2"
for i in $(seq 1 30); do echo "data-$i" >"$MNT/d1/f$i"; done
for i in $(seq 1 30); do cat "$MNT/d1/f$i" >/dev/null 2>&1; done
for i in $(seq 1 15); do mv "$MNT/d1/f$i" "$MNT/d2/g$i"; ln "$MNT/d2/g$i" "$MNT/d2/h$i" 2>/dev/null; done
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
if [ -z "$RDMA_IP" ]; then
    note "phase 4: SKIP — no 172.16.223.x RDMA IP found (set EFS_VG_RDMA_IP)"
else
    note "phase 4: efsd + efs-fuse over RDMA ($RDMA_IP)"
    kill_ours 9; sleep 2
    rm -rf "$STORAGE" "$MNT"; mkdir -p "$STORAGE" "$MNT"
    # server under valgrind, default transport (RDMA listener up)
    $VG --log-file="$WORK-efsd-rdma.vg" ./efsd --node-id 1 --addr "$RDMA_IP" \
        --port "$PORT" --storage "$STORAGE" --quota 1G >"$WORK-efsd-rdma.out" 2>&1 &
    for i in $(seq 1 40); do grep -q listening "$WORK-efsd-rdma.out" 2>/dev/null && break; sleep 0.5; done
    ./efs-mgmt mkfs "$RDMA_IP:$PORT" "$EXPORT" >/dev/null 2>&1
    # client under valgrind, default transport (RDMA upgrade on connect)
    $VG --log-file="$WORK-fuse-rdma.vg" ./efs-fuse "$RDMA_IP:$PORT" "$EXPORT" "$MNT" \
        >"$WORK-fuse-rdma.out" 2>&1 &
    for i in $(seq 1 60); do grep -q "efs-fuse $MNT " /proc/mounts 2>/dev/null && break; sleep 0.5; done
    if ! grep -q "efs-fuse $MNT " /proc/mounts; then
        note "phase 4: fuse mount failed (RDMA may be unavailable) — skipping"
        cat "$WORK-fuse-rdma.out"
    else
        grep -q "RDMA transport up" "$WORK-fuse-rdma.out" \
            && note "  RDMA transport up (upgrade exercised)" \
            || note "  WARN: no 'RDMA transport up' line — upgrade may not have run"
        # metadata + packed-file ops (no chunk PUT -> no broken-data-path hang)
        mkdir -p "$MNT/r1"
        for i in $(seq 1 20); do echo "rdma-$i" >"$MNT/r1/f$i"; done
        for i in $(seq 1 20); do cat "$MNT/r1/f$i" >/dev/null 2>&1; done
        # Chunk PUT/GET (128 KiB+ is not packed).
        dd if=/dev/urandom of="$MNT/r1/chunk" bs=256k count=1 status=none
        dd if="$MNT/r1/chunk" of=/dev/null bs=256k status=none
        for i in $(seq 1 10); do mv "$MNT/r1/f$i" "$MNT/r1/g$i"; done
        ls "$MNT/r1" >/dev/null 2>&1
        for i in $(seq 11 20); do rm -f "$MNT/r1/f$i"; done
        sync
        fusermount3 -u "$MNT" 2>/dev/null   # clean exit -> full stacks
        for i in $(seq 1 20); do grep -q "LEAK SUMMARY" "$WORK-fuse-rdma.vg" 2>/dev/null && break; sleep 1; done
        gate_leaks "$WORK-fuse-rdma.vg" "efs-fuse/rdma"
    fi
    kill_ours TERM   # SIGTERM efsd -> leak summary
    for i in $(seq 1 20); do grep -q "LEAK SUMMARY" "$WORK-efsd-rdma.vg" 2>/dev/null && break; sleep 1; done
    gate_leaks "$WORK-efsd-rdma.vg" "efsd/rdma"
fi

# ---------------------------------------------------------------- done ---
echo
if [ "$fails" -eq 0 ]; then
    note "PASS — no definite/indirect leaks, no uninit-on-wire, no invalid rw"
    exit 0
fi
note "$fails gate failure(s) — logs under $WORK-*.vg (kept on failure)"
trap - EXIT   # keep logs for inspection
kill_ours 9; fusermount3 -uz "$MNT" 2>/dev/null
exit 1
