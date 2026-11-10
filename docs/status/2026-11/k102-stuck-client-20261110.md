# K102: a client retries a removed inode's write-back and its stop refusal names nothing (Task FC)

**Verdict: PASS for the fix, `make test`, `tests_for` and ASan; TSan gate NOT clean (an unrelated `lim` race, see Results). K102 FIXED on branch `claude/optimistic-williams-2bb8ca`, blocks alpha: yes until the orcd-mini clean stop is rerun.**
The new test `tests/test_k102_removed_inode_flush.py` fails on the unfixed product code (4,992 retries of the removed inode and 4,992
`report-sync-fail ... held-unit-flush` lines in 62 s, then `stop-refused ... (retained-releases=0 wb-pending=0)`) and passes with the fix
(stop succeeds in 0.4 to 1.0 s, mount exit 0, no retry line). The loop of the orcd-mini client was the stop drain's own 60 s loop, not a
free-running thread, and it ended because its state was never retired, not because anything was retrying a lost cause on purpose: a
chunk whose inode the server had removed was re-dirtied by every failed flush, whatever the error. Not yet run: the orcd-mini stop itself.

Purpose: Task FC, diagnose and fix K102 from the saved state of Task EW's O1 run
(`~erbmi1/efs-artifacts/taskEW-20261109/o1-poisoned-client/`, read only). K102 itself is recorded on Task EW's unmerged branch; this page
proposes its row text below and edits no K row.

Tested source: devel `98e34842d` (product code identical to the candidate `9bb50fe79`: `git diff 9bb50fe79 98e34842d -- src include` is empty)
for every "before" run; the fix is commits `f3d75fcc8`, `b2d9082ae` and `1f81f8995` on this branch (final source commit `8735d7f9b`; `a513ce3e2` after it changes only guide counts in two READMEs), for every "after" run.
Build identity: both trees were uploaded to node9900 as a tar of the tracked files of a clean tree (not `git archive`; the same file set)
and built there (`BUILD=unknown`, no git metadata). Local and remote sha256 matched on every upload: unfixed tree
`2385cb56172dd4ca3c7e03b820f1fd61e877e408d6afd0f4f11b04ce11952602`, final gate tree
`69bc4e028dd9055f8d5818c8f7028482f0e2c6a0059b0cf6ef085df605f7f5f9`.

Environment: node9900 (192 cpus, load average 12 to 29 from other tasks during the runs), libfuse `/data1/efs/deps/libfuse`, private loopback
clusters of three members (`tests/faults/efsd`, node 3 at a 4 MiB quota) and two private `efs-fuse` mounts under `/tmp/efs-taskFC-tmp`,
RDMA transport auto (mlx5_0), buffered I/O. No orcd-mini state was touched.

## What the saved logs show

`client-node9900-after-stop.log` (the full log, 108,102 lines; `client-node9900-full.log` was cut at 22:35, before the failure):

- 22:33:56.965 and .991 `background-pressure ino=6980 rc=-3 retained`, again 22:36:21.830: the pressure thread flushed inode 6980, got NOT_FOUND and
  kept the state. Nothing else is logged about 6980 until the stop.
- 22:38:43.421 first `dcache-flush-fail ino=6980 stage=put rc=-3`, each followed by `report-sync-fail ino=0 stage=held-unit-flush ... rc=-3`;
  3,181 pairs, the last at 22:39:43.418; `stop-refused ... (retained-releases=0 wb-pending=0)` at 22:39:43.430. That is the 60 s stop window
  (`EFS_STOP_MS`, `src/client/stop_control.h`) to the second. Before and after it the client logs no `dcache-flush-fail`: the loop is not a standing
  background loop.
- 22:52:33 to 22:53:33 `client.sh stop` of the unmount: `unmount not clean: flush rc=-3` (`unmount.txt`, `force-discard.txt`), the second
  retrier below.

## Answers to step 2

**(a) What survived.** Dirty write-cache entries of inode 6980: `struct dcache_ent` rows with `dirty=1` and a body (`src/client/write.c`), re-dirtied by
the failed flush, plus the W77 publication unit of the multi-chunk write they belong to (`g_units`, `unit_chunk_state` is not settled while a chunk is
dirty), which is why every REPORT held the unit back (`report_units_shape` → `EFS_ERR_AGAIN`) and fell into the `held-unit-flush` stage. No ticket,
PUT window or write-back pool job: `wb-pending=0`. The field state itself cannot be inspected any more (the client was force-discarded); this is read
from the code and reproduced below. In the reproduction the entries are the partial chunks of the overwrite, each with a published base.

**(b) Who retries and what triggers it.** Three callers, none of them a loop of its own besides the first:
1. `efs_control_drain` (`src/client/efs_fuse/release.c`), the stop control's drain loop, 10 ms between passes, for 60 s: each pass calls
   `efs_dcache_drain_pending` → `efs_dcache_flush_ino(6980)` (prints `dcache-flush-fail`) and then `efs_client_report_dirty(1)` →
   `report_dirty_ino_complete` → `efs_dcache_flush_all` (prints `report-sync-fail`). About 53 pairs a second were logged (a pass takes about 8 ms).
2. `fuse_pressure_main` (`src/client/efs_fuse/append.c`), once per dcache pressure kick (`background-pressure ... retained`), and
3. `efs_fuse_destroy` (`main.c`), the unmount's own 60 s drain loop at a 200 ms cadence.

**(c) Why NOT_FOUND does not end it.** No code on the path looks at the code. `dcache_steal_dirty_budgeted` re-dirties the entry when the merge-base fetch
fails (`fetch_rc`), `flush_pipe_drain` → `dcache_flush_keep(put_ok=0)` re-dirties it when the PUT fails, and `dcache_flush_slot_inner_budgeted` does
the same, for every error. `dcache_flush_ino_pass` only copies the code into `pipe.rc`, `efs_dcache_flush_ino` prints it as `stage=put` (the label
covers the whole pass, fetch included) and returns it. In the reproduction the -3 comes from the merge-base fetch (`dcache_fetch_merge_base` →
`efs_client_pull_chunks_range_absent`, GETCHUNKS on the removed inode) for a partial chunk with a base; with `EFS_PUT_TICKET_DBG=1` the ticket admit
logged no failure. A full-chunk overwrite has no merge base and goes to the PUT, where the server's answer for a removed FileID is STALE (-14,
ticket not admitted), not NOT_FOUND. I could not prove from the saved log which of the two gave the field's -3; the ior hard workload writes partial
chunks, so the fetch is the likely source. The REPORT path has a related rule (`report_dirty_ino_run`: "NOT_FOUND here is not a missing inode ...
retry it like BUSY"), which is unaffected by the fix.

**(d) What stays false in the stop loop.** `data_rc == EFS_OK` (`efs_dcache_drain_pending` returns -3 on every pass) and `report_rc == EFS_OK`
(`efs_client_report_dirty(1)` returns -3 from the held-unit flush). The two counts the old message printed, `retained-releases` and `wb-pending`,
are not conditions of the loop's success test (`wb-pending` only delays a pass), and the return codes and the four counters the test does use were
never printed. The new message prints all of them (below); in a first iteration of the fix it showed `data-rc=-14 report-rc=-13 dcache-pending=2`,
which is how the STALE answer for the full chunks was found.

## Reproduction (`tests/test_k102_removed_inode_flush.py`, fast suite)

Three daemons (`tests/faults/efsd`), node 3 at 4 MiB, two mounts. Mount A publishes a 1 MiB file (eight chunks), the quota is filled through the real
refusal path as in `test_k90_unmount_enospc.py` with room left for about three chunk fragments, then A overwrites all but 200 bytes of the file with one
write: one W77 unit of eight chunks, the first and last partial with a published base; about three chunks land (unreported), the others are refused and
`close` returns ENOSPC with the rest dirty. Mount B removes the file. `efs-fuse --stop` on A must then succeed within 20 s. No new fault hook: the
refusal is the quota, which is the field's shape (partial chunks of a held unit whose publication did not complete). Before the fix:

```
AssertionError: stop of mount A refused (rc 1) after 62 s: 4992 retries of removed inode 4097, 4992 report-sync-fail held-unit-flush;
['stop-refused: unresolved writes or lease releases remain; mount retained (retained-releases=0 wb-pending=0)']; efs-fuse: stop control refused or unavailable (1); mount retained
```

and the mount log is 5,102 `dcache-flush-fail ino=N stage=put rc=-3` against 5,101 `report-sync-fail ino=0 stage=held-unit-flush ... rc=-3`, the field's pair.
After the fix: `K102 removed inode flush: 5 chunks retired once each, stop ok in 0.4 s, mount A exit 0, 0 retry lines PASS`.

## The fix

- `src/client/write.c`, `dcache_retire_gone(ino, ci, rc)`: when a flush of a chunk failed with NOT_FOUND or STALE, a fresh `efs_client_rpc_getattr` to the
  inode's owner decides. Only NOT_FOUND from it (the row is deleted; the server keeps the row of an unlinked inode while any lease is open, so an inode
  another client still holds open is never retired, and inode numbers are never reused) retires the chunk: `efs_dcache_drop`, one writeback error
  recorded through `wb_error_record_add` (the path `report-span-lost` and `report-stuck` already use; `fsync`/`close` of a descriptor return it, and D28
  keeps it while the client lives), one `dcache-inode-gone ino= ci=` line. Called from the four places a flush can fail: `dcache_flush_ino_pass` and the
  reclaim sweep (steal/fetch failure), `flush_pipe_drain` (PUT failure) and `dcache_flush_slot_inner_budgeted` (fetch and PUT failure). Any other code, and
  a NOT_FOUND or STALE for an inode that still exists, keeps the existing re-dirty and retry. No retry count, timeout or sleep changed.
- `src/client/efs_fuse/release.c`, `efs_control_drain`: the last pass's values are kept and `stop-refused` now reads
  `(retained-releases=%u wb-pending=%d passes=%u ownership-rc=%d data-rc=%d report-rc=%d report-pending=%llu report-reserved=%llu report-inflight=%llu dcache-pending=%llu)`,
  every condition the success test uses. `passes=0` says the deadline passed before a pass ran. `main.c` (unmount line "Same counts as stop-refused") and
  the `release.h` contract are reworded. `tests/test_stop_dirty_drain.py` compiles the real loop and asserts the named values; no other test matched the
  old text (`grep -rn stop-refused tests/`).

The task asked that STALE keep its existing handling. It still does for any inode that exists. The widening to a STALE on a confirmed-removed inode is
deliberate: the reproduction's full chunks answer STALE, not NOT_FOUND, and without it a file with both kinds of dirty chunk would still loop. It costs one
getattr per failed chunk flush that answers STALE or NOT_FOUND.

## Results

All on node9900 (final gate tree = commit `8735d7f9b` source; the raw files named are in the external directory below).

| Check | Tree | Result |
|---|---|---|
| `tests/test_k102_removed_inode_flush.py` | unfixed `98e34842d` | FAIL: stop refused after 62 s, 4,992 retries of the removed inode, 4,992 `report-sync-fail ... held-unit-flush`, `stop-refused (retained-releases=0 wb-pending=0)` (`k102-test-unfixed-final.txt`) |
| same test, three solo runs | fixed | PASS, 5 chunks retired once each, stop in 1.0 / 0.5 / 0.4 s, mount exit 0, 0 retry lines; 11.6 / 10.3 / 16.7 s per run (budget 25 s) |
| `make test` | fixed | rc 0 (`make-test-4.txt`; earlier rc 2 runs were before the harness stubs and guide counts, see git log) |
| `python3 scripts/tests_for.py client/efs_fuse --run` | fixed | rc 0 (`tests-for-4.txt`) |
| `bash tests/sanitizer_check.sh asan` | fixed | rc 0, PASS (59 run, 0 failed, 370 s) (`asan-4.txt`) |
| `bash tests/sanitizer_check.sh tsan` | fixed | **mixed**: PASS (31 run, 0 failed) once (`tsan.txt`, earlier tree), FAIL twice on the final tree (`tsan-4.txt`, `tsan-5.txt`): one report each, `d4_progress`, a data race of two FUSE write threads on the lazy-initialised `static uint64_t lim` in `efs_dcache_maybe_reclaim` (`src/client/write.c:9145` read, `:9146` write; reports in `tsan-race-logs/`) |
| `d4_progress` solo under TSan | fixed / unfixed | 3 of 3 PASS on each tree |
| full TSan | unfixed `98e34842d` | PASS (31 run, 0 failed, 606 s) (`tsan-base-full.txt`) |

TSan is therefore not a clean rc 0 on the final tree. The two reports are in `efs_dcache_maybe_reclaim`, whose lines this branch does not touch
(`git diff 98e34842d -- src/client/write.c` has no hunk there; the function is the pre-existing `if (!lim) lim = dcache_reclaim_limit();` benign-race
idiom). The single unfixed full run passed, so one run does not prove the race pre-exists under gate load, and the fixed full runs split 1 pass to 2
fails. The honest reading is: a pre-existing race in untouched code that the gate hits intermittently; not proven independent of the branch's timing.

## Reproduction commands

```bash
git archive --format=tar HEAD | ...   # upload; build as below on node9900, work under /tmp/efs-taskFC
export EFS_FUSE_PREFIX=/data1/efs/deps/libfuse PKG_CONFIG_PATH=/data1/efs/deps/libfuse/lib64/pkgconfig LD_LIBRARY_PATH=/data1/efs/deps/libfuse/lib64
make -j16 all tests/faults/efsd
python3 tests/test_k102_removed_inode_flush.py          # unfixed tree: AssertionError after about 100 s; fixed: PASS in about 11 s
python3 tests/test_stop_dirty_drain.py
make test
python3 scripts/tests_for.py client/efs_fuse --run
bash tests/sanitizer_check.sh tsan
bash tests/sanitizer_check.sh asan
```

Prerequisites: `/dev/fuse`, `fusermount3`, libfuse as above. The unfixed side: check out `98e34842d` (or `9bb50fe79`), copy only the new test file in.
Cleanup: the test removes its work directory under `TMPDIR`; remove the tree you uploaded.

Raw output (outside Git): `~erbmi1/efs-artifacts/taskFC-20261110/` on node9900, `SHA256SUMS` hash `e4fad67fdd1abac2644774753962be5fd87147c9e1fdec7812f52330c781fd08`. It holds the unfixed and fixed test
outputs, `make test`, `tests_for`, `tsan` and `asan` logs. The earlier unfixed runs there (`k102-test-unfixed.txt`, `-2.txt`) are of an earlier shape of
the test (fresh file, then a whole-file overwrite) that showed the retry loop but not the `held-unit-flush` pair; `-3.txt` is the final shape.

## Proposed K102 row text

FIXED on branch `claude/optimistic-williams-2bb8ca`, blocks alpha: yes until the orcd-mini clean stop is rerun; a flush that failed because the
server had removed the inode was re-dirtied like any other failure, so the stop drain retried it for its whole 60 s and refused with a message whose
printed counts were not the conditions that failed; the flush now confirms a removed inode with a fresh getattr, retires the chunk and records one
writeback error, and `stop-refused` prints every return code and counter the stop tests.

## Not done

- The orcd-mini stop was not rerun (the live cluster and its client are out of scope for this task); that is what keeps K102 at "blocks alpha: yes".
- No K row, checklist or decision file was edited; the row text above is for the merge of Task EW's branch.
- The recorded writeback error is not asserted end to end: a descriptor cannot exist for the removed inode in the test (an open lease keeps the row, so
  the inode is not retired). `wb_error_record_add` is the same call the existing `report-span-lost` path makes.
- `efs_dcache_drop` skips a stalled or writer-owned entry; `dcache_retire_gone` then still logs and returns, which only matters for an entry that is
  not dirty, so nothing re-enters the loop, and the state stays visible in `dcache-pending` of a later refusal.
- getattr is the only evidence of removal; it is the owner's authoritative answer (the REPORT repair pass relies on the same NOT_FOUND). A lagging
  owner is not modelled.
- TSan `d4_progress` race on `lim` in `efs_dcache_maybe_reclaim` (see Results): not fixed here (outside the task), a follow-up: make `lim` an atomic or `pthread_once`. The TSan gate was not obtained as rc 0 on the final tree.
- Which of fetch or PUT gave the field's -3 is not proven (see (c)).
