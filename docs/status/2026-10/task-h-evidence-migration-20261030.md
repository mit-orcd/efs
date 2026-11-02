# Task H evidence migration — October 30, 2026

**Purpose:** Task BO. `docs/status/2026-10/branch-disposition-20261030.md` found `agent/task-h-repair` (head `cd822e2f7`) parked only because five evidence pages exist on no other ref. This page records their migration onto devel and which of the branch-only scripts are worth keeping. **Verdict: PASS** (docs only; nothing built, no host used, the branch and its worktree untouched). Method: `git show agent/task-h-repair:docs/status/<name>.md`, each page copied under the same name with one added "Migrated from" line.

## What moved

| Page | Lines | Shortened |
| --- | ---: | --- |
| [task-h-wave33-reconciliation-20261029.md](task-h-wave33-reconciliation-20261029.md) (F01–F35 / R01–R11 inventory) | 234 | no |
| [task-h-publication-repair-20261026.md](task-h-publication-repair-20261026.md) | 3883 | no |
| [task-h-k38-b6-repair-20261028.md](task-h-k38-b6-repair-20261028.md) | 1516 | no |
| [task-h-devel-b2-integration-20261028.md](task-h-devel-b2-integration-20261028.md) | 262 | no |
| [task-a-finding-b-rerun-20261026.md](task-a-finding-b-rerun-20261026.md) | 317 | no |

Nothing was shortened because none embeds a raw capture: no line exceeds 1000 characters, the only code blocks are reproduction commands, one-line gate summaries and one collection script, and the tables are binary hashes and per-run summaries. Raw captures stay where the pages say they are (nuc `/home/maik/efs-artifacts/taskH-*`, node9901 `/data1/efs/traces/taskH-20261026`) with the hashes the pages give; this migration did not check that those locations still exist. Conclusions are unchanged.

Page content is a historical record of the branch: its "this branch", `EFS_GIT_BRANCH=agent/task-h-repair` and backticked script paths refer to the branch. Backticked paths of scripts and tests in the table below were not rewritten (hundreds of mentions); read them as `agent/task-h-repair:<path>`. The one markdown link to a branch-only file (`scripts/task_h_truncate_probe.py` in the Finding-B page) was replaced by that branch path.

## Links fixed on devel

Pointed at the migrated page instead of "on `agent/task-h-repair`" / "on that branch" / "on the Task H branch":

- `h-k22-allocator-20261029.md`, `h-k33-routing-20261029.md` (wave33 and K38/B6 pages), `h-k42-session-20261029.md`, `h-k45-shutdown-20261029.md`, `h-recovery-20261029.md`, `task-h-devel-baseline-20261029.md`: wave33 inventory.
- `branch-disposition-20261030.md`: the inventory line and "Why this branch stays" item 2 now say the pages were migrated.

Left as they are: other mentions of `agent/task-h-repair` as the source branch of a port (`h-k38-publication`, `alpha-1.1-consolidation`, `k38-task-g-repair`, `architecture-full.md` index lines, and the baseline's use of `git archive agent/task-h-repair tests/`) are true statements about the source, not links to the five pages.

## Branch-only scripts and tests: recommendation

Recommendation only; the user decides and the branch stays. The disposition page counts 20 scripts (all under `scripts/`) plus `tests/live/k22_*`, three tests and `tools/fuse_error_trace.c`; three more branch-only files turned up in the diff against devel (`src/server/put_epoch_files.h`, `tests/test_put_epoch_files.py`, `tests/test_k22_load_workers.py`) and are included. 29 in all. Judged from each file's docstring and which migrated page cites it; none was run.

Keep = the only reproduction of a page's claim. Drop = devel has an equivalent or the claim is historical.

| File | Rec | Reason |
| --- | --- | --- |
| `scripts/qualify_publication_repair.py` | keep | the qualification driver the publication, b2 and K38/B6 pages' reproduction sections run |
| `scripts/k22_verify_retained.py` | keep | cold readback oracle for the retained strict K22 fixture; the publication page's verdict rests on it |
| `scripts/k33_verify_clone.py` | keep | verifies the retained K33 failure on hashed clones with the original build; no devel equivalent |
| `scripts/k36_verify_retained.py` | keep | readback oracle for the K36 compaction-crash fixture |
| `scripts/k42_verify_retained.py` | keep | readback and fence resume for the K42 fixture |
| `scripts/verify_window.py` | keep | bounded verification helper imported by the retained verifiers; keep iff they are kept |
| `tests/test_verify_window.py` | keep | the helper's test; goes with `verify_window.py` |
| `scripts/task_h_truncate_probe.py` | keep | only reproduction of the Finding-B O_TRUNC/fsync NOT REPRODUCED result |
| `scripts/trace_recovery_failure.py` | keep | the acknowledgement-journal capture behind the open K33 fatal-read subfinding |
| `tools/fuse_error_trace.c` | keep | LD_PRELOAD errno trace used by that K33 capture; no devel equivalent |
| `scripts/check_fuse_error_trace.py` | keep | preflight for the trace library; goes with it |
| `src/server/put_epoch_files.h` | keep | F25 code, HELD; devel has it only in reverted history, so it goes with the F25 decision |
| `tests/test_put_epoch_files.py` | keep | F25's regression test; same reason |
| `scripts/d4_verify_retained.py` | drop | readback of a stopped private D4 fixture; claim historical, devel has `tests/test_d4_progress.py` and `test_d4_plateau.py` |
| `scripts/k22_verify_discovered.py` | drop | variant of `k22_verify_retained.py` for a journal-less failed fixture; historical |
| `scripts/stop_verify_retained.py` | drop | readback of the busy-client stop fixture; K42 stop cases are covered by devel's `tests/test_k42_peer_stop_open.py` |
| `scripts/check_k33_cleanup_logs.py` | drop | checks cleanup logs of one finished 64-case K33 campaign; historical |
| `scripts/inspect_k33_lease_history.py` | drop | read-only look at one synthetic name in retained logs; diagnostic |
| `scripts/trace_k33_stop_read.py` | drop | self-described "never a qualification PASS" failover diagnostic; superseded by the recovery trace |
| `scripts/inspect_allocator_refusals.py` | drop | allocator CAS summary for a diagnosis; the fix is ported (F10–F20) |
| `scripts/diagnose_publication_latency.py` | drop | "DIAGNOSTIC ONLY" four-way load trace; claim historical |
| `scripts/diagnose_publication_loaded.py` | drop | same class, one loaded round |
| `scripts/profile_publication_tail.py` | drop | same class, tail profile |
| `scripts/instrument_client_eio.py` | drop | patches a disposable tree for return-site diagnostics; no page cites it |
| `tests/live/k22_unapplied.py` | drop | live K22 leader admission; devel has `tests/test_k22_leader_admission.py` |
| `tests/live/k22_backlog_probe.c` | drop | log-pressure filler for that live test |
| `tests/test_k22_load_workers.py` | drop | tests the live gate's load helpers; goes with `k22_unapplied.py` |
| `tests/test_bootstrap_cohort.py` | drop | first-use bootstrap cohort is an R-row not ported by design |
| `tests/test_storage_fence_authority.py` | drop | devel has `test_storage_fence_members.py`, `test_storage_fence_retry.py`, `test_put_storage_fence.c` |

Limits: "devel has an equivalent" means a test of the same area exists, not that it was compared line by line. The 13 keeps are about 1.4k lines; if the user wants the branch gone, keeping only those means copying them (or tagging the branch head) first. Deleting the branch also still needs the F25 decision named in the disposition page.
