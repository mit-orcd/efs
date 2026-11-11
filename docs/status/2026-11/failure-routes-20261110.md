# Failure routes: what a recurring failure line proves (Task U2)

**Merge state (Nov 10):** merged into devel in wave 78 after an independent review of every route against the source. The merge corrected the statements the review found wrong or too narrow: what `abort decided rc=-7` proves on `txn-finish`, the causes of `dcache-flush-fail stage=put`, the meaning of `rc=` on `report-split`, `report-resolve` and `report-sync-fail`, what `stage=report` covers on `flush-meta`, the description of `uncertified-busy` and its next check, and the next check and K rows of the `efs-fuse` route. It dropped the `efs-mgmt` route (no K row contains its line), so the page has 8 routes, and it rewrote six error-code comments (IO, PROTO, NO_QUORUM, QUOTA, BUSY, FENCED); no code is "not determined" now. The report below is the task's own and is not updated. Limit of `scripts/check_failure_routes.py`: it checks the six labels, their order and one function name per route, not whether a statement is true.

*The paragraph above is the current state. Everything below is the task's report as it was written before the merge: where it says "unmerged", "not merged" or names a next action, the paragraph above replaces it.*

- Source commit: devel `67fc3b4a1`; changes on top in branch `claude/task-u2-failure-routes-35b2a5` (commits listed by `git log devel..claude/task-u2-failure-routes-35b2a5`).
- Verdict: **PASS** for the documentation and checker described below (no test host involved, nothing run on a cluster).
- Remaining failures: none in the checks; 9 routes written, 2 of the 21 undocumented error codes are "not determined" (`EFS_ERR_BUSY`, `EFS_ERR_FENCED`); the seeds `txn-finish` and the three refusal reasons are not diagnostic keys of `src` as the task assumed (see Limitations).
- Active run: none.
- Next action: the coordinating session reviews and merges; wave merges regenerate `docs/how-it-works/diagnostics.md`, so rerun `python3 scripts/gen_diagnostics.py` after a merge.
- Merge state: branch `claude/task-u2-failure-routes-35b2a5`, unmerged.

## Purpose

An external structure review of devel `67fc3b4a1` found that `docs/how-it-works/diagnostics.md` locates code but does not say what a failure line proves. This task fills the empty Meaning cells of the error codes (Part A) and writes routes for the recurring failures (Part B): [the routes page](../../how-it-works/failure-routes.md).

## What changed

- `include/efs/common.h`: a trailing comment on each of the 21 `#define EFS_ERR_*` lines that had none (comments only). `cmp` of `cc -E -P -Iinclude include/efs/common.h` before and after: exit 0.
- `scripts/gen_diagnostics.py`: one sentence in the introduction linking the routes; `docs/how-it-works/diagnostics.md` regenerated.
- `docs/how-it-works/failure-routes.md` (new, 9 routes), `scripts/check_failure_routes.py` (new), one line in the `docs-check` recipe of `Makefile`, one sentence in `AGENTS.md`.

## Part A: sites read per code

Three or more return sites per code were read (`grep -rwn "EFS_ERR_<NAME>" src`, surrounding function): IO (`bench_local.c`, `member_store.c`, `raft_host.c`), NOMEM (`kv_snap.c`, `metadata.c`, `sim.c`), NOT_FOUND (`kv_lsm.c`, `metadata.c`, `meta_apply/lookup.c`), EXIST (`append_plan.c`, `meta_apply/namespace.c`, `sim_txn.c`), INVAL (`efs_mgmt.c`, `query_disk.c`, `pack_ledger.c`), NET (`raft_host.c`, `protocol.c`, `inode_rpc.c`), PROTO (`kv_snap.c`, `meta_apply/lanes.c`, `record_decode.c`), NO_QUORUM (`recovery.c`, `inode_rpc.c`, `write.c`), DECODE (`member_store.c`, `common.c`, `packed_reader.c`), CHECKSUM (`pack_mapping.c`, `segment_rebuild.c`, `packed_reader.c`), QUOTA (`query_disk_budget.h`, `store.c`, `efs_fuse/write.c`), NOT_EMPTY (`metadata.c`, `raft_host.c`, `efs_fuse/namespace.c`), BUSY (`kv_snap.c`, `raft_host.c`, `publication_ack_owner.c`), STALE (`session.c`, `txn.c`, `raft_host.c`), NOT_PRIMARY (`kv_lsm.c`, `raft_host.c` twice), UNSUPPORTED (`kv_lsm.c`, `raft_host.c` twice), FRAG_FORMAT and FRAG_IDENTITY (`handler.c`, `common.c`, `efs_fuse/namespace.c`), FENCED (`session.c`, `raft_host.c`, `inode_rpc.c`), UNREGISTERED (`session.c`, `raft_host.c` twice), UNFORMED (`meta_apply/init.c`, `raft_host.c` twice).

Sampling was every n-th site, not an exhaustive read; several of these codes have hundreds of sites (INVAL 1385, BUSY 827), so each comment states only what the sampled sites agree on.

"Not determined": `EFS_ERR_BUSY` (a size or byte limit in `kv_snap.c` against a still non-empty queue in `publication_ack_owner.c`; retry advice differs) and `EFS_ERR_FENCED` (an epoch below the session floor in `session.c` against a not-OK data-commit reply in `raft_host.c`). Count: 2 of 21.

## Part B: ranking of keys by number of K rows mentioning them

Command: `grep -l -- "<key>" docs/status/k/*.md | wc -l` for each of the 102 keys of the "Diagnostic keys" table. Substring match, so a key that is also a program name (`efs-fuse`, `efs-mgmt`) is inflated. Count then key, all 102 keys:

efs-fuse 6, efs-mgmt 6, flush-meta 4, report-resolve 3, report-split 3, report-sync-fail 3, dcache-flush-fail 2, publish-stalled 2, report-stuck 2, stop-refused 2, storage-fence 2, append-abandon 1, background-pressure 1, degraded-mark 1, force-discard 1, put-ticket 1, raft-log 1, raft-status 1, segment-rebuild 1, append-reserve 0, buf-budget 0, buf-fault 0, cwi-fail 0, data-commit 0, dcache-inode-gone 0, dcache-pressure 0, efs-client 0, flush-prepare-fail 0, fsync-split 0, gc-health 0, inode-rpc 0, kv-bound 0, kv-compact 0, kv-fault 0, kv-flush 0, kv-flush-prof 0, kv-lsm 0, kv-snap 0, lookup-reply 0, mtime-pin 0, namespace-prof 0, opid-admission 0, opid-append-rpc 0, opid-begin 0, opid-create 0, opid-create-adopt 0, opid-end 0, opid-lease 0, opid-mkdir 0, opid-owner 0, opid-stats 0, publish-contention 0, put-attempt 0, put-orphan 0, put-reply 0, put-upload 0, query-cache 0, query-catchup 0, query-config 0, query-dirty 0, query-disk 0, query-publish 0, query-recovery 0, query-refresh 0, query-restore 0, query-revisions 0, query-stage 0, raft-append 0, raft-change 0, raft-dir 0, raft-hold 0, raft-host 0, raft-session 0, raft-snap 0, raft-step-down 0, rdma-trace 0, read-fragments 0, read-prof 0, readdir-trace 0, rebuild-put 0, reply-buffer 0, report-admission 0, report-irec 0, report-landed 0, report-loop 0, report-pressure 0, report-receipt 0, report-replay 0, report-slot-timeout 0, report-span-lost 0, report-stale 0, rpc-create 0, stage-evict 0, storage-budget 0, storage-inventory 0, sync-write 0, test-create-adopt-negative-request 0, test-create-adopt-reply-loss 0, test-create-retry 0, txn-fault 0, txn-stale-version 0, write-queue 0

Top 6 (ties alphabetical): `efs-fuse` 6, `efs-mgmt` 6, `flush-meta` 4, `report-resolve` 3, `report-split` 3, `report-sync-fail` 3. Seeds added: `dcache-flush-fail` (2), `raft-host: txn-finish` (not a table key; the table key is `raft-host`), and the refusal reasons. Nine routes.

## Routes and the functions read for each

| Route | Functions read in full | Callers read (one level up) |
|---|---|---|
| `dcache-flush-fail` | `efs_dcache_flush_ino`, `dcache_flush_ino_pass`, `put_win_wait`, `flush_pipe_add`, `flush_pipe_drain`, `dcache_retire_gone` | `report_dirty_ino_complete`; other callers (`efs_fuse/namespace.c`, `efs_fuse/write.c`, `efs_fuse/append.c`, `efs_dcache_drain_pending`) located by grep, not read |
| `flush-meta` | `efs_fuse_flush_ino`, `efs_file_sync_error`, `efs_append_flush_report` | `ll_flush_run` |
| `report-sync-fail` | `report_dirty_ino_complete` | `efs_client_report_dirty_ino`, `efs_client_report_pressure_ino` |
| `report-resolve` | `efs_client_rpc_report_resolve`, `report_rc_transient` | `report_dirty_ino_run` (call site only) |
| `report-split` | `server_raft_host_report` (the failure and logging part), `host_report_stale_verdict` | `host_report_exec` |
| `raft-host: txn-finish` | `host_fin_one`, `host_txn_retire`, `host_fin_thread` | `host_fin_thread`; the enqueue site at `HOST_FIN_ABORT` located by grep, not read |
| refusal `reason=` | `efs_session_cleanup_reason`, `efs_session_cleanup_retry_ok`, `cmd_raft_session_fence`, `efs_client_session_end`, salt use in `session_revoke.c` | `efs_session_revoke` (reason fields only) |
| `efs-fuse` | `efs_fuse_destroy` | FUSE ops table registers it |
| `efs-mgmt` | `mgmt_identify`, `mgmt_session_get`, `mgmt_session_admit` | six `cmd_raft_*` callers (the `mgmt_identify` call sites only) |

K rows: the `## Disposition` of K42 and K112 was read for the refusal and `txn-finish` routes; the other K rows (K17, K30, K38, K39, K40, K43, K56, K81, K90, K102, K5, K6, K22, K35, K37, K95) are linked from "Seen in", their text was not read for this task. No K row has a `## Summary` section.

## Left "not determined" on the routes page

- `report-resolve`: how `id=` relates to the server's `pub=`.
- `raft-host: txn-finish`: why a line repeats for one `coord`.
- `dcache-flush-fail`: the cause when neither `dcache-inode-gone` nor `report-sync-fail` is present.
- `flush-meta`: whether the write-back error or the stall caused `stage=writeback`.
- `efs-mgmt`: which setup step failed.
- refusal reasons: the holder of an OPEN append; the server pairing with `txn-finish` is quoted from K112, not shown.

## Commands run and results

| Command | Result |
|---|---|
| `cmp` of preprocessed `common.h` before and after | exit 0 |
| `python3 scripts/gen_diagnostics.py --check` | exit 0 |
| `python3 scripts/check_failure_routes.py` | exit 0 |
| same, one `Next check` line deleted | exit 1, "labels [...] want [...]" for `dcache-flush-fail` |
| same, `efs_dcache_flush_ino` misspelled | exit 1, "efs_dcache_flush_inx is not defined in src/client/write.c" |
| same, heading `flush-meta` misspelled | exit 1, "flush-metax is not a key in diagnostics.md" |
| restored | exit 0 |
| `python3 scripts/check_doc_links.py`, `python3 scripts/status_layout.py`, `make docs-check` (includes the new checker), `python3 scripts/check_k_register.py` | all exit 0 |

The routes page is 85 lines. The graph rebuild and `tests/test_code_graph.py` are recorded in the report to the coordinating session.

## Limitations

- The seed `txn-finish` is the second word of the line `raft-host: txn-finish ...`, not a key of the diagnostics table; the heading keeps the real line shape and the checker tests its first word.
- The refusal reasons are not printed from a literal in `src`: `efs_session_cleanup_reason` in `include/efs/session_revoke.h` returns them and `efs_mgmt.c` and `inode_rpc.c` print them. `append-reap-owed` is, per `include/efs/meta_apply.h`, no longer produced.
- Every route statement was derived from reading code, not from a run; no log was reproduced.
- The enqueue site of `txn-finish` aborts and several `dcache-flush-fail` callers were not read.
- Reproduction: `python3 scripts/check_failure_routes.py`; the ranking is the `grep -l` loop above.
