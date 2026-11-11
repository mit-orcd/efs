# K113: a member that rejoins by snapshot finishes under sustained load (Task FT)

**Status**
- Source commit: `2f65444c6` (code and tests; branch `claude/beautiful-sinoussi-b869f2`, on `k113-base` `cdd20885c` = devel `fe81bb0e7` plus the K109 fix). Page, ledger entry and graph follow in later commits.
- Verdict: **PASS on the targeted test** (`tests/test_k22_snapshot_import.py`, unchanged: base 0 of 3, fixed tree 3 of 3 on fstor007, alone). Full-gate and sanitizer verdicts are in "Verification".
- Remaining failures: see "Verification". Not verified: production snapshot window, distinct hosts, RDMA (see "Not verified").
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
3. **Progress independent of the leader.** `host_snap_import_pump` runs in the pump cycle (`h->mu` held) after `drain_inbox`: one `snap_import_poll` per group, i.e. one `efs_kv_import_step` (at most 1024 keys written and 16384 records merged) per cycle. That bounds the work per cycle exactly as the leader-retry step did, so AppendEntries and heartbeats are served between steps. The pump's wait is zero while a finished import has work, so a step follows the previous one at once; when the KV refuses a batch (`BUSY`, an L0 stall) `import_stall` is set and the pump returns to its normal 5 ms wait: no spin, no new timeout and no sleep. A leader retry of the closing chunk of the same image still steps and installs through the old path; both run on the pump thread.
4. **Convergence, with this test's numbers (fstor007, fixed tree).** The leader snapshots every 1 to 2 s; one import is export+open 0.26 to 0.33 s plus 208 to 209 steps for a 212k-key diff, `import applied total_ms` 1.3 to 1.4 s (`step_max_us` 42 to 120 ms), so one import (about 1.7 s) is of the order of the snapshot interval and the old restart-per-snapshot scheme could never finish; the new scheme does not care. After X is installed the log after X was still retained (window 1 MiB past each snapshot, 1 to 2 s of load), so the member caught up 0 to 1 s later without another snapshot: `resume->install` 4 to 5 s and `install->caught` 1 s in all three runs, 2 to 3 imports per run (the two groups, one extra). A second and third import were not needed here, so their cost is not measured; the local image is then close to the leader's, so export and open stay at 0.3 s and the diff shrinks to the keys written since X. Stop condition A did not apply. It would apply at production caps if writers outrun the import; not measured.
5. **Fail closed.** `import_block` is set when the import starts (GC thread) and cleared only when it is installed, fails or is cancelled, so a half-imported member never applies log entries or serves reads (`host_query_capable` refuses while the import state is not IDLE) and does not claim a caught-up applied index: the Raft core is not told anything until the import is complete. Votes: the core's last log is unchanged until the install. A failed step discards the importer and restarts cleanly (wave 74 path, test `test_failed_importer_is_discarded` unchanged).

## Code

- `src/server/raft_host.c`: `snap_import_active`, the offset-0 guard in `host_snap_chunk`, `host_snap_import_pump` (called from `host_pump`), `import_stall`, `host_pump_poll_timeout`.
- `src/raft/raft.c`, `include/efs/raft.h`: `imp_*` fields, `efs_raft_snap_imported`.
- Callers checked before editing: `snap_import_poll` had one caller (`host_snap_chunk`), `snap_import_cancel` two (`host_snap_chunk` and the cancel-during-open test); `host_pump_poll_timeout` one (`host_pump`).

## Tests

| Test | Base | Fixed tree |
|---|---|---|
| `tests/test_snap_import_pump.c` (new, runs `tests/test_snap_import_host.c` first, unchanged): a newer image's chunk 0 (closing and first) is refused and X's importer, `.part` and `import_block` stay; the pump steps without any leader retry and installs X (`efs_raft_snap_imported` wrapped); Y is accepted afterwards; the KV refuses five batches: one step per pump call and the pump keeps its normal wait | fails (the guard removed: refused chunk cancelled the import); does not build on the base (new symbols) | pass |
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

(see the section filled in below)

## Not verified

- The production snapshot window (512 MiB; the test uses 1 MiB), where an import of ten million keys takes minutes: whether the leader's retained log then covers the import time is open.
- Distinct hosts and real network: all members here are loopback on one host. RDMA transport.
- A second and third import after the member is close to the leader's image (not needed in this test).
- The leader still re-sends the refused chunk 0 (up to one chunk) every heartbeat while the member imports an older image; unchanged cost, not measured.

## Raw output

Outside Git: `~erbmi1/efs-artifacts/taskFT-20261110/` on fstor007 (the K22 runs and the gate), `~maik/efs-artifacts/taskFT-20261110/` on intel2 and xorinox, each with `SHA256SUMS`. Reproduction: `git archive` the source commit, build `all test-bins tests/faults/efsd` with the host's libfuse, `python3 tests/test_k22_snapshot_import.py` alone three times; counts are `grep -c` of the `raft-snap: import start|open|applied|installed` lines in node 3's log and `raft-snap: start` in both leaders' logs; the committed script `scripts/evidence/k113/k22_alone.sh SRCTREE OUTDIR` does exactly this (three runs, `grep -c` every 2 s because the test deletes its work directory on exit, waits while a `make_test_summary.sh` of the same user runs) and writes `summary.txt`.
