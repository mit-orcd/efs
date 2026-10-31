# H-K22 allocator, receipt, bootstrap and Raft group (Task H split, F10-F20)

Purpose: port Task H fixes F10-F20 from `agent/task-h-repair` onto devel
`126ba81d4` as small commits on `agent/h-k22-allocator` (K22; K38 for F19/F20),
per [`task-h-wave33-reconciliation-20261029.md`](task-h-wave33-reconciliation-20261029.md). Verdict: **PASS for unit and
source-extracted regressions; gate result below; live K22 gates NOTRUN**
(blocks alpha: yes, status unchanged). F10 adds command kind 12: fresh
clusters only, no migration or compatibility.

## Environment and method

intel2 (Rocky 10, 8 CPU), `git archive` copies under `~/efs-h-k22`, TMPDIR
private, TCP/loopback only, no cluster started. Each fix's own test was run
against (a) devel's `src/` and `include/` with the new tests, and (b) the
fix commit's parent source with that commit's tests. Reproduce:
`git archive <commit>~1 | tar -x -C d && git archive <commit> tests | tar -x -C d`,
then `make tests/test_txn` (or `test_raft`) and run it, or run the named
`python3 tests/<test>.py` (source-extracted tests read `src/server/raft_host.c`).
Raw logs stayed on intel2 and were deleted.

## Fix table (devel before vs. with the change)

| Fix | Commit(s) | Test | Devel / prior source | With change |
|---|---|---|---|---|
| F10 allocator reserve at apply | 02ba70ce7 | `test_alloc_value_guard.py` (runs `mkdir_alloc_claim_gate.py`), `test_txn.c` | FAIL (`host_mkdir_alloc_reserve` absent; `EFS_TXN_ALLOC_INODE` undeclared) | PASS |
| F11 own-entry batch wait | 9dc317626 | `test_prepare_wait_progress.py` | FAIL (aborts on devel's fixed wait) | PASS |
| F12 receipt replay reads committed rows | a6d66a6d9, 1e187677d | `test_namespace_receipt_pending.py`, `test_txn.c` | FAIL / `efs_txn_opid_replay` undeclared | PASS |
| F13 scan gate released before coordinator proof | 4aac24509 | `test_txn.c:823` | FAIL "coordinator wait must run outside the KV scan reader gate" at parent source | PASS |
| F14 pending guard scoped to identity and covering ACK | b91d74923 | `test_namespace_receipt_pending.py` (stub is `opid_busy(seq)`), `test_txn.c` | `test_namespace_receipt_pending.py` FAIL on devel. The added `test_txn.c` checks pass at the parent commit: F12's implementation already scopes the guard, so they pin behaviour, not a separate failure | PASS |
| F15 session bootstrap and CREATE deadline | bac5f6f72, fd6160c15 | `test_publication_session_rpc.py`, `test_opid_admission.py` | FAIL (build/assert) | PASS |
| F16 retained CREATE nonterminal, authority delegation | 6681c5c1d, 97670beec | `test_retained_create_authority.py` | FAIL | PASS |
| F17 snapshot import after delayed ACK / replacement leader | cbe779bdf, b9c83a53c | `test_raft.c:2154`, `:2953` | FAIL "pending final chunk is retried after log ACK"; "zero rejection requests image without previous match knowledge" | PASS |
| F18 KV flush cohorts | 5d07a2496 | `test_kv_flush_finish.py` | FAIL | PASS |
| F19 wall-clock empty-prefix probe | ee9748b5b | `test_raft.c`, `test_host_inbox.py`, `test_receive_persist.py` | FAIL (`efs_raft_heartbeat` absent; `test_host_inbox`/`test_receive_persist` FAIL) | PASS |
| F20 known-leader contact while heartbeat queued | 91238d981 | `test_raft.c:826` | FAIL "queued known-leader heartbeats suppress false election" | PASS |

With all commits: `test_txn`, `test_raft` and the nine Python tests above PASS.
Devel-base run: libefs builds, `test_txn`/`test_raft` do not compile, nine
Python tests FAIL.

## Not ported / deferred

- Epoch-bound session ACK from `67350b59a` (SESS_ACK 30->34 bytes, ~15 files):
  not required by F15's bootstrap repro; belongs to the session/revoke group.
- Qualification and verifier scripts, K22 load-worker and leader-admission
  test edits, `test_k33_*` edits, `pin_only` scan flag, stage-trace logging:
  not needed by a kept test.
- No fix deferred to alpha 1.1 under D45: each kept change is a local repair
  (no `raft_host.c` status-coordination, write.c ownership or buffer-pool
  reshaping). Devel's K51 membership heartbeat/stop-leading, pending-row
  eviction, term/commit/install fences and K55 are kept unchanged.
- F14's `test_txn.c` additions are not independent failing reproductions
  (above); F18 remains a fix candidate: correlation, not proof of every
  election cause. Production-scale K22 plateau, 1 GiB lag and RDMA/multi-host
  gates remain NOTRUN.

## Gate

amd (32 CPU, shared with another session's gate; TMPDIR not XFS), final tree,
`EFS_TEST_JOBS=8 scripts/make_test_summary.sh`: **TOTAL 556 / FAIL 2 / NOTRUN 2**,
762 s, OVERBUDGET 4. NOTRUN: `test_cluster_stop` (cluster scripts absent),
`test_store_durability` (TMPDIR not XFS). The two FAILs are `test_k42_peer_stop_open`
and `test_stop_progress`, both `fusermount3 -u` "Device or resource busy" at
teardown on amd. `test_k42_peer_stop_open` passes alone on the same tree;
`test_stop_progress` fails identically on unmodified devel `126ba81d4` on amd, so
it is a host limit, not this change. An earlier run on the pre-fix tree had 6 FAIL:
four source-extracted harnesses needed the F11/F15 updates (committed, passing
on rerun). Verdict: **PASS with these two amd host-limit FAILs; not a clean
gate**; rerun on intel hosts before merge if a clean total is required.
