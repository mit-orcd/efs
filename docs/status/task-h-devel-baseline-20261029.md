# Task H devel baseline — October 29

**Purpose:** for each Task H fix F01–F35 (inventory: `task-h-wave33-reconciliation-20261029.md` on `agent/task-h-repair`), does the branch's regression test fail on current devel product code? Measurement only; no product code changed, no causes investigated.

**Verdict:** 18 DEVEL-BUG, 2 NOT-NEEDED, 13 NOCOMPILE, 2 NO-TEST, 0 FLAKY, 0 BRANCH-FAIL. No release gate or K row is changed by this page.

## Sources and builds

- devel `74d34d588cd42257fdf44803c89245b9f5613477`; branch `agent/task-h-repair` `cd822e2f73db8db6e7e7194a95f1fcea24419eed`.
- Trees under nuc `/home/maik/efs-taskH-baseline-20261029/`, shipped with `git archive <rev> | ssh nuc tar -x`:
  - `branch/` = branch `cd822e2f7`; `git archive` SHA-256 `c1c29fcfc95324bbd360a8481b7f9c141c76196284cef65886baecaa4eaa0ac8`.
  - `devel/` = devel `74d34d588`; `git archive` SHA-256 `80efaccf10ad5ad655fddb803927013cc27748c78a9725ae6be8ed175520dea6`.
  - `devel-btests/` = devel archive overlaid with `git archive agent/task-h-repair tests/` (devel-only test files stay); SHA-256 of the two concatenated streams `78454f096448ebb5655d2ac484bd206dabbce8e3c7c30f3ac9fec2ce558be9d4`.
- Build (`make -k -j8`, then `make -k -j8 test-bins`; build ID `unknown`, the trees carry no git metadata): `devel` and `branch` rc 0 for both steps. `devel-btests`: daemons, client and `libefs.a` build; `make` rc 2 because five test binaries do not compile against devel `src/` and `include/`. First errors:
  - `tests/test_pub_stage.c:347`: `struct efs_txn_pending_rec` has no member `pin_only`.
  - `tests/test_store_durability.c:210`: `put_epoch_owned` undeclared (`put_sync_note` also takes fewer arguments).
  - `tests/test_raft.c:3554`: implicit declaration of `efs_raft_heartbeat`.
  - `tests/test_txn.c:741`: implicit declaration of `efs_txn_opid_busy` (`EFS_TXN_ALLOC_INODE` undeclared at 1323).
  - `tests/test_session.c:1418`: too many arguments to `efs_session_ack_fence_word`.
- Build logs: `build/{devel,branch,devel-btests}.log` in the raw directory below.

## Environment and method

nuc: 8 CPUs, Linux, libfuse present, idle. Tests build their own fixtures under `/tmp`; no private cluster, daemon or mount of mine was left. Each test ran once per tree, one at a time, `EFS_TEST_JOBS=1`, in `screen` (`SCREENDIR=$HOME/.screen`), through `run.sh` with the working directory at the tree root and `timeout <budget>`. Budget is the test's row in the branch `tests/budgets.tsv`; unit tests with no row there got 300 s (my choice; none came near it). Every test that failed on devel-btests got one rerun on both sides (18 tests; all as before, branch pass and devel FAIL), so there is no FLAKY. No test or budget was changed.

Reproduce: ship the three trees as above, `make -k -j8 && make -k -j8 test-bins` in each, then run each command in the table from the tree root with `EFS_TEST_JOBS=1 timeout <budget>`; ids (F01a ...) name the transcripts. `run.sh` and `cases.tsv` sit in `/home/maik/efs-taskH-baseline-20261029/` on nuc.

Verdict rule per row: DEVEL-BUG if a runnable test of the row aborts on an assertion on devel-btests and passes on branch; else NOCOMPILE if a named test did not compile or could not find its target source (`ValueError: substring not found` means the test slices a function out of devel `src/` by name and it is not there); else NOT-NEEDED. Passing tests inside a NOCOMPILE row are noted in the table. Test ids: the letter after the F number names one test; a row reusing another row's test cites that id.

## Results

| Fix | K row | Test file(s) and command | Branch | devel-btests | Verdict |
|---|---|---|---|---|---|
| F01 | K38,K40 | F01a test_w155_report_resolve.py; F01b test_report_resolved_retry.py | pass; pass | F01a pass; F01b FAIL (assert run(&resolved)==STALE, rerun FAIL) | **DEVEL-BUG** |
| F02 | K38 | F02a test_put_ticket_owner.py; F02b test_k38_stale_span_replay.py; F02c test_stale_recovery.py | pass x3 | F02a NOCOMPILE (efs_client_put_retry_admitted undeclared); F02b pass; F02c NOCOMPILE (ValueError: substring not found) | **NOCOMPILE** |
| F03 | K38 | F01b test_report_resolved_retry.py | pass | FAIL (rerun FAIL) | **DEVEL-BUG** |
| F04 | K38,K40 | F04a test_host_cover_route.py | pass | NOCOMPILE (ValueError: substring not found, test cannot locate branch function in devel source) | **NOCOMPILE** |
| F05 | K38 | F05a test_pub_unit_forward_abort.py; F05b test_fin_abort_handoff.py | pass; pass | pass; pass | **NOT-NEEDED** |
| F06 | K38 | F06a test_put_drain.py | pass | NOCOMPILE (efs_session_ack_fence_word: too many arguments) | **NOCOMPILE** |
| F07 | K38 | F07a test_put_quota_reclaim.py; F07b test_put_retry_budget.py | pass; pass | F07a FAIL (OSError 28 ENOSPC, rerun FAIL); F07b NOCOMPILE (substring not found) | **DEVEL-BUG** |
| F08 | K38 | F08a test_getchunks_collect_retry.py | pass | FAIL (efs_getchunks_reply_valid assert, rerun FAIL) | **DEVEL-BUG** |
| F09 | K38,D4 | F09a test_dcache_lock_distribution.py | pass | FAIL (assert ns>=56 && nl>=24, rerun FAIL) | **DEVEL-BUG** |
| F10 | K22 | F10a test_alloc_value_guard.py; F10b ./tests/test_txn | pass; pass | F10a NOCOMPILE (substring not found); F10b NOCOMPILE (EFS_TXN_ALLOC_INODE undeclared) | **NOCOMPILE** |
| F11 | K22 | F11a test_prepare_wait_progress.py | pass | FAIL (host_wait_refs assert, rerun FAIL) | **DEVEL-BUG** |
| F12 | K22 | F12a test_namespace_receipt_pending.py; F10b ./tests/test_txn | pass; pass | F12a NOCOMPILE (efs_txn_key_busy implicit decl); F10b NOCOMPILE | **NOCOMPILE** |
| F13 | K22 | F10b ./tests/test_txn | pass | NOCOMPILE (efs_txn_opid_busy implicit decl; EFS_TXN_ALLOC_INODE undeclared) | **NOCOMPILE** |
| F14 | K22 | F12a test_namespace_receipt_pending.py; F10b ./tests/test_txn | pass; pass | NOCOMPILE; NOCOMPILE | **NOCOMPILE** |
| F15 | K22 | F15a test_opid_admission.py; F15b test_create_errors.py; F15c test_publication_session_rpc.py | pass x3 | F15a NOCOMPILE (rpc_writer_retry_pause unused, -Werror); F15b pass; F15c NOCOMPILE (session_start_mu undeclared) | **NOCOMPILE** |
| F16 | K22 | F16a test_retained_create_authority.py | pass | FAIL (host_retained_create_proof assert, rerun FAIL) | **DEVEL-BUG** |
| F17 | K22 | F17a test_k22_snapshot_import.py; F17b ./tests/test_raft | pass; pass | F17a pass (305 s of 310 s); F17b NOCOMPILE (efs_raft_heartbeat implicit decl) | **NOCOMPILE** |
| F18 | K22 | F18a test_kv_flush_finish.py | pass | FAIL (assert count==1?peak==1:peak>=2, rerun FAIL) | **DEVEL-BUG** |
| F19 | K38,K22 | F19a test_receive_persist.py; F17b ./tests/test_raft | pass; pass | F19a NOCOMPILE (efs_raft_heartbeat); F17b NOCOMPILE | **NOCOMPILE** |
| F20 | K38,K22 | F20a test_host_inbox.py | pass | FAIL (assert n<=5, rerun FAIL) | **DEVEL-BUG** |
| F21 | K42 | F21a test_revoke_redirect.py; F21b test_session_finish_retry.py | pass; pass | F21a FAIL (session_revoke_submit assert, rerun FAIL); F21b NOCOMPILE (efs_session_cleanup_bounded conflicting types) | **DEVEL-BUG** |
| F22 | K42 | F21b test_session_finish_retry.py | pass | NOCOMPILE (efs_session_cleanup_bounded conflicting types) | **NOCOMPILE** |
| F23 | K42 | F23a test_session_cleanup_cli.py | pass | FAIL (assert selections==2 && proposals==1 ..., rerun FAIL) | **DEVEL-BUG** |
| F24 | K38,K51 | F24a test_shutdown_bounded.py | pass | FAIL at line 90, the shutdown phase-table check, before the rejoin section runs (rerun FAIL) | **NO-TEST** |
| F25 | K38 | F25a test_put_epoch_files.py; F25b ./tests/test_store_durability | pass; pass | F25a NOCOMPILE (src/server/put_epoch_files.h absent); F25b NOCOMPILE (put_epoch_owned undeclared) | **NOCOMPILE** |
| F26 | K36,K47 | F26a test_get_integrity.py; F26b test_read_recorded_nodes.py; F26c test_k36_recovery.py | pass x3 | F26a FAIL (packed_cases assert, rerun FAIL); F26b FAIL (overlay_one_delta assert, rerun FAIL); F26c pass (58 s) | **DEVEL-BUG** |
| F27 | K48 | F27a test_readdir_migration_race.py | pass | pass | **NOT-NEEDED** |
| F28 | K43 | F28a test_recovery_pin_owner.py | pass | NOCOMPILE (efs_txn_pending_rec has no member pin_only) | **NOCOMPILE** |
| F29 | K36 | F29a test_txn_recovery_priority.py | pass | FAIL (assert recovered==1 && !pending[3820] ..., rerun FAIL) | **DEVEL-BUG** |
| F30 | K33,K36 | F30a test_metadata_retry_budget.py | pass | FAIL (assert seq_nodes[0]==1 && seq_nodes[1]==2 && seq_nodes[2]==2, rerun FAIL); one abort, F30 and F33 not separable | **DEVEL-BUG** |
| F31 | K33 | F31a test_lease_edge_rpc.py | pass | FAIL (assert open_rc==OK ... calls==10, rerun FAIL) | **DEVEL-BUG** |
| F32 | K33,K38 | F32a test_put_ticket_rpc.py | pass | FAIL (assert !edge_rc && state==1 && calls==11 ..., rerun FAIL) | **DEVEL-BUG** |
| F33 | K33 | F30a test_metadata_retry_budget.py | pass | FAIL, same abort as F30 | **DEVEL-BUG** |
| F34 | K33 | F24a test_shutdown_bounded.py; F17b ./tests/test_raft | pass; pass | F24a FAIL at line 90, before the K33 section runs; F17b NOCOMPILE | **NO-TEST** |
| F35 | K45,W130 | F35a test_mgmt_resync_busy.py | pass | FAIL (assert cmd_raft_resync(2,a)==0, rerun FAIL) | **DEVEL-BUG** |

Transcripts: `/home/maik/efs-artifacts/taskH-baseline-20261029/runs/<tree>/<id>.log` for tree `branch` or `devel-btests` (reruns in `<tree>-rerun/`; `<tree>.results.tsv` holds rc, seconds, budget). Each log ends with a `RESULT rc=... cmd=...` line.

## Counts

| Verdict | Rows | Count |
|---|---|---|
| DEVEL-BUG | F01 F03 F07 F08 F09 F11 F16 F18 F20 F21 F23 F26 F29 F30 F31 F32 F33 F35 | 18 |
| NOT-NEEDED | F05 F27 | 2 |
| NOCOMPILE | F02 F04 F06 F10 F12 F13 F14 F15 F17 F19 F22 F25 F28 | 13 |
| NO-TEST | F24 F34 | 2 |
| FLAKY, BRANCH-FAIL | none | 0 |

## Limitations

- F24 and F34: `test_shutdown_bounded.py` fails on devel at its line 90 (shutdown phase-table check, an earlier expectation), so the rejoin (F24) and stopping-leader (F34) sections never ran. Not counted as DEVEL-BUG. F34's other test, `test_raft`, does not compile on devel.
- F30 and F33 share one file that aborts once; the abort is not separated between them. F03 shares F01b with F01. F13 and F14 rest on `test_txn` and `test_namespace_receipt_pending`, which do not compile.
- NOCOMPILE says the branch test cannot run against devel (internal API or source layout changed). It is neither bug nor no-bug evidence for devel. 13 of 35 rows are in this state, including F10 (allocator kind 12), F12–F14, F22, F25 and F28.
- F17a passed on devel in 305 s against its 310 s budget.
- A failing assertion shows the branch test fails on devel code, not why. `make test-all` and live gates were not run.

## Raw output

Not committed. nuc `/home/maik/efs-artifacts/taskH-baseline-20261029/SHA256SUMS` (132 files; the manifest's own SHA-256 is `af5f5edf1090931ab69c4e303c7d3fd4bada34d7ae811a6a03ddc2968c0a0da5`).
