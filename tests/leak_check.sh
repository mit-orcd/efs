#!/bin/bash
# `make leak-check`: Valgrind memcheck leak gate for the unit suite (K26).
#
# Why: LeakSanitizer needs ptrace. It aborts on hosts with
# kernel.yama.ptrace_scope >= 2 (the ORCD build nodes), so the ASan-linked
# tests run there only with ASAN_OPTIONS=detect_leaks=0. This gate checks
# leaks with memcheck instead, which needs no ptrace.
#
# Output: one line per binary, then one verdict line.
#   PASS <name> (<seconds>s)
#   FAIL <name>: <reason> (<seconds>s)      reason = definite leak / memcheck
#                                           errors / test rc / timeout / no report
#   VERDICT: PASS|FAIL (<n> run, <f> failed, <s>s)
# Exit: 0 PASS, 1 FAIL, 3 NOTRUN (valgrind missing), 2 usage error.
# Never exits 0 without valgrind having produced a complete report for
# every listed binary.
#
# Environment:
#   EFS_VALGRIND      valgrind command (default: valgrind on PATH)
#   EFS_LEAK_ONLY     space-separated names to run (default: whole list)
#   EFS_LEAK_LIST     file with "name|command" lines replacing the list
#                     (used by tests/test_leak_gate.py fixtures)
#   EFS_LEAK_TIMEOUT  per-binary timeout in seconds (default 1800)
#   EFS_LEAK_LOGDIR   where memcheck logs and the memcheck build go
#                     (default: a fresh directory under $TMPDIR, deleted
#                     on PASS, kept on FAIL)
#   EFS_LEAK_ARCH     ISA flags for the memcheck build (default below)
#
# Memcheck build: the native build (-march=native) can contain instructions
# Valgrind cannot decode (aarch64 LRCPC2 STLUR/LDAPUR on the efs1-3 VMs,
# AVX-512 on x86 nodes) and dies with SIGILL. The gate therefore copies the
# sources into its scratch directory and builds the listed binaries there
# with the same CFLAGS plus a baseline -march (EXTRA_DEFS comes last, so it
# overrides -march=native). The tree's own build is never modified.
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
cd "$HERE" || exit 2

VALGRIND="${EFS_VALGRIND:-valgrind}"
if ! command -v "$VALGRIND" >/dev/null 2>&1; then
    echo "NOTRUN: valgrind missing"
    exit 3
fi

TIMEOUT="${EFS_LEAK_TIMEOUT:-1800}"
SUPP="$HERE/tests/leak_check.supp"
LOGDIR="${EFS_LEAK_LOGDIR:-}"
if [ -z "$LOGDIR" ]; then
    LOGDIR="$(mktemp -d "${TMPDIR:-/tmp}/efs-leak-check.XXXXXX")" || exit 2
    OWN_LOGDIR=1
else
    mkdir -p "$LOGDIR" || exit 2
    OWN_LOGDIR=0
fi

# Verdict source: the tested process's own memcheck report (ERROR SUMMARY
# with --errors-for-leak-kinds=definite counts every definite leak and every
# memory error), plus the test's exit status. Not --error-exitcode: memcheck
# applies it to forked children too, and the unit tests fork crash-simulation
# children that _exit() while holding a KV/Raft handle on purpose
# (test_session, test_raft_store); memcheck turned their status 0 into the
# error code and the parent's own child-status checks failed. With
# --child-silent-after-fork those children write no report and keep their
# exit status, so the gate covers the parent process, not forked children.
vg_args() {   # $1 = log file pattern
    printf '%s\n' "$VALGRIND" --tool=memcheck --leak-check=full \
        --show-leak-kinds=definite --errors-for-leak-kinds=definite \
        --child-silent-after-fork=yes \
        --num-callers=30 --suppressions="$SUPP" --log-file="$1"
}

# The bounded list. Rationale (docs/how-it-works/testing.md, "Leak gate"):
# the four tests that were ASan-linked (their plain positive binary runs
# under memcheck via EFS_LEAK_WRAP), and the unit binaries that own the
# long-lived heap structures of the metadata path: KV (memtable, LSM,
# segments, block hash, reader gate), Raft (log, store, follower bound,
# meta apply), store/reclaim, and txn/session/lock. Client, wire and
# query binaries are covered by tests/valgrind_leaks.sh phases 2-4.
# A command may start with VAR=value words; they go into the environment
# (memcheck must run the binary itself, not /usr/bin/env). kv_lsm skips its
# peek-liveness case: memcheck's scheduling can queue the compactor's first
# lsm_lock behind the parked reader (a lock-order race, not a leak).
# F1 (Oct 25) added the unit binaries of the last two days' product code:
# the concurrent KV reader/writer/peek stress (W126), the K33 membership
# record and discovery datagram parsers (membership), the member view,
# the client data commit, put tickets and orphans, append receipts, the
# fragment record, small and packed segments, the pack ledger recovery and
# the retained-namespace audit, and the three Python-driven harnesses for
# the K32 ticket batch, the W155 receipt table and the E2 parallel revoke
# (they honour EFS_LEAK_WRAP through tests/sanitize_env.py). The K36
# recovery thread and the K37 segment rebuild run only inside efsd; their
# leak check is the ASan/LSan daemon run of `make asan-check`.
default_list() {
cat <<'EOF'
readindex_pair|py:tests/test_readindex_pair.py
raft_ack_window|py:tests/test_raft_ack_window.py
host_outbox|py:tests/test_host_outbox.py
stage_evict_lru|py:tests/test_stage_evict_lru.py
kv|tests/test_kv
kv_lsm|tests/test_kv_lsm
kv_insertion_buffer|tests/test_kv_insertion_buffer
kv_snapshot_tail|tests/test_kv_snapshot_tail
kv_seg_index|tests/test_kv_seg_index
kv_seg_bloom|tests/test_kv_seg_bloom
kv_block_hash|tests/test_kv_block_hash
kv_reader_gate|tests/test_kv_reader_gate
raft|tests/test_raft
raft_store|tests/test_raft_store
follower_bound|tests/test_follower_bound
meta_apply|tests/test_meta_apply
store_overwrite|tests/test_store_overwrite
store_durability|tests/test_store_durability
store_gc|tests/test_store_gc
reclaim|tests/test_reclaim
txn|tests/test_txn
txn_resolve_faults|tests/test_txn_resolve_faults
txn_retire|tests/test_txn_retire
session|tests/test_session
lock|tests/test_lock
kv_lsm_concurrent|tests/test_kv_lsm_concurrent 4
membership|tests/test_membership
member_view|tests/test_member_view
data_commit|tests/test_data_commit
put_ticket|tests/test_put_ticket
put_orphan|tests/test_put_orphan
put_orphan_server|tests/test_put_orphan_server
append_receipt|tests/test_append_receipt
frag_record|tests/test_frag_record
small_segment|tests/test_small_segment
packed_segment|tests/test_packed_segment
pack_ledger_recovery|tests/test_pack_ledger_recovery
retained_namespace_audit|tests/test_retained_namespace_audit
put_ticket_batch|py:tests/test_put_ticket_batch.py
receipt_table|py:tests/test_w155_receipt_table.py
revoke_parallel|py:tests/test_session_revoke_parallel.py
EOF
}

if [ -n "${EFS_LEAK_LIST:-}" ]; then
    # Fixture lists name ready binaries by path; no memcheck build.
    LIST="$(cat "$EFS_LEAK_LIST")" || exit 2
else
    LIST="$(default_list)"
    case "$(uname -m)" in
        aarch64|arm64) arch_default="-march=armv8.2-a -mtune=generic" ;;
        x86_64)        arch_default="-march=x86-64-v3 -mtune=generic" ;;
        *)             arch_default="" ;;
    esac
    ARCH="${EFS_LEAK_ARCH-$arch_default}"
    BUILD="$LOGDIR/build"
    bins=$(printf '%s\n' "$LIST" | awk -F'|' '$2 !~ /^py:/ && $2 != "" {n = split($2, a, " "); for (i = 1; i <= n; i++) if (a[i] !~ /=/) {print a[i]; break}}')
    t0=$(date +%s)
    mkdir -p "$BUILD" &&
        tar -C "$HERE" -cf - Makefile include src deps tests | tar -C "$BUILD" -xf - &&
        make -s -C "$BUILD" clean >/dev/null 2>&1 &&
        make -s -C "$BUILD" -j"$(nproc 2>/dev/null || echo 4)" EXTRA_DEFS="$ARCH" $bins \
            >"$LOGDIR/build.log" 2>&1
    brc=$?
    if [ "$brc" != 0 ]; then
        tail -20 "$LOGDIR/build.log"
        echo "VERDICT: FAIL (memcheck build failed rc=$brc; log $LOGDIR/build.log)"
        exit 1
    fi
    echo "built memcheck binaries with EXTRA_DEFS='$ARCH' ($(( $(date +%s) - t0 ))s)"
    cd "$BUILD" || exit 2
fi

# Classify one binary's run from its exit status and its memcheck logs.
classify() {   # $1 name, $2 rc
    local name=$1 rc=$2 logs reports definite errs
    logs=$(ls "$LOGDIR/$name".*.vg 2>/dev/null)
    if [ "$rc" = 124 ] || [ "$rc" = 137 ]; then echo "timeout after ${TIMEOUT}s"; return; fi
    if [ -z "$logs" ]; then echo "no memcheck report (rc=$rc)"; return; fi
    reports=$(cat $logs | grep -c 'ERROR SUMMARY:')
    definite=$(cat $logs | sed -n 's/.*definitely lost: \([0-9,]*\) bytes.*/\1/p' | tr -d , \
        | awk '{s+=$1} END {print s+0}')
    errs=$(cat $logs | sed -n 's/.*ERROR SUMMARY: \([0-9,]*\) errors.*/\1/p' | tr -d , \
        | awk '{s+=$1} END {print s+0}')
    if [ "$definite" -gt 0 ]; then echo "definite leak $definite bytes"; return; fi
    if [ "$errs" -gt 0 ]; then echo "memcheck errors $errs"; return; fi
    if [ "$reports" -eq 0 ]; then echo "no complete memcheck report (rc=$rc)"; return; fi
    if [ "$rc" != 0 ]; then echo "test rc=$rc"; return; fi
    echo ok
}

run=0; failed=0; t_all=$(date +%s)
while IFS='|' read -r name cmd; do
    case "$name" in ''|'#'*) continue;; esac
    if [ -n "${EFS_LEAK_ONLY:-}" ]; then
        case " $EFS_LEAK_ONLY " in *" $name "*) ;; *) continue;; esac
    fi
    run=$((run + 1)); t0=$(date +%s)
    out="$LOGDIR/$name.out"
    mapfile -t VG < <(vg_args "$LOGDIR/$name.%p.vg")
    case "$cmd" in
    py:*)
        # The Python driver builds its own binaries; EFS_LEAK_WRAP makes it
        # run the plain positive binary under memcheck and skip its ASan
        # build (ASan and memcheck cannot share a process).
        EFS_LEAK_WRAP="${VG[*]}" ASAN_OPTIONS=detect_leaks=0 \
            timeout -k 10 "$TIMEOUT" python3 "${cmd#py:}" >"$out" 2>&1
        rc=$? ;;
    *)
        envs=(); read -r -a words <<<"$cmd"
        while [ "${#words[@]}" -gt 0 ] && [[ ${words[0]} =~ ^[A-Za-z_][A-Za-z0-9_]*= ]]; do
            envs+=("${words[0]}"); words=("${words[@]:1}")
        done
        env "${envs[@]}" timeout -k 10 "$TIMEOUT" "${VG[@]}" "${words[@]}" >"$out" 2>&1
        rc=$? ;;
    esac
    reason=$(classify "$name" "$rc")
    dt=$(( $(date +%s) - t0 ))
    if [ "$reason" = ok ]; then
        echo "PASS $name (${dt}s)"
    else
        failed=$((failed + 1))
        echo "FAIL $name: $reason (${dt}s)"
        # Show the first definite-loss stack so the failure is actionable.
        grep -h -A 25 'definitely lost in' "$LOGDIR/$name".*.vg 2>/dev/null | head -40
        tail -5 "$out"
    fi
done <<<"$LIST"

total=$(( $(date +%s) - t_all ))
if [ "$run" -eq 0 ]; then
    echo "VERDICT: FAIL (no binaries selected)"
    exit 1
fi
if [ "$failed" -eq 0 ]; then
    echo "VERDICT: PASS ($run run, 0 failed, ${total}s)"
    [ "$OWN_LOGDIR" = 1 ] && rm -rf "$LOGDIR"
    exit 0
fi
echo "VERDICT: FAIL ($run run, $failed failed, ${total}s; logs in $LOGDIR)"
exit 1
