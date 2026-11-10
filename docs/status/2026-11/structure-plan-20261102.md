# W171 · Structure plan: less context per change

**Status (Nov 9, Task EV): in progress; phases 1 and 2 landed except 2b,
four phase 3 pilots merged.** Source: devel `33cd0a839` (wave 65 merged
Nov 9). Merge state: 1a, 1b and 2a merged in wave 53 (Nov 3), 1c and 2c in
wave 54 (Nov 3); in phase 3 the `efs_fuse.c` split merged in wave 55 and
its interfaces in wave 56 (Nov 4), the `meta_apply.c` split (Task DL) in
wave 59 (Nov 6), the write-back pool ownership pilot (Task DM) in wave 60
(Nov 7) with its `ready` race fixed by Task DT in wave 61 (Nov 7), the
open-reference table (Task EB) in wave 64 (Nov 8), the `.find` result
cache (Task EC, K101 fixed), the navigation leftovers (Task ED) and the
ReadIndex entry map (Task DO) in wave 65 (Nov 9). Verdict: the two splits
did not reduce the lines an agent must read; the three ownership pilots
reduced understanding lines needed (write-back 691 to 593, open table 640
to 543, `.find` 542 to 428) while validation lines needed rose (306 to
361, 178 to 427, 120 to 395), so total needed went 997 to 954, 818 to 970
and 662 to 823, and lines opened 1,380 to 1,172, 1,856 to 1,586 and 972 to
840. The numbers establish that the implementer needed less to understand
a single-owner question once the state had one owner; they do not
establish less total reading (the compiled interface tests add
validation), nor that a cross-owner question stops at one owner, which the
[blind reader](blind-reader-20261109.md) showed it does not. Remaining
failures: none open; 2b, the `raft_host.c` split, waits on the
`devel-w123-recovery` merge, which is the user's decision. Wave 66 (Nov 9)
merged Tasks EF (graph and selector share one resolver), EH (this block),
EI (two changes by blind readers) and EJ (K54 fence test); wave 67 (Nov 9)
merged the degraded-count begin fix (Task EK) and eight small tasks EL to
ES (fuse prefix build, sanitizer lists, budget refit, K101 audit, stop files,
script citations, open splice tests, gate summary); wave 68 (Nov 10)
merged the apply visibility contract (Task ET), the contract-to-case maps
in the owner guides (Task EU) and the measurement method with necessary
expansion and validation reported apart (Task EV). Active run:
none. Next action: start 2b after W123 recovery merges. Rule: at each
wave merge the coordinator sets the status line of every merged task page
(merged in wave N on date), with the ledger entry and the W171 row.

Reviews an external structure proposal against devel and turns it into
gated tasks. Dated status pointers and the Nov 2 state are under
[History](#history).

Goal, as [developing.md](../../how-it-works/developing.md) already states it:
a change to one module is decidable from that module's source, its interface
and its tests. The measure is how few files an agent must read for one
behaviour change, not lines removed.

## Measured on devel

| Area | Measured | Note |
| --- | --- | --- |
| `src/server/raft_host.c` | 26,112 lines, 604 static functions, `struct efs_raft_host` at line 391, 63 public `server_raft_host_*` declarations in `src/server/server_internal.h` | prefix map below |
| `src/meta/meta_apply.c` / `src/client/write.c` / `src/client/efs_fuse.c` | 11,063 / 9,689 / ~8,000 lines | `meta_apply.c` is in `meta/`, not `server/` |
| Tests that splice C source by string search and compile it | 318 of 894 tracked test files; 133 read `raft_host.c` (91 with the identical `root/'src/server/raft_host.c').read_text()` idiom, 15 through `EFS_RAFT_HOST_SOURCE`, the rest ad hoc), 57 read `efs_fuse.c`, 35 `write.c` | the dominant cost of any split |
| Test catalogue | `tests/budgets.tsv` has 102 `make test-long` rows (command, budget, slots; the runner reads only the first three columns); the fast `make test` list is inline in the Makefile; `tests/test_make_test_recipe.py` enforces completeness | half a catalogue exists |
| Status index | `docs/status/README.md` 2,371 lines: 230 narrative lines above "Start here", then ~2,100 lines of dated entries; `work-queue.md`, `in-flight.md`, `decisions.md`, `catalog.md` all current (Nov 1) and its own "Maintaining this index" rule says keep it short | the current-state docs exist; the rule is not enforced |
| Settings | single source is a dated evidence page, `docs/status/2026-10/a6-settings-inventory-20261024.md`, checked against `src/common/settings.c` (23 `R()` rows plus header counts) by `tests/test_settings_inventory.py` and against the readers by `tests/test_env_inventory.py` | move, then generate |
| Cross-directory private includes | 21 `#include "../<dir>/*.h"` under `src/` | a check starts as an allowlist |
| `scripts/` | 277 top-level files, most named after the task that made them | triage, phase 3 |
| Branches touching `raft_host.c` | `devel-w123-recovery` (+662 lines, last commit Nov 2) | the split window is closed until it merges |

`raft_host.c` function-prefix map (definitions with at least six functions):
`host_append_` 27, `host_gc_` 23, `host_read_` 22, `host_fwd_` 18, `host_pub_` 17,
`host_expiry_` 16, `host_txn_` 14, `host_session_` 12, `host_report_` 12,
`host_snap_` 9, `host_query_` 9, `host_prep_` 9, `host_namespace_` 9,
`host_wait_` 8, `host_member_` 8, `host_kv_` 8, `host_reap_` 7, `host_dir_` 7,
`host_unpack_` 6, `host_command_` 6. Banner comments already mark GC reaper,
stranded-transaction recovery, W154 rebuild, D31 collection and drain, K33
addition, K36 recovery, W77 publication and W155 receipts.

## Where this plan departs from the proposal

1. **Splice harnesses come before the split, not after.** The proposal's
   step 3 (tests link real modules) is a programme, not a phase: 318 tests.
   It becomes the rule for new tests and for any test a split actually
   breaks. The bridge is one shared source resolver (phase 2a) and a
   mechanical conversion of the 133 `raft_host.c` readers; first-match
   `s.index` semantics make the concatenation order part of the contract.
2. **No repo-wide `tests/` tree move.** Subsystem and purpose columns in the
   existing catalogue give the same selection without moving 894 files and
   re-pathing budgets, recipes and evidence pages. Revisit once the catalogue
   exists.
3. **The status layer is enforcement, not design.** The living docs the
   proposal asks for exist; the README ignores its own rule.
4. **The dependency-cycle check is dropped.** The code graph has 1,751
   unresolved call edges; it stays a navigation aid. The include check is the
   compiler-visible boundary and starts as an allowlist of the 21 existing
   cross-directory includes.
5. **The `raft_host.c` split waits for W123 recovery.** `devel-w123-recovery`
   carries 662 lines in that file; splitting under it would conflict every
   W123 commit.

## Phases

Each item is one Sonnet task in its own worktree from devel, merged through a
gated wave (intel2 `make test-all` 0 FAIL). Items marked ∥ touch disjoint
files and can run in parallel. New checks go in `make test` or
`make docs-check`, never `test-long` (the gate stays at its budget bound).

### Phase 1 — navigation, no product risk

**1a ∥ Status landing page.** `docs/status/README.md` becomes: the layout
note, "Start here", and a **Current** table of at most ten rows, one per
active effort, each linking one current-state page that states exact source,
verdict, remaining failures, active run and next action. The 230 narrative
lines and ~2,100 dated entries move verbatim to `docs/status/2026-10/ledger.md`
and `docs/status/2026-11/ledger.md` (undated names, so `status_layout.py`
leaves them alone); wave entries are appended to the month ledger from now on
(the merge script's anchor changes in the same commit). W123 gets one
current-state page naming the authoritative controller; the Stage 1 matrix
and the K register already are current-state pages. Done: README under 120
lines, `check_doc_links.py` 0, every Current row's page has the five fields.

**1b ∥ Test catalogue.** `tests/budgets.tsv` gains columns `target`
(`test`/`test-long`), `subsystem` and `purpose` after the three the runner
reads, and rows for every `make test` command. `tests/test_make_test_recipe.py`
(already in `make test`) also fails when a recipe command has no row or a row
lacks subsystem or purpose. `scripts/tests_for.py <subsystem>` prints or runs
that subsystem's commands. Done: 100 percent of recipe commands classified;
`tests_for.py raft_host` lists the ReadIndex tests without reading the
Makefile. Generating the Makefile recipes from the catalogue is a later step,
after the check-only stage has held for a few waves.

**1c ∥ Component entry points.** `src/<dir>/README.md` for `raft`, `kv`,
`meta`, `wire`, `data`, `server`, `client`, `sim`, `common`, `mgmt`, `bench`:
at most 80 lines each, five sections (files and what each owns; contract
header and its invariants, lock ownership, lifetime; what it may call and who
may call it; tests, as the `tests_for.py` command rather than a list that
drifts; open K rows and W items by subsystem). Links point at existing
contracts, never restate them. `check_doc_links.py` scans `src/**/README.md`
(one line in `md_files()`); `docs/check-architecture.py` gains "every `src/`
directory has a README". A short `AGENTS.md` in a directory only where
routing differs from the root one.

### Phase 2 — one gated commit each

**2a ∥ Source resolver.** `tests/csrc.py` with `module(name)` returning the
concatenated text of a module's files in one fixed order (`raft_host` is the
single file today, the directory after 2b); the per-file override
environment variables map onto it. A script converts the 91 identical
idioms; the 15 override and ~8 ad hoc readers are converted by hand; the same
resolver serves `efs_fuse.c` (57) and `write.c` (35) readers. No assertion
changes. Done: 133 files changed mechanically, gate 0 FAIL. Can land before
W123 merges; it does not touch `raft_host.c`.

**2b Pilot split of `raft_host.c`, ReadIndex first.** After
`devel-w123-recovery` merges and while no branch touches the file:
`src/server/raft_host/host_internal.h` takes `struct efs_raft_host` and the
shared statics; `read_index.c` takes `host_read_*` and `host_wait_*` (30
functions); then one file per prefix per commit (`append.c`, `gc.c`, `fwd.c`,
`pub.c`, `expiry.c`, `txn.c`, `session.c`, `report.c`, `snapshot.c`, …). Each
commit moves code, changes `static` to internal linkage and nothing else: no
synchronization change, no reordering inside a function. The gate is the
proof; the F2 TSan lane on the result is recommended because visibility
changes alter inlining. Done for the pilot: an agent changing ReadIndex
reads `src/server/README.md`, `host_internal.h`, `read_index.c` and the
`tests_for.py raft_host.read_index` output, under 3,000 lines against 26,112
today, with zero test edits beyond 2a.
**Status (Nov 9, Task DO):** the ReadIndex entry map and the
before-measurement exist ([evidence](readindex-entry-map-20261108.md)): 28
`host_read_*`/`host_wait_*` definitions in `raft_host.c` plus 4 in
`read_index_pair_remote.h`; of the 28, 17 are the round, the pair path and the
apply wait, the other 11 share the prefix only, so the cut follows the
map's Concern column, not the prefix. One unfamiliar investigation (a read
after a leader change) needed 1,200 lines before the split. The split still
waits on the `devel-w123-recovery` merge (the user's decision).

**2c ∥ Settings single source.** The inventory table moves from the dated A6
page to `docs/how-it-works/settings.md`; the 23 `R()` rows and the header
counts in `src/common/settings.c` are generated from it with a regen-diff
check in `make test` (same pattern as the architecture gate);
`env_inventory.py --check` stays as the independent check that the readers
match the table. Done: adding a setting edits one file.

### Phase 3 — after phase 2 holds

- `efs_fuse.c` split landed in wave 55 (Nov 4, [evidence](efs-fuse-split-20261104.md)):
  12 files under `src/client/efs_fuse/`. **Finish this pilot before any further
  large split** (review of Nov 4): (a) a component guide,
  `src/client/efs_fuse/README.md`, mapping symptom to file, contract and
  tests, and the README gate recursing into split directories (done Nov 4);
  (b) the catalogue routes the 50 FUSE tests per file,
  `client/efs_fuse.<file>` (done Nov 4, six rows that span files stay
  bare); (c) a narrow interface per file: owner, lock, lifetime and failure
  behaviour of the eleven globals and the shared structs in
  `fuse_internal.h` (465 lines), globals made private where the call graph
  allows (done Nov 4, [evidence](efs-fuse-interfaces-20261104.md): eleven
  `<area>.h` headers with the contract at the top, `fuse_internal.h` 80
  lines, `g_mountpoint` private, instruction stream identical in all twelve objects); (d) a measured reading set: for three representative fixes
  (release cleanup, a ReadIndex refusal, write-back error handling) record
  the files and lines an agent must read and validate before and after,
  baseline taken on current devel before (c) lands
  ([baseline, Nov 4](reading-set-baseline-20261104.md)). Target per fix: one
  guide, one interface, one implementation area, the routed tests. Measured
  after (c) and routing ([after, Nov 5](reading-set-after-20261104.md)): the
  pilot cut dead ends and validation lines but not the lines an agent must
  read, and meets two of the four target parts.
- `tests/csrc.py` concatenation is a transition mechanism: it keeps the 318
  source-splicing tests working across splits but ties them to text order.
  Tests migrate towards compiling the real module through its narrow
  interface as (c) produces one; no deadline.
- `meta_apply.c`, `write.c` and the `raft_host.c` pilot (2b) by the 2a/2b
  recipe, each between waves with no branch on the file, and only once (d)
  shows the FUSE pilot reduced the reading set. **Condition not met** (Nov 5,
  [after-page](reading-set-after-20261104.md)): lines needed rose on both FUSE
  fixes; going ahead anyway is the user's decision. **Decided Nov 5: proceed**,
  one file at a time while no branch touches it: `meta_apply.c` first (Task DL,
  with a compiled-interface test and a before/after measurement of an
  unfamiliar investigation), `raft_host.c` after Task DJ's branch merges and
  the ReadIndex entry map exists (Task DO, done Nov 9), `write.c` after Task DK's branch
  merges. A Nov 5 review of `104eb5e10` reads the after-page as "splitting
  and headers alone did not reduce understanding" and recommends ownership
  boundaries instead: private state behind a small owner interface, tests
  that compile the real module through it, multi-tag test coverage, the
  component guides in the graph scope, and guides free of drifting counts.
  Proposed as Task DM (write-back drain and shutdown ownership pilot) and
  Task DN (navigation accuracy); the splits above carry the review's
  measurement rule.
  **`meta_apply.c` split landed Nov 5** (Task DL,
  [evidence](meta-apply-split-20261105.md)): 20 files and 13 area headers
  under `src/meta/meta_apply/` with a component guide, the instruction
  stream unchanged with inlining off, four cases added to
  `tests/test_meta_apply.c` through the public headers. Measured on one
  unfamiliar cross-area investigation (degraded-mark count across a STALE
  publish and a retire): lines needed 450 before, 461 after (not reduced),
  lines opened 675 to 852, whole-file upper bound 12,516 to 1,704; the
  investigation still reads two headers and two files, so the one-interface
  and one-implementation parts of the target are not met.
- Ownership pilots (the Nov 5 review's recommendation): private state behind
  a small owner interface, a test that compiles the real module through it,
  and an unfamiliar investigation measured before and after. **Write-back
  pool done Nov 6** (Task DM, [evidence](writeback-ownership-20261105.md),
  merged in wave 60): `g_wb` private to `writeback.c`, five operations in
  `writeback.h` with the contract on top, callers off the pool's fields,
  `tests/test_wb_interface.c` in `make test`. The investigation (can the
  unmount stop leave pool work or the sticky error behind) stopped at one
  guide, one interface, one implementation file and the routed tests plus
  14 caller lines: lines needed 691 to 593, opened 1,074 to 767, source
  files 7 to 4, validation 306 to 361. First pilot in this plan to meet the
  four target parts (the target for a single-owner question; a
  cross-owner question is measured by the
  [reading-set measures](reading-set-baseline-20261104.md#measures-task-ev-nov-9)), except 14 lines of the caller the question names; next candidates are the other shared FUSE globals
  (`g_open_mu` and the open-ref table, `g_find_res`). Status Nov 9: both
  done ([Task EB](taskEB-review-followup-20261108.md),
  [Task EC](find-res-ownership-20261108.md)).
- Next steps measured Nov 10: [structure review](structure-review-20261110.md)
  with nine prepared [task briefs](structure-review-tasks-20261110.md) (K register
  and ledger per file, concern maps of `write.c` and `raft_host.c`, a
  diagnostics index, host and gate recipes, named interface cases, a test
  recipe; PUT-window ownership deferred).
- Graph queries: a broad `graphify query` returns hundreds of nodes; agents
  need a small ranked starting set (the component README's symptom map is
  the interim answer).
- `scripts/` triage: the top level keeps the tools agents run (`qualify.py`,
  `make_test_summary.sh`, `batch_f.sh`, the checkers, …); task-named scripts
  move under `scripts/evidence/<task>/` and the evidence pages that cite them
  are re-pointed (`check_doc_links.py` proves it). Target: under 40 top-level
  files.
- K register: one file per row with the state in a header, both tables of
  `alpha-1.0-2plus1-checklist.md` generated, `check_k_register.py` becomes the
  regen check.
- Include boundary check in `make docs-check`: report `#include "../<dir>/"`
  across `src/` directories, allowlist the 21 existing ones, fail on new ones.
- Makefile `test`/`test-long` recipes generated from the catalogue.

## Sequencing and risks

- 1a, 1b, 1c, 2a and 2c are independent and can run as one wave of five
  tasks; 1a must be the only branch in its wave that edits
  `docs/status/README.md`, and the merge script changes with it.
- 2b runs alone, between waves, after W123 recovery merges. Any agent branch
  open on `raft_host.c` at that time conflicts on every commit.
- 2a touches 133 test files in one commit; it is mechanical and the gate
  proves it, but review it as a diff of the conversion script, not of the
  files.
- 1a changes where wave evidence is filed; links into the old README anchors
  from outside the repository break (none are known).
- Nothing here changes product behaviour. Any phase whose gate shows a
  difference stops and is reported, not patched forward.

## History

Status lines as they stood before Task EH (Nov 9) replaced them with the
block at the top; the text is unchanged. The opening line, Nov 2:

**Status: PROPOSED Nov 2 (devel `7b983fbb6`); no code changed.** Reviews an
external structure proposal against devel and turns it into gated tasks.

Status pointers added Nov 8 (Task EB) and Nov 9 (Task EC) above the goal:

> **Current status (Nov 8, Task EB):** the write-back pool ownership pilot
> (Task DM, called "unmerged" in the Nov 6 bullet below) merged into devel
> in wave 60, and its open `ready` race was fixed by Task DT ([write-back
> `ready` flag and start-failure join](writeback-ready-20261107.md)) and
> merged in wave 61 ([ledger](ledger.md)). The open-reference table named
> as the next candidate followed in Task EB
> ([evidence](taskEB-review-followup-20261108.md)). The text below is the
> plan as written on Nov 2 with its dated additions, left as it was.
>
> **Nov 9 (Task EC):** the third candidate, `g_find_res`, is private to
> `find_res.c` behind `find_res.h` with a compiled test; the reading set
> fell from 542 to 428 lines needed and 7 to 3 source files, and the
> before-reading found K101 (fixed on the same branch, blocks alpha: no)
> ([evidence](find-res-ownership-20261108.md), unmerged).

The current-state paragraph, Nov 2:

**Current state (Nov 2):** source devel `7b983fbb6`, no code changed; verdict PROPOSED (nothing qualified). Remaining failures: none recorded, phases not started. Active run: none; phase 1a (status landing page) is in progress on its own branch. Next action: merge phase 1 (1a status page, 1b test catalogue, 1c component READMEs) and 2a/2c as one wave; the `raft_host.c` split waits for W123.
