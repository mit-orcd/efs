# Review of the Nov 9 architecture feedback (Task EE)

**Verdict: review and planning only, no source or test change; four briefs
(Tasks EF to EI), one item folded into two briefs, no item dropped. No K
row; blocks alpha: no.** Branch `claude/task-ee-review-nov9-b56b0d` on
devel `df7d11371` (wave 64). The outside reviewer compared `df7d11371` with
`3f9110d52` and sent five recommendations; each factual claim was checked
against the tree and against the three unmerged wave 65 branches (Task EC
`claude/frosty-bartik-e0be55`, Task DO `claude/elastic-villani-1e8f16`,
Task ED `claude/xenodochial-jepsen-f0bc41`).

| # | Recommendation | Verdict | Wave 65 | Brief |
| --- | --- | --- | --- | --- |
| 1 | Graph and test selector agree; one shared resolver | **Agree, larger than stated**: two drift shapes, not one (a real component without rows gets 0 graph edges but 12 selector commands; a parent tag gets the literal rows only, 18 edges against 135 commands) | ED made declared prefixes agree by both answering nothing; the other two shapes remain | EF |
| 2 | Entry instructions: `explain <tag>` first when the component is known | **Agree with a change**: the file the reviewer names, `CLAUDE.md`, is untracked (`.git/info/exclude`), so the tracked surfaces are `AGENTS.md` and `tools/graphify/README.md` (the README already says it); the per-call hook text is graphify's, not the repository's | no | EF (one paragraph) plus two user actions |
| 3 | Focused test-case selection in large binaries | **Agree with a change**: the pilots' validation rise is a 249- and a 379-line single-contract test, which no selection shortens; the cost is real in `tests/test_meta_apply.c` (7,343 lines, 95 cases, 12 commands routed to `meta/meta_apply.publish`); an argv case filter on the binaries, no `--case` on `tests_for.py` | no | EG |
| 4 | Measure the next change with an unfamiliar reader | **Agree**: every reading set so far was the implementing agent's (EB and EC say so under "Not done") | no | EI |
| 5 | One current-status block per active page | **Agree**: the structure plan opens with "PROPOSED ... no code changed" above two later blocks, the EB page says "Not merged" after wave 64 merged it, the W171 row says "unmerged" | EC adds a third dated paragraph to the plan's quoted block; all three branches edit the W171 row | EH |

The reviewer's arithmetic is right. On the EB page: needed 640 + 178 = 818
before, 543 + 427 = 970 after; opened 1,202 + 654 = 1,856 before, 683 + 903
= 1,586 after. The EC page (branch) has the same shape: needed 542 + 120 =
662 to 428 + 395 = 823, opened 852 + 120 = 972 to 445 + 395 = 840. In both,
"needed" rose only because the compiled interface test counts in full
(249 and 379 lines) where before there was no direct test to read.

## Evidence per item

### 1. Graph and selector

On devel `df7d11371`:

| Tag | `graphify explain` `tested_by` edges | `tests_for.py` commands | Agree |
| --- | ---: | ---: | --- |
| `meta/meta_apply.publish` | 0 (guide, contract, implementation only) | 12, with "no row of its own ... the rows of meta/meta_apply follow" | no |
| `server/raft_host` | 18 | 135 | no |
| `client/efs_fuse` | 5 | 55 | no |
| `server/raft_host.read_index` | 18 | 18 | yes |
| `client/efs_fuse.open` | 10 | 10 | yes |
| `client/efs_fuse.writeback` | 4 | 4 | yes |

Cause: `components()` in `scripts/build-code-graph.py` parses
`tests/budgets.tsv` itself and adds one edge per literal tag of a row.
`scripts/tests_for.py` `select()` takes a tag equal to the wanted one or
starting with it and a dot (the parent includes its components), and
`resolve()` falls back from a real component (`src/<dir>/<component>.{c,h}`
or directory) to its parent with a note. Task ED (branch) adds a third rule
to both: a function prefix declared on a `# prefixes` header line is a
component that lists nothing when it has no rows (exit 1); the graph gets
the node with no edges, so that case agrees by being empty on both sides.
The parent-rollup and the file-component fallback still disagree after ED.

`graphify explain` prints relation and confidence (`[tested_by] [EXTRACTED]
tests/budgets.tsv:L630`), not the edge's `evidence_method`, so an inherited
route must differ in a printed field. The existing confidence labels are
EXTRACTED, INFERRED and AMBIGUOUS; an inherited route is INFERRED from the
parent's rows, with the parent tag in `evidence_method`. No new relation,
no third representation: the builder imports `tests_for.py` (stdlib only;
`scripts/rebuild-code-graph.sh` runs the builder under graphify's own
interpreter, which has no project dependencies to satisfy) and a test
asserts equality for every component node.

### 2. Entry instructions

`CLAUDE.md` exists only in the primary checkout and is excluded by
`.git/info/exclude`; no branch can change it, and the worktrees have none.
Its rule: run `graphify query "<question>"` before the first grep, search or
source read. The same rule is in the coordinator's brief template (this
task's brief carries it verbatim). The per-call reminder ("graphify: this
project has a knowledge graph ... `graphify query`") comes from
`graphify hook-guard`, the plugin's command named in `.claude/settings.json`,
not from repository text. `AGENTS.md`'s graph section says how to rebuild and
test, nothing about how to start a question. `tools/graphify/README.md` ("How
to query") already says: for a component, start from its catalogue tag with
`graphify explain <tag>`. A broad query on this tree (`graphify query "where
is the open reference table mutex"`) seeds on `reference()`, `table()`,
`open_()` and `fragment_mutex()` from unrelated files and truncates at 69 of
94 nodes, as the reviewer saw.

Smallest change: one paragraph in `AGENTS.md` (component known: `graphify
explain <tag>`; owner unknown: `graphify query` to locate it, then explain;
after a change: rebuild and test), the README's "How to query" put in the
same order, and two actions outside the tree for the user: the local
`CLAUDE.md` rule and the brief template's "Graph rule" sentence.

### 3. Case selection

`tests/test_open_interface.c` (249 lines) and `tests/test_wb_interface.c`
(279 lines) have `int main(void)` with comment-delimited blocks, every block
about the one contract; `tests/test_find_res_interface.c` (branch, 379
lines) likewise. Selection cannot shorten them, and their reading cost is
the direct test the contract did not have before. `tests/test_meta_apply.c`
has `int main(void)` calling 95 `static void test_*` cases in sequence (7,343
lines); the k37 fix in wave 64 "pins a case" there, and a reader validating
`publish` runs and reads the whole binary. Other sequence binaries with many
cases: `test_raft.c` (3,689 lines, 50 cases), `test_sim.c` (1,857, 47),
`test_kv_lsm.c` (2,202, 31), `test_session.c` (1,471, 28), `test_txn.c`
(1,378, 23), `test_raft_store.c` (1,115, 17). Existing argv handling in
`tests/*.c` is per binary and ad hoc (`test_follower_bound.c` compares
`argv[1]` with one flag; `test_kv_lsm_concurrent.c` reads a duration). The
Python suites are one file per test and need nothing; 23 use `unittest`,
whose `-k` already selects a case.

Smallest change: `main(int argc, char **argv)` runs a case when no argument
is given or any argument is a substring of the case's name (`./tests/test_meta_apply
publish` runs `test_publish` and `test_publish_stale_then_retry`), `--list`
prints the names. The catalogue row is unchanged; a `--case` on
`tests_for.py` would need a per-row case column, a third representation.
The mapping from an operation to its cases is the case name, found with
`--list`; no new file.

### 4. Unfamiliar reader

Both pilot pages record the limit: "The after-reader was the implementing
agent, who wrote `find_res.h`" (EC), "The after-reader and the item 5
reader were the implementing agent, not blind" (EB). On this machine a
fresh session is not blind either: the session-start memory injection
(claude-mem) lists the last week's observations by name, including the
find_res cache, the open table and the k37 count defects. A subagent started
with the Agent tool gets its prompt and the tracked instructions, no
session-start injection; the brief uses that, forbids the memory tools and
`docs/status`, and prints the prompt on the evidence page.

Targets, both verified real on `df7d11371`:

- Caller-side fix: `efs_fuse_destroy` (`src/client/efs_fuse/main.c`) calls
  `efs_open_retry_stop()` with no bounded `efs_open_retry_pass` before it and
  no report of what was retained, while the explicit stop
  (`src/client/efs_fuse/release.c:79-96`) runs the pass and refuses with
  `retained-releases=%u wb-pending=%d`. `open.h` states the retained-row
  lifetime and names the operations; `writeback.h` says `efs_wb_pending`
  tells what a cut drain left. `efs_client_shutdown` (`node_cache.c:927`)
  revokes the session, which collects leases server side, so the recorder
  decides first whether the pass or only the report is owed (reference
  answer), then the blind reader is asked the question.
- Cross-component investigation: the D26 GC watermark derive
  (`src/meta/meta_apply/gc.c:98-106`, `gc_wm_pending = n + gc_wm_pos`,
  derived from the reaper's scan at `src/server/raft_host.c:11149`) counts an
  insert noted while the count was unknown once in the scan and once in
  `gc_wm_pos`; the k37 page records it as "the same derive-time double
  count ... Not changed here", and the fixed twin is
  `efs_meta_degraded_count_begin` (`gc.c:229`). Symptom for the reader: after
  a leader change the reaper's pending watermark exceeds the live GC records
  and extra bounded reap scans run until the clamp.

### 5. Status blocks

`docs/status/2026-11/structure-plan-20261102.md` opens with "**Status:
PROPOSED Nov 2 (devel `7b983fbb6`); no code changed.**", then a quoted
"Current status (Nov 8, Task EB)" block (EC's branch adds a "Nov 9 (Task
EC)" paragraph to it), then "**Current state (Nov 2):** ... verdict PROPOSED
(nothing qualified) ... phases not started". `taskEB-review-followup-20261108.md`
line 19: "Not merged, not pushed." (merged in wave 64, ledger line 7). The
W171 row of `docs/status/README.md`: "Task EB (Nov 8, unmerged)". The
Current table's own sentence already defines the block: "the linked page
states the exact source, verdict, remaining failures, active run and next
action", so no template is needed, only that block at the top of each
active page with the dated history below it. The wave merge recipe lives in
the coordinator's notes, not in the tree; the merge left "Not merged" on the
EB page because nothing says to change it, so the brief also gives the
merged pages' status line to the merge as a one-line rule on the plan page.

Sequencing: EC, DO and ED each edit the plan, the W171 row and the ledger
anchor, and ED edits `build-code-graph.py`, `tests_for.py`,
`test_code_graph.py` and `test_tests_for.py`. Every brief below branches
from devel after wave 65 lands.

## Briefs

Common to all four: branch from devel after the wave 65 merge (`git
merge-base --is-ancestor devel HEAD || git reset --hard devel`); Opus 5.5
medium; fix code, not timeouts; no backward compatibility; never push,
never merge into devel, never edit `~/git/devops` or `~/git/cluster`, do not
read `ACCOUNTS.txt`, touch no live cluster (efsd on ports 17432-17435,
`/data1/efs`, the `efs` user, ports 27432/27433, the orcd-test stores on
fcstor003-013). Graph rule: `graphify explain <component tag>` when the
component is known, `graphify query "<question>"` only to locate an unknown
owner, before the first grep or source read; put the rule in every subagent
prompt that explores code. Stage by name (never `git add -A`, `commit -a`
or `stash`); commit messages end with `Co-Authored-By: Claude Opus 5.5
<noreply@anthropic.com>` (the model running). Each brief commits one
evidence page `docs/status/2026-11/<name>-20261109.md` (purpose, source
commit, environment, commands, results, limitations), a ledger paragraph
under `## Recent evidence and remaining acceptance` in
`docs/status/2026-11/ledger.md` newest first ("no K row, blocks alpha: no"),
then runs `python3 scripts/check_doc_links.py`, `python3
scripts/status_layout.py`, `make docs-check`, rebuilds the graph (`sh
scripts/rebuild-code-graph.sh`, `python3 tests/test_code_graph.py`) and
commits it separately with `git add -f graphify-out/GRAPH_REPORT.md
graphify-out/graph.json graphify-out/source-manifest.json
graphify-out/graph.html` (the add may exit 1 on the gitignored-but-tracked
path; confirm with `git log`). Shared files: `ledger.md` (every brief adds
its paragraph at the anchor; the merge resolves the anchor conflict as in
every wave) and `graphify-out/` (`merge=ours`, regenerated at merge). No
other file is in two briefs; `docs/status/README.md` belongs to EH alone.
Host recipe where a host is named: build under `/tmp/efs-task<XX>` with
`TMPDIR=/tmp/efs-task<XX>-tmp`; libfuse 3.16.2 through a shim directory
whose `lib64` and `include` link the orcd-test deps, with
`PKG_CONFIG_PATH=<shim>/lib64/pkgconfig` and `LD_LIBRARY_PATH=<shim>/lib64`
exported (`EFS_FUSE_PREFIX` alone does not reach `pkg-config fuse3`; Task
ED's page records the failure); on node9900 the prefix is
`/data1/efs/deps/libfuse` (read only). Remove the trees and tars when done;
raw output to `~/efs-artifacts/task<XX>-20261109/` with a `SHA256SUMS`.

### Task EF: the graph answers what the test selector answers

Area: `scripts/build-code-graph.py`, `tests/test_code_graph.py`,
`tools/graphify/README.md`, `AGENTS.md`, `graphify-out/` (rebuild). No other
file. Mac only (the pinned graphify environment); no host.

Do: in `build-code-graph.py`, `components()` stops parsing
`tests/budgets.tsv` itself. It imports `scripts/tests_for.py` by path
(`importlib.util.spec_from_file_location`, as `tests/test_tests_for.py`
does) and, for every component tag (layout tags, catalogue tags, declared
prefix tags), calls `tests_for.resolve(rows, tag)`. A tag answered with its
own rows (the answered subsystem equals the tag) gets `tested_by` edges with
confidence EXTRACTED and `evidence_method` `test-catalogue`, one per test
file the commands name, including rows matched through a child tag (that is
the selector's definition of a subsystem). A tag answered by its parent
gets `tested_by` edges with confidence INFERRED and `evidence_method`
`inherited:<parent tag>`. A tag `resolve()` refuses (a declared prefix with
no rows) gets no edge. Keep the test-file extraction from the command text
as it is (the regex over `tests/` and `scripts/` names). Delete the
builder's own catalogue loop; no second parser remains.

In `tests/test_code_graph.py`, extend the component test: for every
`efs:component:` node, the set of `tested_by` targets equals the files
`tests_for.resolve()` names for that tag (or is empty where it raises);
`meta/meta_apply.publish` has INFERRED edges to the twelve `meta/meta_apply`
commands' files and EXTRACTED none; `server/raft_host` has one edge per
file of its 135 commands; `server/raft_host.publish` has none. Update the
README's component paragraph (an inherited route is INFERRED, labelled with
the parent; `tested_by` remains a catalogue route, not coverage) and put
"How to query" in the order component known: `explain <tag>`; owner
unknown: `query`, then `explain`. Add the same three-sentence rule to
`AGENTS.md`'s graph section (item 2).

Done when: `graphify explain meta/meta_apply.publish` lists twelve
`[tested_by] [INFERRED]` edges and `graphify explain server/raft_host`
lists one `tested_by` edge per file of `python3 scripts/tests_for.py
server/raft_host`; `python3 tests/test_code_graph.py` passes with the
equality assertion over every component node; `python3
tests/test_tests_for.py` still passes unchanged; `make docs-check` passes;
the evidence page shows the before and after `explain` output for the two
tags and the node and edge counts. The page also lists the two actions
outside the tree for the user: the local `CLAUDE.md` graph rule and the
brief template's "Graph rule" sentence, both to say `explain` first.

### Task EG: a case filter on the sequence test binaries

Area: `tests/test_meta_apply.c`, `tests/test_raft.c`, `tests/test_sim.c`,
`tests/test_kv_lsm.c`, `tests/test_session.c`, `tests/test_txn.c`,
`tests/test_raft_store.c`, `docs/how-it-works/testing.md`. No other file
(the catalogue rows and the Makefile are unchanged; the interface tests
`test_open_interface.c`, `test_wb_interface.c`, `test_find_res_interface.c`
are single-contract and stay as they are). Host: fstor007.

Do: in each binary, `main(int argc, char **argv)`; a case runs when argc is
1 or when some argument is a substring of the case's name; `--list` prints
every case name, one per line, and exits 0; an argument that matches no
case is an error (exit 2, the names on stderr), so a misspelling is not a
silent pass. Keep the case order and the per-case code untouched: the
change is `main` only, one `RUN(test_publish)`-style line per case that
checks the name against argv (a macro local to each file, following the
file's own style; no shared header, no new file). Where a binary already
reads argv (`test_follower_bound.c` is not in this list; check each of the
seven before editing), keep its existing arguments working.

Done when: `./tests/test_meta_apply publish` runs `test_publish` and
`test_publish_stale_then_retry` only (the page shows the output and the
wall time next to a bare run), `./tests/test_meta_apply --list` prints 95
names, `./tests/test_meta_apply publsh` exits 2; the same three checks on
each of the other six binaries; a bare run of every changed binary passes
unchanged; fstor007 `make test` rc 0 and `python3 scripts/tests_for.py
meta/meta_apply --run` rc 0 after `make all` and the `TEST_SPECIAL_BINS`
of `make test-long` (Task EB's note: `--run` fails on the first test-long
row without them); `test_meta_apply` is on no `tests/sanitizer_check.sh`
list, so no sanitizer run is owed; `testing.md` gets one paragraph
under the catalogue text: how to list and run the cases of one binary, and
that the catalogue row is the whole binary. Record on the page the line
count of `test_publish` plus `test_publish_stale_then_retry` against the
file's 7,343 lines: that is the validation reading a `publish` reader now
needs.

### Task EH: one current-status block per active page

Area: `docs/status/2026-11/structure-plan-20261102.md`,
`docs/status/2026-11/taskEB-review-followup-20261108.md`,
`docs/status/2026-11/writeback-ownership-20261105.md`,
`docs/status/2026-11/find-res-ownership-20261108.md`,
`docs/status/2026-11/readindex-entry-map-20261108.md`,
`docs/status/2026-11/navigation-leftovers-20261108.md`,
`docs/status/README.md` (the W171 row). Docs only; no source, no host; Mac.

Do: each page named above opens with one block, the one the Current table
already defines ("the exact source, verdict, remaining failures, active run
and next action"), dated today and stating the merge state (merged in wave
N on date, or the branch name and "unmerged"). On the structure plan,
replace the opening "Status: PROPOSED ... no code changed" line with that
block (phases 1 to 3 landed, which pilots merged in which waves, what is
in progress, next action), and move the quoted Nov 8 and Nov 9 pointer
block and the "Current state (Nov 2)" paragraph into a dated "History"
section below the plan's own text; keep the Nov 2 text itself unchanged.
On each task page, replace the "Not merged, not pushed" sentence of the
verdict paragraph with the merge state and wave; nothing else on those
pages changes. Add one rule line to the plan page's status block: at each
wave merge the coordinator sets the merged pages' status line (the merge
notes already touch the ledger and the W171 row). Update the W171 row to
say "merged" where it says "unmerged" and to name the wave 65 tasks in one
clause each.

Done when: each of the seven files opens with exactly one block that says
source, verdict, remaining failures, active run, next action and merge
state, and no "Not merged"/"unmerged"/"PROPOSED" wording remains above it
(`grep -n "Not merged\|unmerged\|PROPOSED" <the seven files>` shows only
lines inside the History sections); `python3 scripts/check_doc_links.py`,
`python3 scripts/status_layout.py --dry-run`, `make docs-check` (including
Task ED's `check_guide_counts.py`) pass; the evidence page lists, per
file, the lines moved and the lines replaced.

### Task EI: the next change measured with a blind reader

Area: measurement of two changes; source files the blind reader touches are
`src/client/efs_fuse/main.c` (the caller-side fix) and
`src/meta/meta_apply/gc.c` with its case in `tests/test_meta_apply.c` (the
investigation); the recorder writes the evidence page only. No other file.
Host: node9900 (build, `make test`, `tests_for.py --run`, TSan entries).
Starts after Task EG merges (it adds a case to `test_meta_apply.c`'s case
list, which EG rewrites as `RUN` lines).

Blinding: the recorder (this session) never writes code. For each
question it starts one fresh general-purpose subagent with the Agent tool
(a cold context: it gets the prompt and the tracked instructions, no
session-start memory injection). The prompt names the worktree path, the
graph rule above, the question (symptom and what to deliver), and the
recording rule: list every file and line range opened, in order, each
marked needed or not needed, and every test looked for and not found;
forbids `docs/status/`, the claude-mem tools (`mem-search`, `search`,
`get_observations`) and any file under `docs/` other than
`docs/how-it-works/`; asks for the change as a diff plus the command that
validates it. The recorder prints each prompt verbatim on the page. Before
starting a subagent, the recorder writes the reference answer on the page
from its own reading (not shown to the reader).

Question 1, the caller-side fix: "At unmount (`efs_fuse_destroy`) a last
close whose CLOSE edge failed is still retained in the open table, and the
write-back pool may still hold jobs. The explicit stop path refuses with a
line naming both counts; unmount says nothing and stops the retry thread.
Decide from the contracts what unmount owes (a bounded retry pass, a
report, or neither, and why), implement it, and name the test that proves
it." Reference answer by the recorder first: whether
`efs_client_session_end` (`src/client/node_cache.c`, `efs_client_shutdown`)
makes the pass redundant, so that the report alone is the fix; whichever
it is, the expected diff touches `main.c` only and is decidable from
`open.h`, `writeback.h` and `release.c`'s stop path. Correctness: the
reader's decision matches the reference, the diff is in `main.c` only, and
the named test passes on node9900. A case added to
`tests/test_open_interface.c` would be the wrong place (that is the owner's
test of `open.h`, not of the unmount path); the recorder notes which test
the reader chose and whether it had to be pointed elsewhere. If the
reference answer is that neither the pass nor the report is owed, question
1 becomes the report line alone (unmount names `retained-releases` and
`wb-pending` as the stop path does), the page says this is a diagnostic
like EB's item 5, and question 2 is the measured real fix.

Question 2, the cross-component investigation: "After a leader change the
GC reaper's pending watermark for an anchor exceeds the number of live GC
records and the reaper runs extra bounded scans until the count clamps.
Find the cause across the reaper and the counter, fix it, and pin it with a
case in `tests/test_meta_apply.c`." Reference: `efs_meta_gc_pending_derived`
adds `gc_wm_pos` (inserts noted while unknown) to the scan count `n`
although the scan already counted those applied before it; the reaper
derives at `raft_host.c` `host_gc_` (the one call of
`efs_meta_gc_pending_derived`); the fixed twin is
`efs_meta_degraded_count_begin`, called by the reaper before its scan. The
expected fix mirrors it: a `begin` that drops `gc_wm_pos` while unknown
before the scan, called from the reaper, and a case that shows the count
without the call and with it. Correctness: the reader names both files and
the derive line, the fix is upward-only safe (no record missed), the case
fails without the call and passes with it, `./tests/test_meta_apply
gc_pending` (EG's filter) and `make test` pass on node9900
(`test_meta_apply` is on no sanitizer list; none is owed).

Record per question: lines opened and needed by category (guide, interface,
implementation, callers, tests, navigation) as the EB page counts them;
source files opened; wrong turns (files opened and not needed, and why
the reader went there); tests looked for and not found; correctness
against the reference; wall time; whether `graphify explain` was the first
call and what it returned. Compare with the EB and EC pages' numbers in one
table and say what the comparison does and does not establish (one reader,
two questions, the graph and selector as they are on that devel).

Done when: both subagent prompts, both reference answers, both reading
logs and both diffs are on the evidence page; both changes build and pass
on node9900 as above; the two changes are committed as two commits with
the reader's diff unchanged except for what the recorder had to fix (named
on the page); the ledger paragraph states the verdict in one line; raw
output under `~erbmi1/efs-artifacts/taskEI-20261109/` with `SHA256SUMS`.

## Not done

- No source or test change on this branch (review and planning only).
- Not run: `make test`, any gate, any host. The `graphify` and `tests_for.py`
  comparisons ran on the Mac against the committed graph of `df7d11371`.
- Item 2's two surfaces outside the tree (the local `CLAUDE.md`, the brief
  template) are the user's; Task EF only lists them.
- The merge recipe is not in the tree; EH adds the status-line rule to the
  plan page, where the coordinator reads it, not to a checked recipe.
- Whether a destroy-time retry pass is owed (EI question 1) is left to the
  recorder's reference answer; this page records only that destroy runs no
  pass and reports nothing while the stop path does both.
