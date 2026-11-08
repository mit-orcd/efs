# Review follow-up: open-reference ownership, coverage tags, guides in the graph (Task EB)

**Verdict: IN PROGRESS (Nov 8). This page's before-half is recorded here
before any source change; the after-half, the five items' results and the
proof follow in later commits.** Branch `claude/task-eb-review-feedback-eba909`
on devel `3f9110d52` (wave 63). Not merged, not pushed. No K row; blocks
alpha: no.

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
