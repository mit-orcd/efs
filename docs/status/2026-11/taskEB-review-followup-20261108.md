# Review follow-up: open-reference ownership, coverage tags, guides in the graph (Task EB)

**Verdict: PASS (Nov 8). All five items done. (1) The open-reference table
and its mutex are private to `open.c` behind an `open.h` contract and a
compiled test; the same question read on the new tree needs 640 to 543
lines (−97), opens 1,202 to 683, 9 source files to 4, and stops at one
guide, one interface, one implementation file and the routed tests. (5)
The caller-side change (the stop refusal names what remains) was made
from the two contracts and the routed tests without opening `open.c` or
`writeback.c`. (2) Catalogue rows carry several tags, a command is listed
once, a misspelled component is refused (exit 2) while a real one without
tests falls back with the note. (3) The 13 `src/**/README.md` guides are
documents in the graph and a component node per catalogue tag answers
`graphify explain <tag>` with guide, contract, implementation and tests.
(4) Status pointers sit on top of the plan and the pilot page.** node9900:
`make test` rc 0, `tests_for.py client/efs_fuse --run` (55 rows) rc 0,
TSan `open_interface` PASS; `python3 tests/test_code_graph.py` 10 OK. No K
row; blocks alpha: no. Branch `claude/task-eb-review-feedback-eba909` on
devel `3f9110d52` (wave 63). Merged into devel in wave 64 (Nov 8). Status Nov 9: remaining failures none; active run none; next action none on this page, W171 continues on the [structure plan](structure-plan-20261102.md).

- Purpose: act on the five suggestions of the Nov 8 external review of devel
  against its Nov 6 review (`cb51d1cd6`), which found the write-back
  ownership pilot ([Task DM](writeback-ownership-20261105.md), the `ready`
  race fixed by [Task DT](writeback-ready-20261107.md)) a convincing
  reduction of coupling: (1) repeat the ownership extraction on the
  open-reference table and its mutex, (2) finish the per-component test
  coverage tags, (3) bring the component guides into the code graph, (4)
  make the merged status of the pilot obvious without rewriting its
  history, (5) test whether a caller-side change can stop at the interface.
  [Structure plan](structure-plan-20261102.md) phase 3.
- Source: devel `3f9110d52`; before-half `ee60d35a4` (recorded before any
  source change); item 1 `ae74a589c`, item 2 `9956632ec`, item 3
  `41f0d056d`, item 4 `a5a536f78`, item 5 `b0d7eaff7`, graph `78e8f2230`,
  then this page and the ledger, then the graph again (this page's commit
  corrects route counts in `src/client/efs_fuse/README.md`, which item 3
  made a graph input). The node9900 tree is `git archive` of
  `b0d7eaff7` (`EFS_GIT_ID` unknown in an archive).
- Environment: node9900 (192 cpus, gcc, `PKG_CONFIG_PATH=/data1/efs/deps/libfuse/lib64/pkgconfig`,
  build in `/tmp/efs-taskEB`, `TMPDIR=/tmp/efs-taskEB-tmp`), load 0.2–0.7;
  the Mac for the splice tests, the doc checks and the graph rebuild
  (graphifyy 0.9.97, `PYTHONHASHSEED=0`).
- Measurement: by the implementing agent, not a blind reader. The before
  half counts the lines this session opened while orienting on devel
  `3f9110d52`, before the design was fixed; the after half repeats the same
  question on the changed tree with the same counting rule (a line is
  "needed" when the answer depends on it; "opened" when it was read).

## Item 1: scenario

"A last close whose CLOSE edge could not be confirmed leaves a retained
open-reference row (`n == 0`, `close_pending`). (a) Does the inode still
count as open for the cache evictor's pin rule and for GETATTR's open-fd
path? (b) Which fields of the row may another area change meanwhile, under
which lock? (c) What removes the row, and on which thread?" Not one of the
Task DB baseline fixes and not Task DM's question.

Answer found by the before-reading: (a) yes: `efs_client_ino_is_open` is
`open_ref_find(ino) != NULL` under `g_open_mu`, so a retained row pins the
inode in the stage evictor and sends GETATTR down the open-fd path although
no descriptor is open; `open.h` does not say so (`stage_evict.c`'s pin rule
comment says "no fd has it open"). (b) `append.c` reads and writes the
row's `append_burst`, `append_burst_ms`, `append_handoff_ms` and
`append_publishing` under `g_open_mu` in three critical sections of its
own, with the row found through the exported `open_ref_find`; `open.c`
clears `leased` under the inode stripe. (c) `efs_open_release_pending_run`
unlinks and frees the row once the CLOSE edge is proven, on the releasing
thread, on the next OPEN of the inode (`efs_open_acquire_fileid_run` calls
`efs_open_release_pending` first) or on the retry thread's
`efs_open_drain_pending`.

## Item 1: before (devel `3f9110d52`)

| File | Lines | Category | Needed |
|---|---|---|---|
| `src/client/efs_fuse/README.md` | 1–92 | guide | yes (open row, Contract bullet on `open.h` globals) |
| `src/client/efs_fuse/open.h` | 1–92 | interface: contract, `struct efs_open_ref` (16 fields), `extern g_open_mu`, `open_ref_find` | yes |
| `src/client/efs_fuse/open.c` | 1–160 | table, hash, `open_ref_find/link/unlink`, `efs_open_note`, `efs_open_set_leased`, `efs_open_retry_order`, `efs_close_note` | yes |
| `src/client/efs_fuse/open.c` | 171–177, 232–238 | `efs_open_release_pending_run`: find, then unlink and free once proven | yes |
| `src/client/efs_fuse/open.c` | 178–231 | the lease edge body between them | no: dead end (edge, not the row) |
| `src/client/efs_fuse/open.c` | 243–265 | `efs_open_release_pending` (publishes the retry key), `efs_open_close_unadmitted` | yes |
| `src/client/efs_fuse/open.c` | 266–283 | `wb_lock_budget` | no |
| `src/client/efs_fuse/open.c` | 285–336 | `efs_open_drain_pending`: selection, turn counter, unadmitted rescan | yes |
| `src/client/efs_fuse/open.c` | 357–367 | `efs_open_acquire_fileid_run`: release pending first, then note | yes |
| `src/client/efs_fuse/open.c` | 340–356, 368–410, 478–560 | lease resume wait, FileID fetch, adopt, pending creates | no: overshoot |
| `src/client/efs_fuse/open.c` | 670–682 | `efs_client_ino_is_open` | yes |
| `src/client/efs_fuse/open.c` | 665–669, 683–690 | neighbours | no |
| `src/client/efs_fuse/append.c` | 297–302 | `append_publication_wait` reads `append_publishing`, `append_handoff_ms` under `g_open_mu` | yes |
| `src/client/efs_fuse/append.c` | 285–296, 303–310 | neighbours | no |
| `src/client/efs_fuse/append.c` | 480–518 | `append_publish_burst`: two critical sections on the row's append fields | yes |
| `src/client/efs_fuse/append.c` | 470–479 | neighbours | no |
| `src/client/efs_fuse/append.h` | 12–16 | "append fields in `struct efs_open_ref` (open.h) are read and updated under `g_open_mu`" | yes |
| `src/client/efs_fuse/append.h` | 1–11, 17–41 | rest | no |
| `src/client/efs_fuse/release.c` | 29–50 | RELEASE: `efs_close_note`, then prove the edge, spin on `efs_open_close_unadmitted` | yes |
| `src/client/efs_fuse/release.c` | 71–75 | control drain runs `efs_open_retry_pass` | yes |
| `src/client/efs_fuse/release.c` | 1–28, 51–70, 76–100 | flock unlock, force path, drain loop | no |
| `src/client/efs_fuse/getattr.c` | 260–268 | the open-fd path gated on `efs_client_ino_is_open` | yes |
| `src/client/efs_fuse/getattr.c` | 252–259, 269–270 | neighbours | no |
| `src/client/stage_evict.c` | 16 | pin rule 2 comment ("no fd has it open") | yes (grep hit) |
| `src/client/client_internal.h` | 232 | `efs_client_ino_is_open` declaration | yes (grep hit) |
| grep `g_open_mu\|open_ref\|retained\|pthread_mutex_t` over the module | 58 hit lines | navigation | 14 |
| grep `g_open_mu\|open_ref` in `open.c` | 61 hit lines | navigation | 61 |
| grep the table symbols over `src/client`, `tests` | 80 hit lines | navigation | 20 (the 9 `append.c` hits, the file list) |
| `graphify query "open-reference table mutex ownership plan"` | 68 nodes shown of 232 | navigation | no: dead end (start nodes `reference()`, `table()` in unrelated tests) |

Understand: opened 1,202, needed 640. Categories (needed): guide 92,
interface 92, implementation 273 (`open.c`), callers outside the owner 88
(`append.c` 45, `append.h` 5, `release.c` 27, `getattr.c` 9,
`stage_evict.c` 1, `client_internal.h` 1), navigation 95. Source files
opened: 9.

Validate (`tests_for.py client/efs_fuse.open` routes 6 rows; the table's
tests are found by grep, two of them routed elsewhere):

| File | Lines | Category | Needed |
|---|---|---|---|
| `tests_for.py client/efs_fuse.open` output, `budgets.tsv` grep | 12 | routing | yes |
| `tests/test_open_lease.py` | 1–134 | tagged `.release`, found by grep; splices the table text from `pthread_mutex_t g_open_mu`, takes `struct efs_open_ref` from the header with `csrc.hblock`; asserts a failed HOLD leaves `n == 0, close_pending`; its own `efs_client_ino_is_open_test` | 41 |
| `tests/test_fuse_lease_retention.py` | 1–130, 60 grep hit lines | `.open`; same splice; retained rows across ambiguous replies, `open_ref_link/unlink` called directly | 60 |
| `tests/test_open_busy.py` | 1–89 | `.open`; same splice; a deadline keeps the exact unknown OPEN retained | 20 |
| `tests/test_open_retry_lock_budget.py` | 1–51 | `.open`; own `struct efs_open_ref`, `g_open_refs`, `g_open_mu`; splices `efs_open_drain_pending` | 15 |
| `tests/test_append_reserve_budget.py` | 1–120 | `.append`, found by grep; own `g_open_mu`, `open_ref_find`, row with the append fields; splices `append_publication_wait`, `append_publish_burst` | 15 |
| `tests/test_append_burst_clock.py` | 1–58 | `.append`, found by grep; own row and `open_ref_find`; splices `append_publish_burst`, patches one of its lines by text | 15 |
| `tests/test_read_errno.py`, `test_pending_create_cleanup.py`, `test_open_retry_fairness.py` | not opened | `.open`; grep shows no table symbol | 0 |

Validate: opened 654, needed 178. Six tests re-declare or re-splice the
table; none compiles `open.c`; three take the struct from the header, so
the struct cannot move without touching them. No test asks the (a)
question (retained row counts as open) and none drives `append.c`'s row
updates against the real table.

Dead ends: 3 (`open.c` 178–231, the `graphify query`, `release.c` 51–70).

Could the investigation stop at one guide, one interface, one
implementation file and the routed tests? **No.** The header exports the
whole mutable row, the table mutex and the lookup; `append.c` holds three
critical sections on the row; (a) is answered only by `open.c`'s body and
the evictor's comment contradicts it; the routed tests miss two of the
table's tests and none exercises the real table.

## Item 5: the caller-side change to attempt after item 1

"`efs_control_drain` (`release.c`) refuses a stop with `stop-refused:
unresolved writes or lease releases remain`; make the line say how many
retained releases and how many write-back jobs remain." The agent is to
make it from `open.h`, `writeback.h` and the routed tests, and record
whether `open.c` or `writeback.c` had to be opened and why.

## Item 1: the interface (`src/client/efs_fuse/open.h`, 117 lines, was 92)

`struct efs_open_ref`, `g_open_mu`, `g_open_refs`, `g_open_hash` and
`open_ref_find` are private to `open.c` (the struct sits above the mutex,
so the splice tests cut one block). The header opens with owner, lock,
lifetime and each operation's answer; new or newly stated:

| Operation | Answer |
|---|---|
| `efs_open_retained()` (new) | rows waiting for a confirmed last close; walks the table under its mutex, for the stop path |
| `efs_open_append_handoff(ino, *publishing)` (new) | the row's handoff time (0 without a row) and whether a publication runs |
| `efs_open_append_burst_note(ino, now)` (new) | 1 when this append write must publish first (32 writes or one second); 0 while a publication runs or without a row |
| `efs_open_append_burst_published(ino, rc, done_ms)` (new) | ends it: `EFS_OK` restarts the age and opens a 100 ms handoff window, failure leaves the burst full |
| `efs_client_ino_is_open(ino)` | 1 while a row exists, retained rows included (stated; it was the before-reading's finding) |
| lifetime | a reopen proves the pending CLOSE first, waits for an unanswered edge within its deadline, fails with its error otherwise |

Behaviour preserved: the three append operations are `append.c`'s critical
sections moved verbatim (`append_publication_wait` L297–302,
`append_publish_burst` L480–518 before); `efs_open_retained` is new and
called only by the stop refusal (item 5). No timeout, budget or sleep
changed. `tests/test_append_burst_clock.py` still patches the moved line
by text (`r->append_burst_ms=done_ms;`) for its old-timing negative control.

Callers: `append.c` had the only direct accesses outside `open.c` (three
critical sections); `release.c` and `getattr.c` were already on operations.
`append.h` names the operations instead of the fields. Tests:
`test_open_lease.py`, `test_fuse_lease_retention.py` and `test_open_busy.py`
start their slice at the struct instead of taking it from the header with
`csrc.hblock`; `test_append_reserve_budget.py` and
`test_append_burst_clock.py` splice the three operations from `open.c`
beside their own row; `test_open_retry_lock_budget.py` unchanged.

Compiled test `tests/test_open_interface.c` (249 lines) links the real
`open.c` with the lease edges, the FileID fetch and 40 helpers from other
files as link-time stubs (the lease stub answers the next OPEN or CLOSE
edge with a chosen rc): no row (every operation answers nothing, nothing
is created); two opens and two closes (one HOLD, one CLOSE, the row gone);
a CLOSE edge that fails leaves the row retained, open to the evictor and
GETATTR, counted by `efs_open_retained`, refused to a reopen with the
edge's error, drained by `efs_open_retry_pass` once the edge answers (and
`EFS_ERR_BUSY` from a pass whose deadline is past); the append bookkeeping
on a real row; 3,000 retained rows across colliding buckets drained in at
most two passes; four threads opening and closing distinct inodes beside a
counting thread (the TSan entry). In `make test` after `test_wb_interface`,
`tests/budgets.tsv` (5 s, `client/efs_fuse.open`; solo 0.02/0.02/0.02 s on
node9900), the TSan and ASan lists of `tests/sanitizer_check.sh`
(`open_interface`), `docs/how-it-works/testing.md`.

## Item 1: after (tree `ae74a589c`), same scenario, same counting

Answer from the header: (a) yes, stated ("a retained row counts as open:
the inode stays pinned in the cache evictor and GETATTR keeps its open-fd
path until the CLOSE is proven"); (b) no other area touches a row: callers
"never lock the table or read a row", `append.c` reaches the burst state
only through the three operations, grep finds no table symbol outside
`open.c`; (c) the retry thread, the next RELEASE or the next OPEN proves
the edge and `efs_open_release_pending` frees the row. The compiled test
asserts (a), (c) and the append operations on the real table.

| File | Lines | Category | Needed |
|---|---|---|---|
| `src/client/efs_fuse/README.md` | 1–98 | guide | yes |
| `src/client/efs_fuse/open.h` | 1–117 | interface: owner, lock, lifetime, operations and answers | yes |
| `src/client/efs_fuse/open.c` | 10–89 | private: struct, mutex, list, hash, find/link/unlink | yes |
| `src/client/efs_fuse/open.c` | 103–180 | `efs_open_note`, `efs_open_set_leased`, `efs_open_retry_order`, `efs_close_note` | yes |
| `src/client/efs_fuse/open.c` | 250–257 and 6 lines of 182–263 | `efs_open_release_pending_run` frees the proven row | yes (14) |
| `src/client/efs_fuse/open.c` | rest of 182–263 | lease edge body | no: dead end (68) |
| `src/client/efs_fuse/open.c` | 264–285 | `efs_open_release_pending`, `efs_open_close_unadmitted` | yes |
| `src/client/efs_fuse/open.c` | 303–362 | `efs_open_drain_pending` selection | no: the header states who drains (60) |
| `src/client/efs_fuse/open.c` | 689–785 | `efs_client_ino_is_open`, `efs_open_retained`, the three append operations | yes |
| `src/client/efs_fuse/append.c` | 297–299, 476–484 | the three call sites | opened to confirm the grep; not needed (12) |
| grep `g_open_mu\|open_ref_find\|struct efs_open_ref` over `src/` outside `open.c` | 0 hits | navigation | yes (one step, no lines) |
| grep `efs_open_\|efs_close_note\|efs_client_ino_is_open` outside `open.c` | 18 hit lines | navigation: every caller on an operation | yes |
| `graphify explain client/efs_fuse.open` | 13 connections, 20 lines | navigation: guide, contract, implementation, 10 tests | yes |

Understand: opened 683 (was 1,202, −43 %), needed 543 (was 640, −97
lines, −15 %). Categories (needed): guide 98 (was 92), interface 117 (was
92), implementation 290 (was 273: the struct moved in, the four
operations), callers 0 (was 88), navigation 38 (was 95). Source files
opened: 4 (was 9); `append.h`, `release.c`, `getattr.c`, `stage_evict.c`
and `client_internal.h` are no longer needed: the header answers (a) and
(b), and the only caller-side lines opened were the 12 of `append.c` that
confirm the grep.

Validate:

| File | Lines | Category | Needed |
|---|---|---|---|
| `tests_for.py client/efs_fuse.open` output, the ten `budgets.tsv` rows | 12 | routing (10 rows, was 6: the second tags of item 2) | yes |
| `tests/test_open_interface.c` | 1–249 | `.open`; the real `open.c`: retained row counts as open and retained, drained by the pass, the append operations | yes |
| `tests/test_open_lease.py` | 1–134 | `.open` (second tag); a failed HOLD leaves `n == 0, close_pending` | 41 |
| `tests/test_fuse_lease_retention.py` | 1–130, 60 grep hit lines | `.open`; retained rows across ambiguous replies | 60 |
| `tests/test_open_busy.py` | 1–89 | `.open`; a deadline keeps the exact unknown OPEN retained | 20 |
| `tests/test_open_retry_lock_budget.py` | 1–51 | `.open`; own row; the drain's lock budget | 15 |
| `tests/test_append_reserve_budget.py`, `tests/test_append_burst_clock.py` | 1–120, 1–58 | `.open` (second tag); the moved operations on their own row | 15 + 15 |
| `tests/test_read_errno.py`, `test_pending_create_cleanup.py`, `test_open_retry_fairness.py` | not opened | `.open`; no table symbol | 0 |

Validate: opened 903 (was 654), needed 427 (was 178). Routed: 10 of 10,
none found outside the route (was 6 routed plus 3 by grep); one compiles
the module and asks the (a) question. The rise is the 249-line compiled
test.

Dead ends: 2 (was 3): the lease edge body and the drain selection in
`open.c`.

Could the investigation stop at one guide, one interface, one
implementation file and the routed tests? **Yes.** No caller line is
needed; the 12 call-site lines were opened to confirm a grep that already
returned no table symbol outside the owner. Lines needed fell by 97 (640
to 543), lines opened by 519; validation lines rose by 249 because the
routed tests now include a compiled one, as in Task DM.

## Item 5: the caller-side change, measured

Change made (`b0d7eaff7`): `efs_control_drain`'s refusal line is now
`stop-refused: unresolved writes or lease releases remain; mount retained
(retained-releases=%u wb-pending=%d)` from `efs_open_retained()` and
`efs_wb_pending(0)`.

| Read | Lines | Needed for |
|---|---|---|
| `src/client/efs_fuse/open.h` 64–66 | 3 | `efs_open_retained`: what it counts, that it walks the table under its mutex (so it belongs on the stop path, not per operation) |
| `src/client/efs_fuse/writeback.h` 36–39 | 4 | `efs_wb_pending(0)`: queued plus in-flight jobs, never blocks past the pool mutex |
| `tests/test_open_interface.c` 175–188 | 14 | the retained count's cases (0 without a row, 1 after a failed CLOSE, 0 once proven) |
| `src/client/efs_fuse/release.c` 92–98 | 7 | the line to change (its function was read in the before-half) |
| `tests/test_stop_dirty_drain.py` 95 | 1 | the routed test splices `efs_control_drain` and stubs its callees: one stub added |

Opened 29 lines, no implementation file. **The implementation did not have
to be opened.** Caveat: the agent had written both contracts in this task,
so "unfamiliar" holds for the caller (`release.c`'s drain loop was read
once, in the before-half), not for the owners; a reader who did not write
`open.h` would still find the walk-under-the-mutex caveat and the
never-blocks rule only in the headers, which is where this change needs
them. `test_stop_dirty_drain.py` PASS with the stub (Mac and node9900).

## Item 2: coverage tags

`scripts/tests_for.py` (`9956632ec`): column 6 of `tests/budgets.tsv` may
hold several comma-separated tags; `select` lists a command once however
many of its tags match; `resolve` falls back from a dotted component with
no row of its own to its parent only when `src/<dir>/<component>.c`, `.h`
or a directory of that name exists, and refuses any other name with exit
2 (`tests_for: unknown component meta/meta_apply.publsh: no catalogue row
and no src/meta/meta_apply/publsh.{c,h} or directory (misspelled? ...)`);
a bare subsystem without rows keeps its exit 1. Before, `meta/meta_apply.publish`
and `meta/meta_apply.publsh` both printed the parent's rows with the same
notice. `tests/test_make_test_recipe.py` checks the tag syntax (non-empty,
no spaces, each once). Second tags, from Task DM's and DD's notes:

| Row | Tags now | Why |
|---|---|---|
| `test_wb_runtime.py` | `client/efs_fuse.write,client/write` | splices `src/client/write.c` and `writer_state.c` |
| `test_stop_dirty_drain.py` | `client/write,client/efs_fuse.release` | splices `efs_control_drain` |
| `test_k42_peer_stop_open.py`, `test_stop_progress.py`, `test_k54_member2_unmount.py`, `test_k90_unmount_enospc.py` | `client/efs_fuse.main,client/write` | live mounts writing through `efs-fuse` |
| `test_open_lease.py` | `client/efs_fuse.release,client/efs_fuse.open` | splices the table |
| `test_append_reserve_budget.py`, `test_append_burst_clock.py` | `client/efs_fuse.append,client/efs_fuse.open` | splice the moved operations |

Routes after: `client/efs_fuse` 55 rows (was 51 on the README), `.open`
10 (was 6), `.release` 4 (was 3), `.append` 5, `.main` 10, `.write` 2,
`.writeback` 4. `tests/test_tests_for.py` (in `make test`, row `harness`)
pins selection, deduplication, the fallback and the refusal on a synthetic
catalogue and on the real one. Limit: a dotted tag whose component is a
function prefix rather than a file (`server/raft_host.read_index`) is
known only through its rows; `server/raft_host.publish` has none and is
refused, since the prefix map in the structure plan is not machine-readable.
Not done: a tag-by-tag audit of every row; the second tags above are the
ones the Task DM and DD notes named.

## Item 3: component guides in the graph

`tools/graphify/scope.json` adds `src` to `document_roots` (the only
`.md` files under `src/` are the 13 `README.md` guides) and names
`tests/budgets.tsv` as the catalogue input. `scripts/build-code-graph.py`
adds one component node per catalogue tag and per `.c` file in a guided
directory (label = the `tests_for.py` tag), with `guided_by` (the
directory's README), `contract` (`.h`), `implemented_by` (`.c`) and
`tested_by` edges (one per tag of a catalogue row, `evidence_method`
`test-catalogue`, the row's line as location). Nothing is inferred from
source text. `tests/test_code_graph.py` checks the writeback, open and
`server/raft_host.read_index` components and the guide documents;
`tools/graphify/README.md` documents the query. Graph: 18,139 to 18,479
nodes, 35 to 48 documents; `guided_by` 165, `contract` 43,
`implemented_by` 137, `tested_by` 599; rebuild 12 s.

Before (`3f9110d52` graph):

```
$ graphify explain src/client/efs_fuse/writeback.h
  Degree: 1        --> writeback.h [contains]
$ graphify query "write-back pool owner contract implementation tests"
  Start: ['owner', 'write()', 'tests/test_publication_writeback.py', 'tests()', 'pool_size()', ...] | 259 nodes found, TRUNCATED: showing 63
```

After (this branch):

```
$ graphify explain client/efs_fuse.writeback
  Degree: 7
  --> src/client/efs_fuse/README.md   [guided_by]
  --> src/client/efs_fuse/writeback.h [contract]
  --> src/client/efs_fuse/writeback.c [implemented_by]
  --> tests/test_wb_interface.c, tests/test_write_queue.py,
      tests/test_wb_worker_budget.py, tests/test_wb_drain_budget.py [tested_by]
$ graphify explain client/efs_fuse.open      Degree: 13 (guide, contract, implementation, 10 tests)
$ graphify explain server/raft_host.read_index   Degree: 19 (guide src/server/README.md, 18 tests, no file of its own)
```

Limit: a free-text `graphify query` is seeded by keyword scores and still
returns hundreds of nodes at depth 2 (265 for the sentence above; a query
for the tag itself seeds the component node first but expands through
`contains`), so the compact starting set is `graphify explain <tag>`, not
`query`. `tested_by` is a catalogue route, not measured coverage.

## Item 4: merged status

Quoted status blocks at the top of [the structure plan](structure-plan-20261102.md)
and [the pilot page](writeback-ownership-20261105.md) point to wave 60,
Task DT and wave 61; the historical text is unchanged. The W171 row of
`docs/status/README.md` says merged.

## Proof

| Check | Result |
|---|---|
| node9900 `make test` (final tree, archive of `b0d7eaff7`) | rc 0, 126 s wall, 4,025 log lines (`final-make-test.log`) |
| node9900 `python3 scripts/tests_for.py client/efs_fuse --run` | rc 0, 55 commands (`final-tests-for-fuse.log`; needs the `test-long` fault binaries built first) |
| node9900 `EFS_SAN_ONLY=open_interface bash tests/sanitizer_check.sh tsan` | PASS (1 run, 0 failed; `open_interface.out`) |
| node9900 `./tests/test_open_interface` solo | 0.02 / 0.02 / 0.02 s |
| node9900 item 1 tree (`ae74a589c` state) `make test` | rc 0 |
| Mac: the six adjusted splice tests, `test_stop_dirty_drain.py`, `test_tests_for.py`, `test_make_test_recipe.py` | PASS |
| Mac: `python3 tests/test_code_graph.py` on the rebuilt graph | 10 tests OK |
| Mac: `python3 scripts/status_layout.py`, `python3 scripts/check_doc_links.py` | nothing to move, 0 dangling |

## Reproduce

```sh
# Mac or Linux, from the branch
python3 tests/test_open_lease.py && python3 tests/test_fuse_lease_retention.py && python3 tests/test_open_busy.py
python3 tests/test_append_reserve_budget.py && python3 tests/test_append_burst_clock.py && python3 tests/test_tests_for.py
python3 scripts/tests_for.py meta/meta_apply.publsh; echo rc=$?        # 2, refused
python3 scripts/tests_for.py meta/meta_apply.publish                    # the parent's rows, note on stderr
sh scripts/rebuild-code-graph.sh && python3 tests/test_code_graph.py && graphify explain client/efs_fuse.writeback
# node9900 (libfuse in /data1/efs/deps/libfuse)
export PKG_CONFIG_PATH=/data1/efs/deps/libfuse/lib64/pkgconfig LD_LIBRARY_PATH=/data1/efs/deps/libfuse/lib64
make -j48 all && make tests/test_open_interface && ./tests/test_open_interface
make test                                                              # rc 0
python3 scripts/tests_for.py client/efs_fuse --run                     # needs the test-long fault binaries (make test-long's prerequisites)
EFS_SAN_ONLY=open_interface bash tests/sanitizer_check.sh tsan
```

Raw output: node9900 `~erbmi1/efs-artifacts/taskEB-20261108/` (21 files: the make, make test, tests_for and TSan logs of the item 1 and final trees),
`SHA256SUMS` sha256 `29bed80f971a9839e17ecea73859167f2e5472dc2f6d9ea5ffd4017a98ff0fe1`. `/tmp/efs-taskEB*` and the uploaded tars
removed.

## Not done

- One host (node9900); no full `make test-all` gate (other sessions use
  the host for gates); fstor007 not used.
- The after-reader and the item 5 reader were the implementing agent, not
  blind; the before-half was committed before any source change.
- `append.c`'s three operations moved verbatim; the append burst timing
  itself was not re-measured (its splice tests pass unchanged).
- Item 2 tagged the rows the Task DM and DD notes named, not every row.
- `open_interface` is on the ASan list of `tests/sanitizer_check.sh` but
  only the TSan entry was run (as Task DT did for `wb_interface`).
