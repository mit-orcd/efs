#!/bin/bash
# `make tsan-check` / `make asan-check`: the F1 concurrency and memory gates
# over a copy of the tree built with ThreadSanitizer or with
# AddressSanitizer + UndefinedBehaviorSanitizer (+ LeakSanitizer where
# ptrace allows it).
#
# Usage: bash tests/sanitizer_check.sh tsan|asan
#
# Output: one line per entry, then one verdict line.
#   PASS <name> (<seconds>s)
#   FAIL <name>: <reason> (<seconds>s)   reason = <n> ThreadSanitizer reports /
#                                        <n> AddressSanitizer errors / <n> leaks /
#                                        <n> UBSan errors / test rc / timeout
#   VERDICT: PASS|FAIL (<n> run, <f> failed, <s>s)
# Exit: 0 PASS, 1 FAIL, 3 NOTRUN (the compiler cannot build or run a
# sanitized program: runtime missing), 2 usage error. A sanitizer report in
# any process of an entry (unit binary, Python harness binary, efsd,
# efs-fuse, efs-mgmt) fails that entry: every report is written through
# log_path to its own file, so nothing is lost in a daemon's log. No entry
# PASSes without the sanitized tree having been built in this run (or
# EFS_SAN_BUILD naming one), and the gate never exits 0 without a run.
#
# Environment:
#   EFS_SAN_ONLY      space-separated names to run (default: whole list)
#   EFS_SAN_LIST      file with "name|command" lines replacing the list,
#                     run in place without a build (tests/test_sanitizer_gate.py)
#   EFS_SAN_BUILD     an existing tree already built with this mode's flags
#                     (skips the copy and build; for reruns)
#   EFS_SAN_TIMEOUT   per-entry timeout in seconds (default 3600)
#   EFS_SAN_LOGDIR    where reports and the build go (default: fresh under
#                     $TMPDIR, deleted on PASS, kept on FAIL)
#   EFS_SAN_JOBS      build parallelism (default nproc)
#   CC                compiler (default gcc as the Makefile)
#
# Commands may start with VAR=value words (environment for that entry). A
# command whose program ends in .py runs under python3 with the harness
# variables set: EFS_TEST_TSAN=1 (tsan) or EFS_TEST_SANITIZE=1 and
# EFS_SANITIZE=1 (asan), which the Python-driven C harnesses read
# (tests/sanitize_env.py and the older per-test switches), and the
# Makefile-built binaries they link or start come from the sanitized tree.
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE" || exit 2
MODE="${1:-}"
case "$MODE" in tsan|asan) ;; *) echo "usage: $0 tsan|asan" >&2; exit 2;; esac

CC="${CC:-gcc}"
TIMEOUT="${EFS_SAN_TIMEOUT:-3600}"
TO=()
if command -v timeout >/dev/null 2>&1; then TO=(timeout -k 10 "$TIMEOUT"); fi   # absent on macOS
LOGDIR="${EFS_SAN_LOGDIR:-}"
if [ -z "$LOGDIR" ]; then
    LOGDIR="$(mktemp -d "${TMPDIR:-/tmp}/efs-$MODE-check.XXXXXX")" || exit 2
    OWN_LOGDIR=1
else
    mkdir -p "$LOGDIR" || exit 2
    OWN_LOGDIR=0
fi

case "$MODE" in
tsan)
    SAN_CFLAGS="-fsanitize=thread"
    SAN_LDFLAGS="-fsanitize=thread"
    # gcc diagnoses atomic_thread_fence under TSan (W138); the Makefile has
    # no -Werror, so the build only warns. Keep -O3 -g from CFLAGS.
    ;;
asan)
    # Baseline ISA, as the leak gate: with -march=native on an AVX-512 host
    # gcc emits 64-byte-aligned vector stack accesses (BLAKE3's make_output
    # and compress_subtree_to_parent_node), and ASan's fake stack for
    # detect_stack_use_after_return does not give that alignment, so every
    # hashing binary died with SEGV at a wild address (xorinox, gcc 14.3).
    # EFS_SAN_ARCH overrides; an empty value keeps the Makefile's -march.
    case "$(uname -m)" in
        aarch64|arm64) arch_default="-march=armv8.2-a -mtune=generic" ;;
        x86_64)        arch_default="-march=x86-64-v3 -mtune=generic" ;;
        *)             arch_default="" ;;
    esac
    SAN_CFLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer ${EFS_SAN_ARCH-$arch_default}"
    SAN_LDFLAGS="-fsanitize=address,undefined"
    # LeakSanitizer runs where it exists (Linux; it needs ptrace_scope < 2).
    case "$(uname -s)" in Linux) LSAN="detect_leaks=1" ;; *) LSAN="" ;; esac
    ;;
esac

# Toolchain probe: a sanitized program must build and run, or the gate is
# NOTRUN (exit 3), never a pass. The probe has a thread so a missing
# libtsan/libasan runtime shows at link or at start.
probe="$LOGDIR/probe"
cat >"$probe.c" <<'PEOF'
#include <pthread.h>
#include <stdlib.h>
static void *t(void *a) { return a; }
int main(void) { pthread_t th; char *p = malloc(8); if (!p) return 2;
    if (pthread_create(&th, 0, t, p)) return 3; pthread_join(th, 0); free(p); return 0; }
PEOF
if ! $CC -g -O1 $SAN_LDFLAGS -pthread "$probe.c" -o "$probe" >"$probe.log" 2>&1 || ! "$probe" >>"$probe.log" 2>&1; then
    echo "NOTRUN: $MODE runtime missing ($CC $SAN_LDFLAGS cannot build or run a program; $probe.log)"
    exit 3
fi

default_list_tsan() {
cat <<'LEOF'
# KV: the W126 reader/writer/peek stress, the LSM (flush, compaction, peeks,
# the reader gate), the KV snapshot tail; Raft store and reclaim threads.
telemetry|tests/test_telemetry
kv_lsm_concurrent|tests/test_kv_lsm_concurrent 4
kv_lsm|tests/test_kv_lsm
# W123 ports: MANIFEST order at load; a real fold and flush overlap through SIGKILL and reopen.
kv_manifest_order|tests/test_kv_manifest_order
kv_fold_reopen|tests/test_kv_fold_reopen
kv_reader_gate|tests/test_kv_reader_gate
raft_store|tests/test_raft_store
# Task EM: sequence binaries whose stores start KV compactor threads.
meta_apply|tests/test_meta_apply
append_transaction|tests/test_append_transaction
session|tests/test_session
reclaim|tests/test_reclaim
query_disk_io|tests/test_query_disk_io
query_delta|EFS_QUERY_DELTA_PARENTS=64 EFS_QUERY_DELTA_LANES=1 EFS_QUERY_DELTA_MULTI_LANES=1 tests/test_query_delta
# Client write path (data commit workers) and the member view readers.
data_commit|tests/test_data_commit
member_view|tests/test_member_view
# Task DM: the real write-back pool (workers, drains, stop) through writeback.h.
wb_interface|tests/test_wb_interface
# Task EB: the real open-reference table (rows, retained closes, the retry pass) through open.h.
open_interface|tests/test_open_interface
# Task EC: the real .find result cache (per-caller results, slots, byte budget, fetch limit) through find_res.h.
find_res_interface|tests/test_find_res_interface
# Task U1: the real PUT window (counts, bounded wait, overflow, concurrent churn) through put_window.h.
put_window_interface|tests/test_put_window_interface
# Task V3: the real group view (publish, coherent reader race, predicates, wait bracket) through group_view.h.
group_view_interface|tests/test_group_view
# Task V4: the real read-proof owner (admission, request, wait, leadership loss) through read_proof.h.
read_proof_interface|tests/test_read_proof
# Task V1: every exit of a flusher closes its PUT window (real put_window.c, stubbed cache).
put_window_callers_ok|tests/test_put_window_callers.py ok
put_window_callers_fail|tests/test_put_window_callers.py fail
put_window_callers_retired|tests/test_put_window_callers.py retired
put_window_callers_drain|tests/test_put_window_callers.py drain
put_window_callers_nomem|tests/test_put_window_callers.py nomem
put_window_callers_fetch|tests/test_put_window_callers.py fetch
put_window_callers_fetch_retired|tests/test_put_window_callers.py fetch_retired
# Harnesses that build their own TSan binary (W138 rules: run or fail).
apply_ring|tests/test_apply_ring.py
session_latch|tests/test_session_latch_tsan.py
heartbeat_shutdown|tests/test_heartbeat_shutdown.py
# E2 parallel revoke, W155 receipt table, K32 ticket batches on the TSan libefs.a.
revoke_parallel|tests/test_session_revoke_parallel.py
receipt_table|tests/test_w155_receipt_table.py
put_ticket_batch|tests/test_put_ticket_batch.py
# K109: the storage fence's detached per-member workers (counting rule, slow member).
storage_fence_members|tests/test_storage_fence_members.py
waiter_cut_halted|tests/test_node_waiter_cut.py halted
waiter_cut_slow|tests/test_node_waiter_cut.py slow
waiter_cut_two|tests/test_node_waiter_cut.py two
waiter_cut_noreply|tests/test_node_waiter_cut.py noreply
waiter_cut_deadline|tests/test_node_waiter_cut.py deadline
waiter_cut_race|tests/test_node_waiter_cut.py race
waiter_cut_gating|tests/test_node_waiter_cut.py gating
# Daemons: K36 recovery thread, K37 segment rebuild and audit, and the
# client read and write paths through efs-fuse under load (D4, short), and
# the K38 swept-hint STALE replay through efs-fuse, and the K88 per-chunk
# CAS hints through the library writer, and the K96 STATUS slot class
# under 24 concurrent library writers.
k36_recovery|tests/test_k36_recovery.py
k36_segment_recovery|tests/test_k36_segment_recovery.py
k110_barrier_history|tests/test_k110_barrier_history.py
k37_segment_rebuild|tests/test_k37_segment_rebuild.py
d4_progress|EFS_D4_LOAD_S=60 EFS_D4_THREADS=4 tests/test_d4_progress.py
k38_swept_hint_live|tests/test_k38_swept_hint_live.py
k88_chunk_hints_live|tests/test_k88_chunk_hints_live.py
k96_status_slots_live|K96_ROUNDS=1 tests/test_k96_status_slots_live.py
# K109: a clean unmount past a stalled member (detached fence workers in a live daemon).
k109_stalled_member|tests/test_k109_stalled_member_unmount.py
# K93: F14 (16 members, a refused 17th joiner): the rejoin thread's HELLO
# reads the member-store identity while members learn new revisions.
member_limit|tests/test_member_limit.py
# AB/AE: pump diagnostics through the reporter, blocked sink, role events, unmount.
pump_logging|tests/test_pump_logging.py
# K79: SIGTERM with stderr a full pipe exits rc 0 within the stop bound.
stop_blocked_stderr|tests/test_stop_blocked_stderr.py
LEOF
}

default_list_asan() {
cat <<'LEOF'
# The four tests that always built an ASan binary of their own.
readindex_pair|tests/test_readindex_pair.py
raft_ack_window|tests/test_raft_ack_window.py
host_outbox|tests/test_host_outbox.py
heartbeat_shutdown|tests/test_heartbeat_shutdown.py
rdma_handler_alignment|tests/test_rdma_handler_alignment.py
stage_evict_lru|tests/test_stage_evict_lru.py
# Harnesses with an ASan switch (EFS_TEST_SANITIZE / EFS_SANITIZE / --sanitize).
address_parser|tests/test_address_parser.py --sanitize
rollup_budget|tests/test_rollup_budget.py --sanitize
rollup_allocation_failure|tests/test_rollup_allocation_failure.py --sanitize
client_memory|tests/test_client_memory.py --sanitize
append_cohort|tests/test_append_cohort.py
applied_interval|tests/test_applied_interval.py
checkpoint_worker|tests/test_checkpoint_worker.py
cursor_checkpoint|tests/test_cursor_checkpoint.py
host_inbox|tests/test_host_inbox.py
inode_lock_stripes|tests/test_inode_lock_stripes.py
opendir_authority_errno|tests/test_opendir_authority_errno.py
permission_groups|tests/test_permission_groups.py
recorder_start|tests/test_recorder_start.py
session_combined_group|tests/test_session_combined_group.py
report_snapshot_locks|tests/test_report_snapshot_locks.py
resolver_deadline|tests/test_resolver_deadline.py
receipt_table|tests/test_w155_receipt_table.py
identified_resolve|tests/test_w155_identified_resolve.py
report_resolve|tests/test_w155_report_resolve.py
revoke_parallel|tests/test_session_revoke_parallel.py
put_ticket_batch|tests/test_put_ticket_batch.py
# K109: the storage fence's detached per-member workers (counting rule, slow member).
storage_fence_members|tests/test_storage_fence_members.py
# New parsers and decoders: the K33 membership record and discovery
# datagrams, the member view, the fragment record, small and packed
# segments and their reader, the pack ledger, append receipts, put tickets
# and orphans, the client data commit, the retained-namespace audit, wire.
membership|tests/test_membership
member_view|tests/test_member_view
# Task DM: the real write-back pool (workers, drains, stop) through writeback.h.
wb_interface|tests/test_wb_interface
# Task EB: the real open-reference table (rows, retained closes, the retry pass) through open.h.
open_interface|tests/test_open_interface
# Task EC: the real .find result cache (per-caller results, slots, byte budget, fetch limit) through find_res.h.
find_res_interface|tests/test_find_res_interface
# Task U1: the real PUT window (counts, bounded wait, overflow, concurrent churn) through put_window.h.
put_window_interface|tests/test_put_window_interface
# Task V3: the real group view (publish, coherent reader race, predicates, wait bracket) through group_view.h.
group_view_interface|tests/test_group_view
# Task V4: the real read-proof owner (admission, request, wait, leadership loss) through read_proof.h.
read_proof_interface|tests/test_read_proof
# Task V1: every exit of a flusher closes its PUT window (real put_window.c, stubbed cache).
put_window_callers_ok|tests/test_put_window_callers.py ok
put_window_callers_fail|tests/test_put_window_callers.py fail
put_window_callers_retired|tests/test_put_window_callers.py retired
put_window_callers_drain|tests/test_put_window_callers.py drain
put_window_callers_nomem|tests/test_put_window_callers.py nomem
put_window_callers_fetch|tests/test_put_window_callers.py fetch
put_window_callers_fetch_retired|tests/test_put_window_callers.py fetch_retired
frag_record|tests/test_frag_record
small_segment|tests/test_small_segment
packed_segment|tests/test_packed_segment
packed_reader|tests/test_packed_reader
pack_ledger|tests/test_pack_ledger
pack_ledger_recovery|tests/test_pack_ledger_recovery
append_receipt|tests/test_append_receipt
put_ticket|tests/test_put_ticket
put_orphan|tests/test_put_orphan
put_orphan_server|tests/test_put_orphan_server
data_commit|tests/test_data_commit
retained_namespace_audit|tests/test_retained_namespace_audit
wire|tests/test_wire
kv_lsm|tests/test_kv_lsm
# W123 ports: MANIFEST order at load; a real fold and flush overlap through SIGKILL and reopen.
kv_manifest_order|tests/test_kv_manifest_order
kv_fold_reopen|tests/test_kv_fold_reopen
kv_snap_import|tests/test_kv_snap_import
snap_import_host|tests/test_snap_import_host
snap_import_pump|tests/test_snap_import_pump
raft|tests/test_raft
# Task EM: the other sequence binaries (EG case filter; raft, sim and txn
# start no thread, so they are on this list only).
raft_store|tests/test_raft_store
meta_apply|tests/test_meta_apply
append_transaction|tests/test_append_transaction
sim|tests/test_sim
txn|tests/test_txn
session|tests/test_session
# Daemons: the removed-settings refusal (A6), K36 recovery scan pages, the
# K37 segment audit and rebuild, and the D4 load (efsd and efs-fuse; LSan
# at their clean exits).
env_removed|tests/test_env_removed.py
k36_recovery|tests/test_k36_recovery.py
k36_segment_recovery|tests/test_k36_segment_recovery.py
k110_barrier_history|tests/test_k110_barrier_history.py
k37_segment_rebuild|tests/test_k37_segment_rebuild.py
d4_progress|EFS_D4_LOAD_S=60 EFS_D4_THREADS=4 tests/test_d4_progress.py
# AB/AE: pump diagnostics through the reporter, blocked sink, role events, unmount.
pump_logging|tests/test_pump_logging.py
# K109: a clean unmount past a stalled member (detached fence workers in a live daemon).
k109_stalled_member|tests/test_k109_stalled_member_unmount.py
# K79: SIGTERM with stderr a full pipe exits rc 0 within the stop bound.
stop_blocked_stderr|tests/test_stop_blocked_stderr.py
LEOF
}

if [ -n "${EFS_SAN_LIST:-}" ]; then
    LIST="$(cat "$EFS_SAN_LIST")" || exit 2
    RUNDIR="$HERE"
else
    LIST="$(default_list_$MODE)"
    if [ -n "${EFS_SAN_BUILD:-}" ]; then
        RUNDIR="$EFS_SAN_BUILD"
        [ -x "$RUNDIR/efsd" ] && [ -x "$RUNDIR/efs-fuse" ] && [ -f "$RUNDIR/libefs.a" ] ||
            { echo "VERDICT: FAIL (EFS_SAN_BUILD=$RUNDIR has no built efsd/efs-fuse/libefs.a)"; exit 1; }
        # The named tree must carry this mode's instrumentation.
        if ! nm "$RUNDIR/efsd" 2>/dev/null | grep -q "__${MODE}_init"; then
            echo "VERDICT: FAIL (EFS_SAN_BUILD=$RUNDIR: efsd is not a $MODE build)"; exit 1
        fi
    else
        RUNDIR="$LOGDIR/build"
        t0=$(date +%s)
        mkdir -p "$RUNDIR" &&
            tar -C "$HERE" -cf - Makefile include src deps tests scripts | tar -C "$RUNDIR" -xf - &&
            make -s -C "$RUNDIR" clean >/dev/null 2>&1 &&
            make -s -C "$RUNDIR" -j"${EFS_SAN_JOBS:-$(nproc 2>/dev/null || echo 4)}" \
                CC="$CC" EXTRA_DEFS="$SAN_CFLAGS" LDFLAGS="-lpthread -lm -ldl -libverbs $SAN_LDFLAGS" \
                test-bins >"$LOGDIR/build.log" 2>&1
        brc=$?
        if [ "$brc" != 0 ]; then
            tail -20 "$LOGDIR/build.log"
            echo "VERDICT: FAIL ($MODE build failed rc=$brc; log $LOGDIR/build.log)"
            exit 1
        fi
        echo "built $MODE tree with EXTRA_DEFS='$SAN_CFLAGS' ($(( $(date +%s) - t0 ))s)"
    fi
fi
cd "$RUNDIR" || exit 2

# Count the reports of one entry from its log_path files.
classify() {   # $1 name, $2 rc
    local name=$1 rc=$2 n
    if [ "$rc" = 124 ] || [ "$rc" = 137 ]; then echo "timeout after ${TIMEOUT}s"; return; fi
    case "$MODE" in
    tsan)
        n=$(cat "$LOGDIR/$name".tsan.* 2>/dev/null | grep -c '^WARNING: ThreadSanitizer:')
        [ "$n" -gt 0 ] && { echo "$n ThreadSanitizer reports"; return; } ;;
    asan)
        # Which log_path a report lands in depends on the toolchain (gcc has
        # two runtimes, clang one that takes the last parsed option set),
        # and a binary without log_path prints to stderr (.out): read all.
        n=$(cat "$LOGDIR/$name".asan.* "$LOGDIR/$name".ubsan.* "$LOGDIR/$name.out" 2>/dev/null | grep -c '^==[0-9]*==ERROR: AddressSanitizer')
        [ "$n" -gt 0 ] && { echo "$n AddressSanitizer errors"; return; }
        n=$(cat "$LOGDIR/$name".asan.* "$LOGDIR/$name".ubsan.* "$LOGDIR/$name.out" 2>/dev/null | grep -c '^==[0-9]*==ERROR: LeakSanitizer')
        [ "$n" -gt 0 ] && { echo "$n LeakSanitizer reports"; return; }
        n=$(cat "$LOGDIR/$name".asan.* "$LOGDIR/$name".ubsan.* "$LOGDIR/$name.out" 2>/dev/null | grep -c 'runtime error:')
        [ "$n" -gt 0 ] && { echo "$n UBSan errors"; return; } ;;
    esac
    if [ "$rc" != 0 ]; then echo "test rc=$rc"; return; fi
    echo ok
}

# First report of a failed entry, so the failure is actionable.
show_first() {   # $1 name
    local f
    for f in "$LOGDIR/$1".tsan.* "$LOGDIR/$1".asan.* "$LOGDIR/$1".ubsan.* "$LOGDIR/$1.out"; do
        [ -s "$f" ] || continue
        if grep -q 'WARNING: ThreadSanitizer:\|ERROR: AddressSanitizer\|ERROR: LeakSanitizer\|runtime error:' "$f"; then
            echo "--- $f"; sed -n '1,40p' "$f"; return
        fi
    done
}

run=0; failed=0; t_all=$(date +%s)
while IFS='|' read -r name cmd; do
    case "$name" in ''|'#'*) continue;; esac
    if [ -n "${EFS_SAN_ONLY:-}" ]; then
        case " $EFS_SAN_ONLY " in *" $name "*) ;; *) continue;; esac
    fi
    run=$((run + 1)); t0=$(date +%s)
    out="$LOGDIR/$name.out"
    envs=(); read -r -a words <<<"$cmd"
    while [ "${#words[@]}" -gt 0 ] && [[ ${words[0]} =~ ^[A-Za-z_][A-Za-z0-9_]*= ]]; do
        envs+=("${words[0]}"); words=("${words[@]:1}")
    done
    case "$MODE" in
    tsan)
        # die_after_fork=0: the KV, Raft and session unit tests fork
        # crash-simulation children that open a store (and so start a
        # compactor thread); TSan kills such a child by default and the
        # parent's child-status checks fail without any race.
        # detect_deadlocks=0: libtsan's lock-order detector aborts (CHECK
        # sanitizer_deadlock_detector.h:67) when one thread holds more than
        # 64 mutexes, and that abort's stack print self-deadlocks in
        # pthread_getattr_np -> realloc (gcc 14 libtsan, glibc 2.39): the
        # efs-fuse metadata fetch holds every inode lock stripe at once and
        # never mounted. Data-race detection is unaffected; lock-order
        # inversions are not reported by this gate (a stated limit).
        envs+=("TSAN_OPTIONS=log_path=$LOGDIR/$name.tsan halt_on_error=0 die_after_fork=0 detect_deadlocks=0 exitcode=66 ${TSAN_OPTIONS:-}"
               "EFS_TEST_TSAN=1") ;;
    asan)
        # verify_asan_link_order=0: the W159 slow-fsync LD_PRELOAD interposer
        # loads before libasan in the daemons that test_pump_logging starts.
        envs+=("ASAN_OPTIONS=log_path=$LOGDIR/$name.asan $LSAN detect_stack_use_after_return=1 verify_asan_link_order=0 exitcode=66 ${ASAN_OPTIONS:-}"
               "UBSAN_OPTIONS=log_path=$LOGDIR/$name.ubsan print_stacktrace=1 halt_on_error=1 exitcode=67 ${UBSAN_OPTIONS:-}"
               "EFS_TEST_SANITIZE=1" "EFS_SANITIZE=1") ;;
    esac
    case "${words[0]}" in
    *.py) prog=(python3 "${words[@]}") ;;
    *)    prog=("${words[@]}") ;;
    esac
    env "${envs[@]}" ${TO[@]+"${TO[@]}"} "${prog[@]}" >"$out" 2>&1
    rc=$?
    reason=$(classify "$name" "$rc")
    dt=$(( $(date +%s) - t0 ))
    if [ "$reason" = ok ]; then
        echo "PASS $name (${dt}s)"
    else
        failed=$((failed + 1))
        echo "FAIL $name: $reason (${dt}s)"
        show_first "$name"
        tail -5 "$out"
    fi
done <<<"$LIST"

total=$(( $(date +%s) - t_all ))
if [ "$run" -eq 0 ]; then
    echo "VERDICT: FAIL (no entries selected)"
    exit 1
fi
if [ "$failed" -eq 0 ]; then
    echo "VERDICT: PASS ($run run, 0 failed, ${total}s)"
    [ "$OWN_LOGDIR" = 1 ] && rm -rf "$LOGDIR"
    exit 0
fi
echo "VERDICT: FAIL ($run run, $failed failed, ${total}s; logs in $LOGDIR)"
exit 1
