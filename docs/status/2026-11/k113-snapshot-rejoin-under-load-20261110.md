# K113: a member that rejoins by snapshot finishes under sustained load (Task FT)

**Status**
- Source commit: `fc5566962` (all code and tests; the commits after it change only this page, the ledger and the graph). Branch `claude/beautiful-sinoussi-b869f2`, on `k113-base` `cdd20885c` = devel `fe81bb0e7` plus the K109 fix. Tested tree: `git archive fc5566962`, tar sha256 `6b2a8edf6234e5e0…` (full value in the raw directory's `tar-sha256.txt`), built on fstor007 with the host's libfuse, build id `unknown` (no git metadata in the shipped tree).
- Verdict: **PASS**: `tests/test_k22_snapshot_import.py` (unchanged) fails 0 of 3 on the base and passes 3 of 3 on the fixed tree on fstor007, alone; the full gate on the fixed tree ends `FAIL 0`; `make test`, ASan and TSan pass.
- Remaining failures: none in the final gate. The first gate on an earlier fixed tree had three failures (two mine, one flake; see "Verification"). Not verified: production snapshot window, distinct hosts, RDMA (see "Not verified").
- Next action: the coordinator rules on K113 and merges; this page changes no K row.
- Branch: `claude/beautiful-sinoussi-b869f2`, **unmerged**, not pushed.

Linked item: K113 (blocks alpha: yes, the row's own value; not changed here).

## Reproduction

The same unchanged test, base tree `cdd20885c`, run alone (no gate around it). Counts are node 3's `raft-snap: import start|open|applied|installed` lines and the sum of the two leaders' `raft-snap: start` lines.

| Host | Run | Result | start | open | applied | installed | leader snapshots |
|---|---|---|---|---|---|---|---|
| intel2 (8 threads) | 1 to 3 (first set) | pass | not captured | | | | |
| intel2 | 1 | pass | 3 | 3 | 2 | 0 | 11 |
| intel2 | 2 | pass | 5 | 5 | 2 | 0 | 46 |
| intel2 | 3 | pass | 2 | 2 | 2 | 0 | 0 |
| xorinox (16 threads) | 1 | pass | 3 | 3 | 2 | 0 | 22 |
| xorinox | 2 | pass | 3 | 3 | 2 | 0 | 29 |
| xorinox | 3 | pass | 2 | 2 | 2 | 0 | 22 |
| **fstor007 (96 threads)** | 1 | **FAIL** | 110 | 103 | **0** | 0 | 363 |
| **fstor007** | 2 | **FAIL** | 105 | 109 | **0** | 0 | 359 |
| **fstor007** | 3 | **FAIL** | 128 | 124 | **0** | 0 | 405 |

The failure needs the load of a big host: on intel2 and xorinox the writers are too slow for the leader to snapshot faster than one import completes. On fstor007 it fails alone, not only inside the gate. Failure text: `no InstallSnapshot on node 3 within the bound` (300 s). Each import on the base costs about 0.25 to 0.5 s for export plus open (`n_local` 330k to 350k keys against `n_in` 840k to 870k) and is thrown away by the next snapshot, which arrives 1 to 3 s later.

## Design note

1. **Not restarting.** A chunk at offset 0 (the closing chunk of a one-chunk image or the first of a longer one) of any snapshot, newer or the same, gets `BUSY` while an import is queued, running, DONE or being applied (`snap_import_active` in `host_snap_chunk`). Nothing of it is kept: the answer carries the offset 0, the leader sees no progress and retries after a heartbeat, without a read of the chunk. X's `.part` file and importer stay untouched; Y is simply refused until X is installed. After X is installed the next chunk 0 of Y is accepted as before (Y's `.part` is created then). A failed import (`SNAP_IMP_FAIL` or a failed step) is not "active": the next chunk 0 discards it and starts clean, as in wave 74.
2. **Finishing X when the leader no longer asks.** The Raft core records, when it answers the closing chunk of an image `BUSY`, that image's index, term and membership (`imp_*` in `src/raft/raft.c`; a chunk 0 of Y overwrites `rx_*`, so they are copied). The host pump calls the new `efs_raft_snap_imported(r, incl, term)` when the import is done; it runs the same `snap_installed` as the closing chunk path with the saved membership: snapshot index and applied index become X, the saved config is persisted, and a SNAP_REP goes to the current leader. It refuses (`EFS_ERR_INVAL`) any image other than the recorded one, an index at or below `last_applied`, or below `snap_idx`. Safe because applies of that group are blocked (`import_block`) from the start of the import until it is installed, so `last_applied` is still below X, and an older complete image X is exactly the state at X: the log after X (or a newer snapshot) follows through the normal paths. If the core refuses after the KV already holds X, the host requests a resync (`efs_raft_request_resync`), which replaces the state from the leader's next snapshot (fail closed).
3. **Progress independent of the leader.** `host_snap_import_pump` runs in the pump cycle (`h->mu` held) after `drain_inbox`: one `snap_import_poll` per group, i.e. one `efs_kv_import_step` (at most 1024 keys written and 16384 records merged) per cycle. That bounds the work per cycle exactly as the leader-retry step did, so AppendEntries and heartbeats are served between steps. After an accepted step that is not the last, the pump kicks its own eventfd (`host_pump_kick`), so the next step follows without waiting for the 5 ms timer; when the KV refuses a batch (`BUSY`, an L0 stall) `import_stall` is set, nothing is kicked and the pump keeps its normal wait: no spin, no new timeout and no sleep (`host_pump_poll_timeout` is unchanged). A leader retry of the closing chunk of the same image still steps and installs through the old path; both run on the pump thread.
4. **Convergence, with this test's numbers (fstor007, fixed tree).** The leader snapshots every 1 to 2 s; one import is export+open 0.26 to 0.33 s plus 208 to 209 steps for a 212k-key diff, `import applied total_ms` 1.3 to 1.4 s (`step_max_us` 42 to 120 ms), so one import (about 1.7 s) is of the order of the snapshot interval and the old restart-per-snapshot scheme could never finish; the new scheme does not care. After X is installed the log after X was still retained (window 1 MiB past each snapshot, 1 to 2 s of load), so the member caught up 0 to 1 s later without another snapshot: `resume->install` 4 to 5 s and `install->caught` 1 s in all three runs, 2 to 3 imports per run (the two groups, one extra). A second and third import were not needed here, so their cost is not measured; the local image is then close to the leader's, so export and open stay at 0.3 s and the diff shrinks to the keys written since X. Stop condition A did not apply. It would apply at production caps if writers outrun the import; not measured.
5. **Fail closed.** `import_block` is set when the import starts (GC thread) and cleared only when it is installed, fails or is cancelled, so a half-imported member never applies log entries or serves reads (`host_query_capable` refuses while the import state is not IDLE) and does not claim a caught-up applied index: the Raft core is not told anything until the import is complete. Votes: the core's last log is unchanged until the install. A failed step discards the importer and restarts cleanly (wave 74 path, test `test_failed_importer_is_discarded` unchanged).

## Code

- `src/server/raft_host.c`: `snap_import_active`, the offset-0 guard in `host_snap_chunk`, `host_snap_import_pump` (called from `host_pump`), `import_stall`.
- `src/raft/raft.c`, `include/efs/raft.h`: `imp_*` fields, `efs_raft_snap_imported`.
- Callers checked before editing: `snap_import_poll` had one caller (`host_snap_chunk`), `snap_import_cancel` two (`host_snap_chunk` and the cancel-during-open test); `host_pump_poll_timeout` is unchanged (`tests/test_host_inbox.py` compiles it stand-alone).

## Tests

| Test | Base | Fixed tree |
|---|---|---|
| `tests/test_snap_import_pump.c` (new, runs `tests/test_snap_import_host.c` first, unchanged): a newer image's chunk 0 (closing and first) is refused and X's importer, `.part` and `import_block` stay; the pump steps without any leader retry and installs X (`efs_raft_snap_imported` wrapped); Y is accepted afterwards; the KV refuses five batches: one step per pump call and no kick (normal wait), then an accepted non-final step kicks the pump | fails (the guard removed: refused chunk cancelled the import); does not build on the base (new symbols) | pass |
| `tests/test_raft.c` `test_snapshot_imported_without_retry`: after a `BUSY` closing chunk the follower installs the image without a retry (snapshot and applied index equal the image's); wrong index and a second call are refused | does not build on the base | pass |
| `tests/test_snap_import_host.c`, `tests/test_kv_snap_import.c` (unchanged) | pass | pass |
| `tests/test_k22_snapshot_import.py` (unchanged), fstor007 alone | FAIL 0 of 3 | PASS 3 of 3 |

## Before / after (fstor007, test alone)

| | Base | Fixed |
|---|---|---|
| Result | 0 of 3 | 3 of 3 |
| Node 3 `import start` / `applied` / `installed` | 105 to 128 / 0 / 0 | 2 to 3 / 2 to 3 / 2 to 3 |
| Leader `raft-snap: start` | 359 to 405 | 96 to 112 |
| resume to INSTALLED | never within 300 s | 4 to 5 s |
| INSTALLED to caught up | | 1 s |
| node 3 RSS peak | | 70 to 89 MB |

## Verification

Host fstor007 (96 threads, user erbmi1, work under `/tmp/efs-taskFT`, removed afterwards) for the K22 runs and the gates; intel2 for the rest.

| Check | Result |
|---|---|
| K22 test alone, fixed tree `fc5566962` | PASS 3 of 3: 2/2/2/2, 4/4/4/3, 4/4/4/3 (start/open/applied/installed; leaders 100, 0, 133; "installed" counts only pump installs, the others went through the closing-chunk path) |
| Full gate on fixed tree `fc5566962`, `EFS_TEST_JOBS=8 bash scripts/make_test_summary.sh` | `TOTAL 609 FAIL 0 NOTRUN 3 STATUS INCOMPLETE (680s) OVERBUDGET 2 HOSTLIMIT 3`, exit 3: the three NOTRUN rows are host limits (no cluster scripts, no valgrind header, no RDMA hardware), and the two OVERBUDGET rows (`test_k41_packer_placement.py` 70.3 s against 65, `test_k110_barrier_history.py` 11.9 s against 10) are why the status is INCOMPLETE rather than COMPLETE; budgets untouched. The K22 test row in `timings.tsv`: index 565, 100.2 s, rc 0 (it did not fail inside the 8-job gate, the case the K109 fix exposed) |
| First gate, earlier fixed tree `2f65444c6` | FAIL 3: `tests/test_host_inbox.py` (mine: it compiles `host_pump_poll_timeout` stand-alone and my change added fields to it; fixed by leaving that function unchanged, 3 of 3 after), `tests/test_raft_ack_window.py` (mine: my new core case in `tests/test_raft.c` leaked the deferred messages under its ASan build; fixed, base alone 3 of 3, fixed tree passes in the final gate), `tests/test_b6_unavailable.py` with `EFS_B6_MODES=full` (`FileNotFoundError` in its own cleanup loop inside the gate; alone on that tree 3 of 3 pass, and it passes in the final gate; not mine, not changed) |
| `make test` (intel2) | rc 0 |
| `tests_for server/raft_host --run`, `tests_for raft --run` (intel2) | rc 0, rc 0 |
| `bash tests/sanitizer_check.sh asan`, `tsan` (intel2) | rc 0, rc 0 |
| Mac: `check_k_register.py`, `gen_ledger.py`, `check_doc_links.py`, `gen-architecture-full.py`, `make docs-check`, code graph rebuild and `tests/test_code_graph.py` | all pass (K register 113 rows, 0 dangling links, architecture regenerated, `make docs-check` clean); graph checks in the last commit |


## Round 2 (Task FU)

An independent review of the Task FT fix found four defects. Task FU repairs them on top of `wave77-base` (`20d58f67c`). Tests: the new cases fail on `wave77-base` and pass on the fixed tree (see "Round 2 verification"). The earlier text of this page stays as written, except the claim in finding 4.

| Finding | Defect | Change |
|---|---|---|
| F1 | In `on_snap_req` a chunk at offset 0 overwrote `rx_*` before `snap_chunk` ran. The host answered `BUSY` both for a queued closing chunk and for a refused chunk 0, so a refused one-chunk image Y overwrote `imp_*` with Y. The pump then finished X, `efs_raft_snap_imported(X)` returned `EFS_ERR_INVAL`, the member asked for a resync, and this could repeat with the next image. | `host_snap_chunk` refuses chunk 0 with `EFS_ERR_AGAIN` (no other path of the snapshot callback returns it). The core computes the new membership in locals, assigns `rx_*` only when the host did not refuse, and on `AGAIN` answers as for `BUSY` (`success = 1`, `prev_index = offset`) without touching `rx_*` or `imp_*`. `imp_*` is recorded only in the real `BUSY` branch of a closing chunk. |
| F2 | `host_snap_import_pump` treated a hard `efs_kv_import_step` error like "more to do": no log, no resync, the core was not told. | The pump separates the hard error (neither OK nor `BUSY`): it logs `raft-snap: import failed group=%u incl=%llu rc=%d`, calls `efs_raft_request_resync`, and does not kick. `snap_import_poll` has already dropped the importer and set the state to IDLE with `import_block` cleared, so the next chunk at offset 0 is not refused (`snap_import_active` is false) and starts a fresh import. |
| F3 | `efs_raft_snap_imported` had no role guard, and with `leader < 0` it addressed its reply to peer 0. | It returns `EFS_ERR_INVAL` when the member is the leader. With no known leader it installs and sends no reply (`snap_installed` skips the send when `m->to < 0`). |
| F4 | The page said the member catches up without another snapshot. That holds only while the leader still sends X. | Corrected below. |

**Correction (F4).** The sentence in design note 4, "the member caught up 0 to 1 s later without another snapshot", describes the K22 test, where the leader's retained log still covered X. When the leader has already moved on to Y, `on_snap_rep` (`src/raft/raft.c`) ignores the ack for X, and Y is transferred and imported in full after X is installed. The fix guarantees that an import finishes under load. It does not save the transfer of the next snapshot. Not measured: how often this happens at production caps.

**Cancel branch.** The branch in `snap_import_poll` that handled "state left APPLY while the pump stepped" is deleted. The only writer of the state during a step was `snap_import_cancel`, called from `host_snap_chunk` at offset 0, and that is now refused while the state is APPLY. `host_snap_chunk` and the pump both run with `h->mu` held.

### Round 2 verification

Host intel2 (user maik, 8 threads), work under `~/efs-taskFU/`: `fix` is `git archive` of the fixed tree, `base` is the same tree with `src/raft/raft.c`, `src/server/raft_host.c` and `include/efs/raft.h` taken from `wave77-base`. The new cases fail on `base` and pass on `fix`:

| Check | `wave77-base` sources | Fixed tree |
|---|---|---|
| `tests/test_raft.c` `test_snapshot_refused_image_keeps_import` (X queued, newer one-chunk Y refused with the host's refusal code, then `efs_raft_snap_imported(X)`; a retry of X's closing chunk) | FAIL: `Y was not queued`, `X installs after Y was refused`, `snapshot and applied index are X's`, `retry of X's closing chunk after the refusal installs X` (the base host's refusal code is BUSY, so the case is compiled with `K113_REFUSE=EFS_ERR_BUSY`) | pass |
| `tests/test_raft.c` `test_snap_imported_guards` (a leader refuses the import; a second call and a wrong index are `EFS_ERR_INVAL`) | FAIL: `a leader refuses the import` | pass |
| `tests/test_snap_import_pump.c` `test_hard_error_requests_resync` (log line, one resync request, no kick, importer dropped, a chunk at offset 0 accepted afterwards) | FAIL: `the failure is logged` | pass |
| `make`, `./tests/test_raft`, `./tests/test_snap_import_pump`, `tests/test_host_inbox.py`, `tests/test_raft_ack_window.py` | | rc 0 each |
| `scripts/tests_for.py server/raft_host --run`, `scripts/tests_for.py raft --run` | | rc 0, rc 0 |
| `tests/test_k22_snapshot_import.py` | | rc 0 |
| `make test` | | rc 0 (the first run failed `tests/test_tmpdir_hygiene.py` on my new pump case, a bare `mkstemp` and a `/tmp` path; fixed by `efs_test_path` and `efs_test_mkstemp`) |
| `tests/sanitizer_check.sh asan` | | rc 0 on the first run (before the tmpdir fix in the pump test) |
| `tests/sanitizer_check.sh tsan` | | first run: only `k37_segment_rebuild` failed (the known intermittent, not mine); the rerun after the tmpdir fix did not finish: see "Not verified" |

Reproduction: `git archive HEAD | ssh intel2 'mkdir -p ~/efs-taskFU && tar -x -C ~/efs-taskFU'`, then in the tree run the commands in the table. For the base column, replace the three source files with `git show wave77-base:<path>` and build `tests/test_raft` with `-DK113_REFUSE=EFS_ERR_BUSY` (edit the default in the test).

## Not verified

- The production snapshot window (512 MiB; the test uses 1 MiB), where an import of ten million keys takes minutes: whether the leader's retained log then covers the import time is open.
- Distinct hosts and real network: all members here are loopback on one host. RDMA transport.
- A second and third import after the member is close to the leader's image (not needed in this test).
- The leader still re-sends the refused chunk 0 (up to one chunk) every heartbeat while the member imports an older image; unchanged cost, not measured.

- Round 2: the final ASan and TSan rerun on the tree with the pump test's tmpdir fix (intel2 stopped answering SSH during the TSan run). The earlier runs on the tree before that test-only fix: ASan rc 0, TSan only the known `k37_segment_rebuild`.
- Round 2: no cluster run (the K22 test passes on intel2, where the base passes too); the load that fails the base needs fstor007.
- Round 2: `efs_raft_snap_imported`'s `incl <= last_applied` refusal has no test of its own (it cannot be reached through the public API without a real apply); the guard cases cover a leader, a wrong index and a second call.

## Raw output

Outside Git: `~erbmi1/efs-artifacts/taskFT-20261110/` on fstor007 (the K22 runs, both gates, solo reruns; `SHA256SUMS` hash `a6aeb1ede044c081`), `~maik/efs-artifacts/taskFT-20261110/` on intel2 (reproduction sets and the `make test`/sanitizer runs; `SHA256SUMS` hash `f40f36887c59e147`) and on xorinox (`SHA256SUMS` hash `6b2775dd2a619f50`). Reproduction: `git archive` the source commit, build `all test-bins tests/faults/efsd` with the host's libfuse, `python3 tests/test_k22_snapshot_import.py` alone three times; counts are `grep -c` of the `raft-snap: import start|open|applied|installed` lines in node 3's log and `raft-snap: start` in both leaders' logs; the committed script `scripts/evidence/k113/k22_alone.sh SRCTREE OUTDIR` does exactly this (three runs, `grep -c` every 2 s because the test deletes its work directory on exit, waits while a `make_test_summary.sh` of the same user runs) and writes `summary.txt`.
