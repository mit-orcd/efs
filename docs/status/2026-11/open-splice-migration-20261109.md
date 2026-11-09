# open.c splice tests become cases of the open.h interface test (Task ER)

**Verdict: PASS (Nov 9). Test change only; no K row, blocks alpha: no.
Of the six splice tests Task EB re-pointed at `open.c`'s private anchors,
two are migrated and removed (`test_open_busy.py`,
`test_open_retry_lock_budget.py`). Two are migrated except for edges
`open.h` does not expose, and keep only those edges
(`test_open_lease.py`: two `release.c` edges; `test_fuse_lease_retention.py`:
four edges that need allocation-failure injection, a sleep hook or a
private-field write). Two are kept unchanged because their subject is
`append.c` (`test_append_reserve_budget.py`, `test_append_burst_clock.py`).
`tests/test_open_interface.c` gains 14 case runs (13 functions) through
`open.h` alone. Seven deliberate breaks of `open.c`, one or more per case
class, each fail the binary (six assertion aborts and one hang killed by
`timeout`); the restored tree passes.** node9900: `make test` rc 0 (127 s), `tests_for.py client/efs_fuse.open --run` (8 rows) rc 0, ASan and TSan `open_interface` PASS. Branch
`claude/great-kilby-1755e2` on devel `33cd0a839` (wave 65). Merged into devel in wave 67 (Nov 9).

- Purpose: the [structure plan](structure-plan-20261102.md) says splice
  tests move to compiling the real module through its interface once one
  exists. [Task EB](taskEB-review-followup-20261108.md) made the
  open-reference table private to `open.c` behind `open.h` and added the
  compiled test, but re-pointed six splice tests at `open.c`'s private
  anchors (`struct efs_open_ref {`, `g_open_mu`, `open_ref_find`) instead of
  migrating them. Linked item: W171 (structure plan, phase 3 follow-up).
- Source: devel `33cd0a839`. Changes: `tests/test_open_interface.c` (lease
  stub generalised, 13 case functions), `tests/test_open_lease.py` and
  `tests/test_fuse_lease_retention.py` trimmed, `tests/test_open_busy.py` and
  `tests/test_open_retry_lock_budget.py` removed with their `Makefile` lines
  and `tests/budgets.tsv` rows, the `test_open_interface` row re-measured,
  the `.open` (10 to 8) and `client/efs_fuse` (56 to 54) counts in
  `src/client/efs_fuse/README.md` and `src/client/README.md`. No `open.c`,
  `open.h` or other source change. `open.h` was not widened.
- Which six: the EB page's table of tests that "re-declare or re-splice"
  the table: `test_open_lease.py`, `test_fuse_lease_retention.py`,
  `test_open_busy.py`, `test_open_retry_lock_budget.py`,
  `test_append_reserve_budget.py`, `test_append_burst_clock.py`. The brief
  also named `test_stop_dirty_drain.py`. It splices `efs_control_drain` and
  only stubs `efs_open_retained`; it takes nothing from `open.c`, so it is
  not one of the six and is unchanged.

## The six tests, their edges and their fate

The case functions in `tests/test_open_interface.c` are named after the
test they come from. The binary has no case filter (Task EG's filter covers
the sequence binaries), and this task adds none. Its PASS line names the
case groups.

| Test (lines before → after) | Edge it proved (anchor, assertion) | Case(s) | Fate |
|---|---|---|---|
| `test_open_busy.py` (89 → removed) | splices the table from `struct efs_open_ref {` plus the acquire functions. A refused admission is retried outside the stripe (4 begins, stripe free at each wait), also for the pending CLOSE a reopen proves first. Unknown OPEN answers are resumed on one identity (1 begin, 3 resumes). A completed rejection is final (no row). An unknown OPEN past the caller's deadline is kept on its exact identity with the inherited deadline unchanged. A contender on a held stripe gives up at the deadline without an identity. An I/O failure retains the row | `open_busy_admission`, `open_busy_verdicts`, `open_busy_deadline` | migrated, removed |
| `test_open_retry_lock_budget.py` (51 → removed) | declares its own row, `g_open_refs` and `g_open_mu`, and splices `efs_open_drain_pending`. A held stripe makes the retry answer BUSY at the deadline without touching the edge or losing the row; a later pass drains it; an unadmitted row ends the pass (`EFS_ERR_AGAIN`). Negative control: a plain `pthread_mutex_lock` blocks | `open_retry_lock_budget` (through `efs_open_retry_pass`: one CLOSE begin for the unadmitted row, not a rescan) | migrated, removed |
| `test_open_lease.py` (134 → 95) | table slice from `struct efs_open_ref {`, `g_open_refs`, `g_open_hash`. Two opens take one HOLD; a failed HOLD leaves `n == 0, close_pending`; the stripe serializes OPEN against OPEN and against an in-flight CLOSE; a FileID-carrying open fetches nothing; 20,000 hashed rows; plus `release.c`'s virtual-inode release and its CLOSE admission wait | `open_lease_shared_and_failed_hold`, `open_lease_edge_serialization`, `open_lease_hashed_rows` | migrated except the two `release.c` edges, which `open.h` does not expose (they are `efs_fuse_release_ino`'s, `release.h`); the test keeps those two |
| `test_fuse_lease_retention.py` (303 → 170) | table slice, `open_ref_link`/`unlink` on hand-made rows, `retry_order`/`retry_turn` written directly, `#define calloc` and `#define usleep` hooks. Ambiguous CLOSE verdicts retain the row and a reopen proves them; retry order publishes a newly allocated CLOSE (and an adoption) ahead of an unadmitted one; every identity gets a turn; the retry thread drains and stops; adoption NET, STALE and revocation; the combined OPEN\|CLEAR edge (first CREATE, existing owner, lost reply, refused admission, proven and late refusal, spent deadline, INVAL). Negative control: the pre-fix `retry_order` scheduling key | `fuse_lease_retention_close_verdicts`, `_retry_order` (edge and adoption runs), `_retry_turns`, `_retry_thread`, `_adoption`, `_combined` | migrated except four edges `open.h` does not expose, which the test keeps: (1) a failed row allocation hands off no token (needs `calloc` failure injection); (2) a competing OPEN waits at the stripe while an adoption reply is withheld (needs the `usleep` hook to know the opener reached the stripe); (3) an adoption retried after refused admission releases the stripe while waiting (needs the `usleep` hook; `efs_open_adopt` sleeps with `usleep`, not `efs_client_opid_wait`); (4) a completed BUSY adoption keeps the row (the test ends it by writing `adoption.result`) |
| `test_append_reserve_budget.py` (140, unchanged) | its own row and `open_ref_find`; splices `open.c`'s three `efs_open_append_*` operations beside `append.c`'s `append_reserve_offset`, `append_publication_wait`, `append_publish_burst` | none new: its `open.c` share (handoff time, publishing flag) is the existing append case | kept: the subject is `append.c`'s reservation loop, which `open.h` does not expose |
| `test_append_burst_clock.py` (61, unchanged) | splices the two burst operations and `append.c`'s `append_publish_burst`; patches `r->append_burst_ms=done_ms;` for its old-timing control | the existing append case (`/* ... append_burst_clock ... */`): the age restarts at `done_ms` (6299 does not publish, 6300 does), a failed drain publishes again | kept: `append_publish_burst` (the done time measured after the flush) is `append.c`'s |

Follow-ups that would let the kept edges go: an interface test for
`release.c` (the two `test_open_lease.py` edges), and an allocation-failure
or wait seam in `open.c` for the four retention edges. Neither is done
here: the brief says not to widen `open.h` for a test.

## How the cases stand in for the splices

The test links the real `open.c` and nothing else of the client, as before.
The lease stub now answers each edge kind with a chosen verdict: OPEN,
CLOSE (also per inode), and CLEAR. It can refuse admission (N begins,
optionally only for one flag set, or every CLOSE begin of one inode), leave
an OPEN unknown (N resumes or forever), make a non-OK answer a completed
verdict, make STALE a completed revocation, and give the combined edge a
proven or late refusal. A gate holds a resume in its RPC so the main thread
can show the stripe is held. Admission waits go through the existing
`efs_client_opid_wait` stub, which asserts that the watched inode's stripe
is free. The private fields the splices read are observed through
`efs_client_ino_is_open`, `efs_open_retained`, `efs_open_close_unadmitted`,
the return codes and the stub's per-kind counts of begins, resumes and wire
sends. Times are real (`CLOCK_MONOTONIC`) and bounded by the cases' own
deadlines (20 to 1000 ms). No sleep was added; the retry-thread case waits
for `efs_open_retained()` to reach 0 with `sched_yield`.

## Deliberate breaks (node9900, each reverted)

`perl` edits one line of `open.c`, rebuilds, runs under `timeout 30`, then
restores the file. Raw: `deliberate-breaks.txt`.

| Case class | Break in `open.c` | Result |
|---|---|---|
| `open_busy` | `acquire_fileid_mode`: `if(rc!=EFS_ERR_BUSY \|\| !admission_busy)break;` → `break;` (no retry after a refused admission) | `open_busy_admission` aborts (line 337, `acquire == EFS_OK`) |
| `open_retry_lock_budget` | `efs_open_drain_pending`: `wb_lock_budget(edge)` → `pthread_mutex_lock(edge)` (the old unbounded stripe wait) | hangs on the held stripe; `timeout` kills it (rc 124) |
| `open_lease` | `acquire_fileid_run`: unlock the stripe around `efs_open_lease_resume_wait` | `open_lease_edge_serialization` aborts (line 292: the stripe is free during the OPEN RPC) |
| `fuse_lease_retention` order | `efs_open_release_pending`: `r->retry_order = efs_open_retry_order(r);` → `(void)efs_open_retry_order(r);` (the old key the Python control rejected) | `fuse_lease_retention_retry_order` aborts (line 477: 42 still retained, 43 tried first) |
| `fuse_lease_retention` adoption order | `efs_open_retry_order`: drop the adoption term | `fuse_lease_retention_retry_order` aborts at line 477, on the adoption run (the edge run before it does not use that term) |
| `fuse_lease_retention` turns | `efs_open_drain_pending`: `selected->retry_turn = ++counter;` → `++counter;` | `fuse_lease_retention_retry_turns` aborts (line 495: 43 starved) |
| `append_burst_clock` | `efs_open_append_burst_published`: drop `r->append_burst_ms=done_ms;` | the append case aborts (line 675: 6299 publishes) |

The restored tree passes: `restored rc=0`.

## Budget

`./tests/test_open_interface`: three solo runs on node9900, 0.20, 0.20 and
0.20 s (load 2.2; Task EB measured 0.02 s before these cases). 1.25 × 0.20
= 0.25 s, rounded up to 5 s, which is the current value. The row stays at 5
s; only its note and description changed. The two removed tests' rows
(30 s each) are deleted. No other row was touched.

## node9900 results

| Check | Result |
|---|---|
| `./tests/test_open_interface`, three solo runs | PASS, 0.20 / 0.20 / 0.20 s |
| kept tests: `test_open_lease.py`, `test_fuse_lease_retention.py`, `test_append_burst_clock.py`, `test_append_reserve_budget.py` | PASS each |
| `make test -j32` | rc 0, 127 s |
| `python3 scripts/tests_for.py client/efs_fuse.open --run` | rc 0, 8 rows (was 10) |
| `EFS_SAN_ONLY=open_interface tests/sanitizer_check.sh asan` | VERDICT: PASS (build 62 s) |
| `EFS_SAN_ONLY=open_interface tests/sanitizer_check.sh tsan` | VERDICT: PASS |
| deliberate breaks (7) | each fails; restored tree passes |
| Mac: `test_make_test_recipe.py`, `test_tests_for.py`, `check_guide_counts.py`, `check_doc_links.py`, `status_layout.py`, `make docs-check` | PASS |

## Reproduce

```sh
# on a Linux host with libfuse 3 (node9900: EFS_FUSE_PREFIX=/data1/efs/deps/libfuse,
# PKG_CONFIG_PATH=$EFS_FUSE_PREFIX/lib64/pkgconfig, LD_LIBRARY_PATH=$EFS_FUSE_PREFIX/lib64)
make tests/test_open_interface && ./tests/test_open_interface
for i in 1 2 3; do /usr/bin/time -f '%e s' ./tests/test_open_interface >/dev/null; done
python3 tests/test_open_lease.py && python3 tests/test_fuse_lease_retention.py
python3 tests/test_append_burst_clock.py && python3 tests/test_append_reserve_budget.py
make test
python3 scripts/tests_for.py client/efs_fuse.open --run
EFS_SAN_ONLY=open_interface bash tests/sanitizer_check.sh asan
EFS_SAN_ONLY=open_interface bash tests/sanitizer_check.sh tsan
# a deliberate break, e.g. the old retry-order key, then restore:
cp src/client/efs_fuse/open.c /tmp/open.c.orig
perl -0pi -e 's/^        r->retry_order = efs_open_retry_order\(r\);$/        (void)efs_open_retry_order(r);/m' src/client/efs_fuse/open.c
make tests/test_open_interface && ./tests/test_open_interface   # aborts in fuse_lease_retention_retry_order
cp /tmp/open.c.orig src/client/efs_fuse/open.c
```

Prerequisites: gcc with ASan/UBSan and TSan runtimes (node9900: GCC 11.5.0),
Python 3. Cleanup: the build tree only (`make clean`). The sanitizer gate
deletes its log directory on PASS.

Environment: node9900 (ORCD, 192 CPUs, shared with seven other tasks),
GCC 11.5.0, tree shipped as a tar of the worktree (sha256
`afb9a70870a84066e2ec61699a69cd92a3605817a6b46ff8b36a77599b20bd59`, the
tested tree. Only this page, the ledger, the two guide counts and the
`test_open_interface` row's note and description changed after it). Built
under `/tmp/efs-taskER` with `TMPDIR=/tmp/efs-taskER-tmp`; no daemon, port,
mount or cluster store is used by any of these tests.

Raw output: `node9900:~erbmi1/efs-artifacts/taskER-20261109/` with
`SHA256SUMS` (sha256 of `SHA256SUMS`: `dadf00a101b545af490b575f910d909a2eef141dcf8c116bd9252f1115982759`).

## Limitations

- The kept edges stay in two splice tests (above). They still slice
  `open.c` from `struct efs_open_ref {`, so a rename of that anchor still
  breaks them.
- The interface cases see a row's count and flags only through `open.h`'s
  answers. The Python tests' `n == 1`, `leased` and `generation == 100`
  checks become behavioural checks: the number of closes needed before the
  last, wire sends per kind, and whether a CLOSE was sent.
- The edge-serialization case starts the second opener and releases the
  first. Like the Python test it replaces, it does not prove that the second
  opener had reached the stripe before the release. The trylock of the held
  stripe does prove the RPC runs under it.
- Not run: `make test-all`, valgrind (none on node9900), fstor007.
