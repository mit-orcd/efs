# `.find` result cache ownership pilot (Task EC)

**Verdict: PASS (Nov 9), with one defect found and fixed (K101, blocks
alpha: no).** The `.find` result table, its mutex and its byte count are
private to a new `find_res.c` behind `find_res.h` with a compiled
interface test. The same unfamiliar question read on the ownership tree
needs 542 to 428 lines (−114), opens 852 to 445, 7 source files to 3, and
stops at one guide, one interface, one implementation file and the routed
tests. The before-reading found K101: an `.efs/find` operation could be
served a result fetched for another supplementary group set (F1), and a
replaced or evicted slot turned GETATTR into `EPERM` (F2); each operation
now reads the slot its own fetch filled before the mutex is released
(negative control: 3 of 3 runs fail before, pass after). node9900:
`make test` rc 0 on both trees, `tests_for.py client/efs_fuse --run` 56 commands rc 0, TSan and ASan `find_res_interface` PASS. Branch `claude/frosty-bartik-e0be55` on devel `df7d11371`
(wave 64). Not merged, not pushed.

- Purpose: the next ownership pilot of the
  [structure plan](structure-plan-20261102.md) (phase 3, ownership-pilots
  bullet: `g_find_res`), repeating the recipe of
  [Task DM](writeback-ownership-20261105.md) and
  [Task EB item 1](taskEB-review-followup-20261108.md): one unfamiliar
  question read before and after the table, its mutex and its byte count
  become private to one owner, measured by the reading-set method of
  [the baseline](reading-set-baseline-20261104.md).
- Source: devel `df7d11371`; before-half `ba72a20a4` (committed before
  any source change), ownership `111594fb4` (behaviour unchanged), K101
  fix `2b86f47e1`, then this page, the ledger and the graph. The node9900
  trees are `git archive` of `111594fb4` and `2b86f47e1` (`EFS_GIT_ID`
  unknown in an archive).
- Environment: node9900 (192 cpus, gcc,
  `PKG_CONFIG_PATH=/data1/efs/deps/libfuse/lib64/pkgconfig`, builds in
  `/tmp/efs-taskEC-own` and `/tmp/efs-taskEC-fix`,
  `TMPDIR=/tmp/efs-taskEC-tmp`), load 0.0–1.2; the Mac for the splice
  tests, the doc checks and the graph rebuild.
- Measurement: by the implementing agent, not a blind reader. A line is
  "needed" when the answer depends on it, "opened" when it was printed into
  the agent's context; ranges were located by the README, one grep or a
  graph `explain`.

## Scenario

"Can a `.find` result be served (a) to a caller with another uid or gid,
or (b) after its slot was reused for another term, or evicted, between the
caller's fetch and its use?" Not one of the Task DB baseline fixes, not
Task DM's or Task EB's question.

## Before (devel `df7d11371`, recorded in `ba72a20a4`)

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

## The interface (`src/client/efs_fuse/find_res.h`, 56 lines; 58 after the fix)

`struct efs_find_res`, `EFS_FIND_RES_N`, the table, `g_find_res_mu`,
`g_find_res_bytes`, the fetch counter, `find_res_caller` and
`find_query_ensure` are `static` in `find_res.c` (moved from `support.c`
L848–1009 and L1026–1046, and from `support.h`). The header opens with
owner, lock (taken before `vq_*`'s and `virtual_file_open`'s own
mutexes, never across an RPC), the fetch-then-use rule, lifetime and each
operation's answer:

| Operation | Caller | Was |
|---|---|---|
| `find_res_intern(dir, query, term, &q)` | LOOKUP (`lookup.c`) | `find_query_ensure`, then its own match and `vq_intern` under `g_find_res_mu` |
| `find_res_open(fi, ino, dir, query, term)` | OPEN (`open.c`) | `find_query_ensure`, then its own match, `vq_version_matches`, `virtual_file_open` under the mutex |
| `find_query_fill_stat`, `find_query_read` | GETATTR (`getattr.c`), READ without a handle (`open.c`) | unchanged names, moved |
| `find_res_clear()` | `efs_fuse_destroy` (`main.c`) | its own loop over the slots under the mutex |

`query_credentials` stays in `support.c` (stats uses it) and is now
declared in `support.h`. In `111594fb4` the bodies moved verbatim: the
same critical sections, the same answers (`-EAGAIN` on a LOOKUP or OPEN
miss, `-1` from GETATTR's), no reordering; the header stated the miss.
Callers outside `find_res.c` hold no lock and read no slot: `git grep
'g_find_res\|find_res_caller' -- src ':!src/client/efs_fuse/find_res.c'`
is empty.

Compiled test `tests/test_find_res_interface.c` (337 lines; 379 with the
K101 case) links the real `find_res.c`; the query RPC (text naming the
caller it was fetched for), the caller context and credentials, `vq_*`,
`virtual_file_open` and the stat helper are link-time definitions. Cases:
LOOKUP, OPEN, GETATTR and the fd-less READ serve the caller's fresh result;
`-ESTALE` when the version changed; fetch errors pass through and a failed
fetch never serves the table's bytes; another uid gets its own result and
no hint from the other's slot; 40 terms through 16 slots; the 64 MiB
budget evicts the oldest and refuses a larger result; a third concurrent
fetch is `-EBUSY` without an RPC; `find_res_clear` frees every slot (the
ASan leak check); four threads with distinct uids on shared and private
terms are served only their own result (the TSan entry). In `make test`
after `test_open_interface`; `tests/budgets.tsv` row 5 s,
`client/efs_fuse.find_res` (solo 0.02/0.02/0.02 s on node9900, 1.25×
rounded up to 5 s as the two earlier interface tests); the TSan and ASan
lists of `tests/sanitizer_check.sh`; `docs/how-it-works/testing.md`.

Tests re-pointed: `test_query_callback_affinity.py` slices the private
table from `find_res.c` instead of taking the struct from the header
(tag `.support` to `.find_res`); `test_lookup_errno.py`,
`test_attribute_authority_errno.py` and `test_open_interface.c` stub
`find_res_intern` / `find_res_open` instead of declaring a table;
`test_read_errno.py` unchanged. `tests/csrc.py` lists `find_res.c` and
`find_res.h`; `Makefile` `CLIENT_SRCS`.

## After (ownership tree `111594fb4`), same scenario, same counting

Answer from the header: (a) the fetch is reauthorized with uid, gid and
supplementary groups, the later match uses uid and gid only, and "between
the two the slot may have been replaced ... a miss": a result fetched for
another group set can be matched; (b) the operation reads the bytes it
matched under the mutex, an open file owns a copy, and a miss answers
`-EAGAIN` from LOOKUP and OPEN but `-1` from `find_query_fill_stat`
(`EPERM` to the application). `find_res.c` confirms both: the slot is
reused by directory and term for any caller, and the three match loops.

| File | Lines | Category | Needed |
|---|---|---|---|
| `src/client/efs_fuse/README.md` | 1–101 | guide: the `find_res.c` row and the Contract bullet | yes |
| `src/client/efs_fuse/find_res.h` | 1–56 | interface: owner, lock, fetch then match, the miss, answers per operation | yes |
| `src/client/efs_fuse/find_res.c` | 1–230 | private: struct, table, counter, `find_res_caller`, `find_query_ensure` (slot reuse), `find_query_len`, `find_query_read`, `find_res_intern`, `find_res_open`, `find_res_clear` | yes |
| `src/client/efs_fuse/find_res.c` | 231–242 | `find_query_ino` | no (12) |
| `src/client/efs_fuse/find_res.c` | 243–263 | `find_query_fill_stat` returns the length's `-1` | yes |
| `src/client/efs_fuse/getattr.c` | 247–251 | GETATTR returns it unchanged | opened to confirm the header; not needed (5) |
| grep the five operations outside `find_res.c` | 6 hit lines | navigation: one call site per caller | yes |
| grep `g_find_res\|find_res_caller` outside `find_res.c` | 0 hits | navigation | yes (one step, no lines) |
| `graphify explain client/efs_fuse.find_res` | 14 lines | navigation: guide, contract, implementation, 2 tests | yes |

Understand: opened 445 (was 852), needed 428 (was
542). Categories (needed): guide 101 (was 98), interface 56 (was 77),
implementation 251 (was 217: the moved operations), callers 0 (was 104),
navigation 20 (was 46). Source files opened: 3 (was 7);
`support.c`, `lookup.c`, `open.c`, `main.c` and `ll_run.c` are no longer
opened: the header states the copy rule, the fd-less READ and each
operation's miss answer.

Validate:

| File | Lines | Category | Needed |
|---|---|---|---|
| `tests_for.py client/efs_fuse.find_res` output, the two `budgets.tsv` rows | 4 | routing | yes |
| `tests/test_find_res_interface.c` | 1–337 | `.find_res`; the real `find_res.c`: per-caller results, errors, slots, budget, fetch limit, clear, concurrent callers | yes |
| `tests/test_query_callback_affinity.py` | 1–51 | `.find_res`; the publisher hint per caller and term, reauthorization | yes |
| grep `find_res` over `tests/` | 4 files, 3 stub lines outside the route | navigation: the stubs no longer hold a table | yes (3) |

Validate: opened 395 (was 120), needed 395 (was 120). Routed: 2 of 2
tests that touch the cache (was 1 of 5); one compiles the module. The rise
is the 337-line compiled test, as in Tasks DM and EB.

Dead ends: 0 (was 3).

Could the investigation stop at one guide, one interface, one
implementation file and the routed tests? **Yes.** No caller line is
needed; the 5 GETATTR lines were opened to confirm what the header states.

## K101 fix (`2b86f47e1`)

`find_query_ensure` returns with the cache mutex held and the slot it
stored; each operation reads that slot and ends with `find_res_done()`
(unlock, release the fetch counter). The second match is gone, so no
operation can be served a slot another caller's fetch replaced, and there
is no miss: LOOKUP, OPEN and GETATTR no longer answer `EAGAIN` or `EPERM`
for a concurrent fetch or an eviction. `find_res_caller` remains for the
publisher hint only. Lock order and the "never across an RPC" rule are
unchanged (the RPC precedes the insert; `vq_intern`,
`vq_version_matches` and `virtual_file_open` were already called under
the mutex); the fetch counter is now released after the copy instead of
before it.

Negative control, `same_term_pairs` (two threads, 3,000 OPEN plus 3,000
GETATTR each; same uid and gid with 1 and 2 supplementary groups, then two
uids, one term):

| Tree | Other group set's result served (per thread) | Misses (per thread) |
|---|---|---|
| `111594fb4` (3 runs) | 517/552, 617/639, 572/669 | 991/1066, 1092/1001, 1121/1106 |
| `2b86f47e1` (3 runs) | 0/0, 0/0, 0/0 | 0/0, 0/0, 0/0 |

Answer to the scenario after the fix, from the header: (a) never, (b)
never; an evicted slot cannot be observed by the operation that fetched.

Blocks alpha: no. Same uid, a race window between two critical sections,
virtual query files only, and fixed on this branch; no incident traced.

## Proof

| Check | Result |
|---|---|
| node9900 `make test`, ownership tree `111594fb4` | rc 0, 152 s (`own-make-test.log`) |
| node9900 `make test`, fix tree `2b86f47e1` | rc 0, 151 s (`fix-make-test.log`) |
| node9900 `python3 scripts/tests_for.py client/efs_fuse --run` (fix tree, after `make test`) | rc 0, 56 commands, 79 s (`fix-tests-for-fuse.log`) |
| node9900 `EFS_SAN_ONLY=find_res_interface bash tests/sanitizer_check.sh tsan` | PASS (1 run, 0 failed; `fix-tsan.log`) |
| node9900 `EFS_SAN_ONLY=find_res_interface bash tests/sanitizer_check.sh asan` | PASS (1 run, 0 failed; ASan + UBSan, leak check; `fix-asan.log`) |
| node9900 `./tests/test_find_res_interface` solo, fix tree | 0.02 / 0.02 / 0.02 s (`fix-find-res-solo.txt`) |
| node9900 negative control: fixed test against `111594fb4`'s `find_res.c` | aborts in `same_term_pairs` 3 of 3 (`negative-control-own-tree.txt`) |
| Mac: the four adjusted splice tests, `test_make_test_recipe.py`, `test_tests_for.py`, `check_k_register.py` | PASS (101 rows, 59 FIXED) |
| Mac: `sh scripts/rebuild-code-graph.sh`, `python3 tests/test_code_graph.py` | 18,529 nodes; 10 tests OK; `graphify explain client/efs_fuse.find_res` degree 5 (guide, contract, implementation, 2 tests) |
| Mac: `make docs-check`, `check_doc_links.py`, `status_layout.py` | pass (after regenerating `architecture-full.md` for the testing.md line), 0 dangling, nothing to move |

## Reproduce

```sh
# Mac or Linux, from the branch
python3 tests/test_query_callback_affinity.py && python3 tests/test_lookup_errno.py
python3 tests/test_attribute_authority_errno.py && python3 tests/test_read_errno.py
python3 scripts/tests_for.py client/efs_fuse.find_res
sh scripts/rebuild-code-graph.sh && python3 tests/test_code_graph.py && graphify explain client/efs_fuse.find_res
# node9900 (libfuse in /data1/efs/deps/libfuse)
export PKG_CONFIG_PATH=/data1/efs/deps/libfuse/lib64/pkgconfig LD_LIBRARY_PATH=/data1/efs/deps/libfuse/lib64
make -j48 all && make tests/test_find_res_interface && ./tests/test_find_res_interface
make test
python3 scripts/tests_for.py client/efs_fuse --run      # after make test (needs the test-long fault binaries)
EFS_SAN_ONLY=find_res_interface bash tests/sanitizer_check.sh tsan
EFS_SAN_ONLY=find_res_interface bash tests/sanitizer_check.sh asan
# negative control: the fixed test against the ownership tree's find_res.c
git show 2b86f47e1:tests/test_find_res_interface.c > /tmp/t.c && git checkout 111594fb4 -- src/client/efs_fuse/find_res.c
gcc -O3 -std=c99 -D_GNU_SOURCE -Iinclude -Isrc/common -Ideps/blake3 $(pkg-config --cflags fuse3) -Isrc/client \
    -o /tmp/t /tmp/t.c src/client/efs_fuse/find_res.c -lpthread && stdbuf -oL /tmp/t   # aborts in same_term_pairs
```

Raw output: node9900 `~erbmi1/efs-artifacts/taskEC-20261108/` (16 files: make, make test, tests_for and sanitizer logs, solo timings, the negative control),
`SHA256SUMS` sha256 `16652e55dae16c3a2146c1050a42f802473fee81907d9d6c7044e829655447d9`. `/tmp/efs-taskEC*` and the uploaded tars
removed.

## Not done

- One host (node9900); no full `make test-all` gate (the merge's job).
- The after-reader was the implementing agent, who wrote `find_res.h`; the
  before-half was committed before any source change.
- `find_query_read` is reached only with a NULL file handle, which libfuse
  never passes; it was moved, not removed.
- `efs_fuse_destroy` (now `find_res_clear`) still leaves the byte count
  as it was (harmless at unmount, kept verbatim).
- The K counts line of the alpha checklist (58 FIXED after wave 64) is
  left for the merge; `check_k_register.py` passes with K101 (59 FIXED).
