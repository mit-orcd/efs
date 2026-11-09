# `.find` result cache ownership pilot (Task EC)

**Verdict: IN PROGRESS. Before-half recorded (Nov 9) on devel `df7d11371`
(wave 64), committed before any source change.** The after-half, the code
change, the compiled test and the proof follow in later commits of this
branch.

- Purpose: the next ownership pilot of the
  [structure plan](structure-plan-20261102.md) (phase 3, ownership-pilots
  bullet: `g_find_res`), repeating the recipe of
  [Task DM](writeback-ownership-20261105.md) and
  [Task EB item 1](taskEB-review-followup-20261108.md): one unfamiliar
  question read before and after the table, its mutex and its byte count
  become private to one owner, measured by the reading-set method of
  [the baseline](reading-set-baseline-20261104.md).
- Measurement: by the implementing agent, not a blind reader. A line is
  "needed" when the answer depends on it, "opened" when it was printed into
  the agent's context; ranges were located by the README, one grep or a
  graph `explain`.

## Scenario

"Can a `.find` result be served (a) to a caller with another uid or gid,
or (b) after its slot was reused for another term, or evicted, between the
caller's fetch and its use?" Not one of the Task DB baseline fixes, not
Task DM's or Task EB's question.

## Before (devel `df7d11371`)

Answer found by the before-reading:

(a) Never to another uid or primary gid: every reader (`lookup.c`
L137–146, `open.c` L841–850, `find_query_len` and `find_query_read` in
`support.c`) matches `find_res_caller(i)` (uid and gid of `ll_ctx()`),
the directory and the term under `g_find_res_mu`. **But the key omits the
supplementary groups the server authorized with**: `query_credentials`
(`support.c` L706–718) sends up to `EFS_QUERY_GROUPS` (128) groups, and
`find_query_ensure` reuses the slot of any caller with the same directory
and term (L911–918). The fetch and the match are two critical sections (the
mutex is never held across the RPC), so a concurrent fetch by a caller with
the same uid and gid but other supplementary groups can replace the slot
in between, and the first caller is served the second caller's result:
names visible only through a group the first caller does not hold. Same
user, narrow window; a confidentiality defect against the group rule.

(b) Never another term's bytes: the text is read in the same critical
section that matched the term, and an open descriptor owns a copy
(`virtual_file_open`, `support.c` L447–470; READ with a file handle
serves only that copy, `open.c` L921–931). But a slot reused or evicted
between fetch and match is a miss, and the callers disagree on what a miss
means: LOOKUP answers `-EAGAIN` (`lookup.c` L147), OPEN answers `-EAGAIN`
with the comment "Eviction between ensure and pin must not look empty"
(`open.c` L840), while **GETATTR of a query file not pinned by the kernel
gets `find_query_len`'s `-1`, returned unchanged by `find_query_fill_stat`
as errno 1, `EPERM`** (`support.c` L960–969, L1032–1034, `getattr.c`
L248): an authority error invented for a transient miss.
`find_query_read`'s miss is 0 bytes (EOF), but it is reached only with a
NULL file handle, which libfuse never passes (`ll_run.c` L317).

| File | Lines | Category | Needed |
|---|---|---|---|
| `src/client/efs_fuse/README.md` | 1–98 | guide: `support.c` row ("`.efs` query results", holds `g_find_res`/`g_find_res_mu`), Contract bullet | yes (98) |
| `src/client/efs_fuse/support.h` | 1–137 | interface: contract (L24–25, L34–39), `struct efs_find_res`, `EFS_FIND_RES_N`, the `extern`s, `find_res_caller`, `find_query_*` | 77 (L1–49, 63–78, 117–128) |
| `src/client/efs_fuse/support.c` | 848–997 | implementation: table, mutex, byte count, work gate, `find_res_caller`, `find_query_ensure` (hint lookup, fetch, slot reuse, 64 MiB eviction), `find_query_len`, `find_query_read` | yes (150) |
| `src/client/efs_fuse/support.c` | 1028–1046 | `find_query_fill_stat` returns the length's `-1` | yes (19) |
| `src/client/efs_fuse/support.c` | 835–847, 999–1027, 1047–1060 | `virtual_access`, `find_query_ino`, `find_dir_fill_stat`, DAC | no: overshoot (56) |
| `src/client/efs_fuse/support.c` | 447–470 | `virtual_file_open` copies the bytes | yes (24) |
| `src/client/efs_fuse/support.c` | 670–690 | `vq_version_matches` (L675–685) | 11 of 21 |
| `src/client/efs_fuse/support.c` | 706–735 | `query_credentials` sends the supplementary groups (L706–718) | 13 of 30 |
| `src/client/efs_fuse/lookup.c` | 120–160 | LOOKUP: fetch, match, `vq_intern` under the mutex, `-EAGAIN` on a miss (L129–148) | 20 of 41 |
| `src/client/efs_fuse/open.c` | 820–870 | OPEN: fetch, match, version check, copy under the mutex, `-EAGAIN` on a miss (L821–854) | 34 of 51 |
| `src/client/efs_fuse/open.c` | 905–960 | READ serves the descriptor's copy (L917–932); the `fi == NULL` fallback to `find_query_read` (L947–957) | 27 of 56 |
| `src/client/efs_fuse/main.c` | 100–125 | `efs_fuse_destroy` frees the slots under the mutex (L112–119), leaves the byte count | 8 of 26 |
| `src/client/efs_fuse/getattr.c` | 236–256 | GETATTR: pinned size, else `find_query_fill_stat` (L237–251) | 15 of 21 |
| `src/client/efs_fuse/ll_run.c` | 305–320 | READ always passes `fi` | no for the question; confirms the fallback is unreachable (16) |
| grep `find_query_\|find_res\|vq_version_matches\|virtual_file_open` over the module, outside the table | 30 hit lines | navigation: the three caller files and GETATTR | yes (30) |
| grep `query_credentials`, `snapshot` in `open.c`, `efs_fuse_read_ino(` | 16 hit lines | navigation | yes (16) |
| `graphify query "find result cache g_find_res ownership"` | 54 nodes shown of 491 | navigation | no: dead end (start nodes `result()`, `find()` in unrelated tests; ~60 lines) |

Understand: opened 852, needed 542. Categories (needed): guide 98,
interface 77, implementation 217 (`support.c`: table and operations 169,
`virtual_file_open` 24, `vq_version_matches` 11, `query_credentials` 13),
callers outside the owner 104 (`lookup.c` 20, `open.c` 61, `main.c` 8,
`getattr.c` 15), navigation 46. Source files opened: 7 (`support.h`,
`support.c`, `lookup.c`, `open.c`, `main.c`, `getattr.c`, `ll_run.c`).

Validate (`tests_for.py client/efs_fuse.support` routes 6 rows; the
cache's tests are found by grep, three of them routed elsewhere):

| File | Lines | Category | Needed |
|---|---|---|---|
| `tests_for.py client/efs_fuse.support` output, `graphify explain client/efs_fuse.support` | 6 + 14 | routing | yes (20) |
| grep `find_res\|find_query_\|EFS_FIND_RES` over `tests/` | 5 files, 12 hit lines | navigation | yes (17) |
| `tests/budgets.tsv` | 4 rows of the found tests | routing: `.support`, `.lookup`, `.namespace`, `.open` | yes (4) |
| `tests/test_query_callback_affinity.py` | 1–51 | `.support`; splices the table text and `find_query_ensure` from the module, takes the struct from the header with `csrc.hblock`; asserts the publisher hint is kept per caller and term and every fetch reauthorizes | yes (51) |
| `tests/test_lookup_errno.py`, `tests/test_attribute_authority_errno.py` | grep hits | `.lookup`, `.namespace`; each declares its own one-slot `g_find_res`, `g_find_res_mu`, `find_res_caller` to compile a spliced handler | yes (10) |
| `tests/test_read_errno.py` | grep hit | `.open`; stubs `find_query_read` | yes (1) |
| `tests/test_open_interface.c` | 93–109 | `.open`; defines the table, the mutex and three functions as link stubs for `open.c` | yes (17) |

Validate: opened 120, needed 120. Five tests re-declare or splice the
table; none compiles `support.c` or matches a result against a caller;
no test asks (a) or (b).

Dead ends: 3 (the `graphify query`, `support.c`'s neighbouring DAC and
`.find` directory stat ranges, `ll_run.c` for the question itself).

Could the investigation stop at one guide, one interface, one
implementation file and the routed tests? **No.** The header exports the
table, the mutex and the per-slot caller check; three files outside the
owner hold their own critical sections over the slots (LOOKUP, OPEN,
unmount) and a fourth (GETATTR) reaches the miss through the owner's
`-1`; the answer to (b) is three different errors for one event, found
only by opening every caller; the routed tests miss four of the five
tests that touch the table and none exercises the real one.

## Findings to carry

- F1 (a): a result fetched for one supplementary group set can be served
  to a caller with the same uid and gid and another set.
- F2 (b): GETATTR answers `EPERM` for a result evicted between fetch and
  match; LOOKUP and OPEN answer `EAGAIN` for the same event.
- `find_query_read` is reached only with a NULL file handle (not by
  libfuse); `efs_fuse_destroy` frees the slots but leaves
  `g_find_res_bytes` (harmless at unmount).
