# Structure review: what an agent still has to read (Nov 10)

**Status (Nov 10): review only, no code or test changed; nine task briefs
prepared, none started.** Source: devel `cff8eb424`. Verdict: eight findings,
ranked below; the two largest costs are no longer in the source tree but in
the status records every task must read and edit. Remaining failures: none
(nothing was run). Active run: none. Next action: the user picks tasks from
[the brief list](structure-review-tasks-20261110.md). Merge state: branch
`structure-review-20261110`, unmerged.

Purpose: the W171 goal ([structure plan](structure-plan-20261102.md)) is
that an agent reads less before it finds a defect, fixes it or adds an
extension. This page measures where the reading still is after waves 53 to
68 and proposes the next steps. Every number below was taken from the tree
at `cff8eb424` with the command named beside it.

## Findings, largest first

| # | Finding | Measured | Proposed task |
| --- | --- | --- | --- |
| 1 | The K register is one 376 KB file that every fix task reads and edits | `docs/status/alpha-1.0-2plus1-checklist.md` 384,010 bytes, 2,312 lines; the 214 K-row lines are 224,817 bytes (mean 1,050, longest line 6,659 characters); K numbers are handed out by message between sessions | T1 |
| 2 | The ledger is one 172 KB file that every branch appends to at the same place | `docs/status/2026-11/ledger.md` 175,154 bytes in 207 lines (newest entries 5,033, 2,687, 1,255 characters); every branch of waves 64 to 68 conflicted on its insert point, 22 conflicts resolved by script | T2 |
| 3 | `src/client/write.c` is the defect hot spot and has no map | 9,812 lines, 289 function definitions, 30 file-scope state items, 9 mutexes; its guide is one line ("owns the dirty cache"); 49 tests splice it; K38, K102, K106, K110 and K32 are all in it | T3 |
| 4 | `src/server/raft_host.c` is mapped for one concern of about twenty | 26,249 lines, 655 function definitions; the [ReadIndex entry map](readindex-entry-map-20261108.md) covers 32; the split waits on the W123 decision | T4 |
| 5 | A log line does not lead to its owner through the graph | `graphify query "dcache-flush-fail stage=put rc=-3"` returns 613 nodes, truncated to 50, starting with unrelated `flush()`, `Stage`, `fail()`; `grep -rn dcache-flush-fail src` answers in one call; about 95 distinct diagnostic keys exist; the error codes (`rc=-13` and so on) are defined only in `include/efs/common.h` lines 111 to 155 | T5 |
| 6 | Host access, gate and merge procedure are in no tracked file | no page under `docs/how-it-works/` or `AGENTS.md` names the ORCD access path; every brief carried about 35 lines of host rules, and sub-sessions still lost time on them (Tasks EL and EI built against the system libfuse, EY lost ssh and opened its own masters, EZ picked busy hosts, the wave 68 gate failed a check that needs `.git`); the wave gate was re-derived by `sed` from the previous wave's scripts eight times | T6 |
| 7 | The interface tests cannot run or name one case | `tests/test_wb_interface.c` (279 lines), `test_open_interface.c` (723), `test_find_res_interface.c` (379) have no named case functions, all cases are inline in `main`; the [contract-to-case maps](contract-case-map-20261109.md) therefore show writeback.h 0 of 8 operations with a named case | T7 |
| 8 | No recipe for adding a test; the routing table predates component tags | `tests/` holds 625 files and no README; `docs/how-it-works/testing.md` is 577 lines about running gates; `developing.md` section 2 routes by topic to protocol pages, names no component tag or `tests_for.py` command, and still states a "~1000-line file cap" | T8 |

Not a finding: the component guides and the graph. `graphify explain <tag>`
returns 14 to 31 lines (670 to 1,995 bytes) for every component tried
(`server/raft_host`, `client/write`, `server/store`, `client/efs_fuse.open`,
`meta/meta_apply`, `meta/metadata`, `client/inode_rpc`), every `src`
directory has a guide (13 guides, 771 lines), and the entry files are small
(`AGENTS.md` 70 lines, `CLAUDE.md` 10, `docs/status/README.md` 48).

## What the findings mean

1. **Records before code.** A fix task today opens `explain` (2 KB), a guide
   (2 to 10 KB), an interface and an implementation section, and then must
   read and edit a 376 KB checklist and a 172 KB ledger to record the
   result. One file per K row and one file per ledger entry, with the
   tables and the ledger page generated, removes that reading, removes the
   merge conflicts and removes K-number collisions (a new row is a new
   file; the generator refuses a duplicate number).
2. **Maps before splits.** Task DO found that the `raft_host.c` split must
   follow a concern map, not the function prefix, and the two splits that
   landed did not reduce reading while the ownership pilots did. A concern
   map (which functions belong to which concern, which state and lock each
   concern owns) costs no code change, does not wait for W123, cannot
   conflict with the fix sessions now editing `write.c`, and is the input
   the later split or ownership extraction needs.
3. **Start from the symptom.** Defects arrive as a log line or an `rc`
   value. A generated index from diagnostic key to file, function and
   component tag, and the error-code table beside it, turns the first step
   into one lookup; the rule "graph before grep" should say so.
4. **Procedure in the tree.** What a sub-session needs to reach a host,
   build, gate and clean up is stable enough to be one page and one script.
5. **Validation by name.** Splitting the interface tests' `main` into
   named cases and giving them the case filter the seven sequence binaries
   already have lets a reader run and read one case.

## Deferred, with the reason

- **Ownership pilot for the PUT window in `write.c`** (`put_win_mu` and the
  four `put_win_*` functions, where K38 and K106 sit): a code change in the
  file five fix sessions are editing now. Brief T9 is written and marked
  "start only after the K38, K102, K106 and K110 fixes are merged".
- **Physical split of `raft_host.c` and `write.c`:** still the user's W123
  decision; T3 and T4 produce the map either split would follow.
- **More guides:** the thin guides (`src/meta/README.md` 50 lines for
  `metadata.c` 4,154, `txn.c` 2,145, `query_disk.c` 1,828; `src/client`
  43 lines for 18,376 lines of `write.c`, `inode_rpc.c`, `read.c`,
  `ops.c`) gain their symptom rows through T3, T4 and T5 rather than a
  separate rewrite.

## Order and conflicts

| Task | Touches | Can run with | Note |
| --- | --- | --- | --- |
| T1 K register per row | the checklist, `scripts/check_k_register.py`, new `docs/status/k/` | nothing else that edits the checklist | run at a quiet point; the converter is rerunnable so open branches can be converted at merge |
| T2 ledger per entry | `docs/status/2026-11/ledger.md`, new `docs/status/2026-11/ledger/` | nothing else that edits the ledger | same |
| T3 `write.c` map | new page, `src/client/README.md`, new `scripts/check_concern_map.py`, Makefile `docs-check` | T5, T6, T7, T8 | docs only |
| T4 `raft_host.c` map | new page, `src/server/README.md`, Makefile `docs-check` | T5, T6, T7, T8 | starts after T3 merges (uses its checker) |
| T5 diagnostics index | new script and page, `AGENTS.md`, Makefile `docs-check` | T3, T4, T6, T7, T8 | |
| T6 host and gate recipes | new page, new `scripts/wave_gate.sh`, one line in `testing.md` | all | no secrets; never reads `ACCOUNTS.txt` |
| T7 named interface cases | three `tests/test_*_interface.c`, the case maps in two guides | T3 to T6, T8 | check first that no open fix branch edits the same test |
| T8 test recipe and routing | new `tests/README.md`, `developing.md` section 2 | all | |
| T9 PUT-window ownership | `src/client/write.c`, new header and test | none of the K38/K102/K106/K110 fix branches | deferred |

Makefile `docs-check` gains one line in T1, T2, T3, T4 and T5; the merge
keeps all lines, as it did for Tasks DO and ED in wave 65.

## Method

- Sizes: `wc -lc` on the files named; K rows: `grep -E '^\| K[0-9]+ \|'`
  piped to `awk` for count and length.
- Function definitions: lines in column 0 matching a C definition head and
  not ending in `;`, control keywords removed:

  ```sh
  grep -nE '^[A-Za-z_][A-Za-z0-9_ \*]*[ \*]([a-z0-9_]+)\([^;]*$' FILE.c
  ```
 An approximation: it misses definitions whose
  head spans lines before the name.
- Graph: `graphify explain <tag> | wc -lc`; the log-line query as quoted.
- Diagnostic keys: `grep -rhoE '"[a-z0-9]+(-[a-z0-9]+)+[: ][^"]{0,3}[a-z_]+=%' src`,
  first token, unique. An approximation of the key set.
- Splice tests: `grep -l <file> tests/test_*.py | wc -l`.
- Merge conflicts: the wave 64 to 68 merge logs of the coordinating session.

## Not done

- No task was started and nothing was built or run.
- The function and key counts are approximations by the patterns above.
- No reading-set measurement was taken for the proposals; each brief asks
  for a before and after on one question, by the
  [reading-set measures](reading-set-baseline-20261104.md#measures-task-ev-nov-9).
- `docs/status/README.md` and the ledger are not updated on this branch
  beyond one ledger paragraph; the merge sets them.
