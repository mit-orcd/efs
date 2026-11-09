# The graph answers what the test selector answers (Task EF)

Task EF, Nov 9 2026, Mac only, devel `33cd0a839` (wave 65). Purpose: item 1
of the [Nov 9 review](review-feedback-nov9-20261109.md) measured that
`graphify explain <tag>` and `scripts/tests_for.py <tag>` disagree for a
parent tag (the graph had the literal rows only) and for a real component
with no row of its own (the graph had no `tested_by` edge, the selector
answers the parent's rows). This task makes the graph builder use the
selector's own resolver, so the two cannot disagree.

**Verdict: PASS, graph builder and docs only, no K row, blocks alpha: no.**
`graphify explain meta/meta_apply.publish` lists 13 `[tested_by] [INFERRED]`
edges, one per file of `tests_for.py meta/meta_apply`; `graphify explain
server/raft_host` lists 143 `tested_by` edges, one per file of its 143
commands; `tests/test_code_graph.py` asserts the equality for all 180
component nodes. The brief's figures (12 and 135 commands) were measured at
`df7d11371`; wave 65 added catalogue rows.

## The change

Commit `9de9d051b` on branch `claude/task-ef-graph-selector-715eab`:

- `scripts/build-code-graph.py`: `components()` no longer reads
  `tests/budgets.tsv` rows itself. It loads `scripts/tests_for.py` by path
  (`tests_for_module()`), takes the tags from `catalogue()` and
  `prefix_tags()`, adds the layout tags as before, and calls
  `resolve(rows, tag)` for every tag. Answered with its own rows (children
  included, the selector's definition of a subsystem): `tested_by` edges
  EXTRACTED, `evidence_method` `test-catalogue`. Answered by the parent:
  INFERRED, `evidence_method` `inherited:<parent tag>`. `LookupError` (a
  declared prefix with no rows): no edge. One edge per test file (before:
  one per file and catalogue line); the file extraction from the command
  text is the same regex, moved into `test_files()`. The edge location is
  the catalogue line of the first command naming the file. The builder's
  own catalogue loop is gone; the only remaining read of the catalogue text
  maps a command to its line number.
- `tests/test_code_graph.py`: for every `efs:component:` node, the
  `tested_by` targets equal the files `resolve()` names (empty where it
  raises), one edge per file, all labelled as above;
  `meta/meta_apply.publish` has only INFERRED edges, to the files of
  `meta/meta_apply`; `server/raft_host` has one edge per file of
  `select(rows, 'server/raft_host')`; `server/raft_host.publish` has none.
  Checked against the old graph: the test fails (`Items in the second set
  but not the first`).
- `tools/graphify/README.md`: "How to query" leads with the rule (component
  known: `explain <tag>`; owner unknown: `query`, then `explain`), and the
  component paragraph says an inherited route is INFERRED and labelled with
  the parent, and `tested_by` is a catalogue route, not coverage.
- `AGENTS.md`, graph section: the same rule in three sentences.

## Results

Node and edge counts (`graphify-out/graph.json`):

| | before (`33cd0a839`) | after |
| --- | ---: | ---: |
| nodes | 18584 | 18587 |
| edges | 76842 | 77215 |
| component nodes | 180 | 180 |
| `tested_by` EXTRACTED | 735 | 850 |
| `tested_by` INFERRED | 0 | 250 |

The three new nodes are the builder's own two new functions and one
docstring rationale node (one old rationale node replaced).

`tested_by` targets per tag (`graphify explain <tag> --all`):

| Tag | before | after | `tests_for.py` commands |
| --- | ---: | ---: | ---: |
| `meta/meta_apply.publish` | 0 | 13 INFERRED | 13 (parent's rows) |
| `server/raft_host` | 59 | 143 EXTRACTED | 143 |
| `client/efs_fuse` | 5 | 55 EXTRACTED | 56 (two commands share a file) |
| `server/raft_host.read_index` | 18 | 18 | 18 |
| `client/efs_fuse.open` | 10 | 10 | 10 |

Before, `graphify explain meta/meta_apply.publish`:

```text
Connections (3):
  --> src/meta/meta_apply/README.md [guided_by] [EXTRACTED] src/meta/meta_apply/README.md:L1
  --> src/meta/meta_apply/publish.c [implemented_by] [EXTRACTED] src/meta/meta_apply/publish.c:L1
  --> src/meta/meta_apply/publish.h [contract] [EXTRACTED] src/meta/meta_apply/publish.h:L1
```

After:

```text
Connections (16):
  --> tests/test_meta_apply.c [tested_by] [INFERRED] tests/budgets.tsv:L124
  --> tests/test_append_transaction.c [tested_by] [INFERRED] tests/budgets.tsv:L98
  --> tests/test_session_owned_capture.c [tested_by] [INFERRED] tests/budgets.tsv:L536
  --> tests/test_append_interleave.c [tested_by] [INFERRED] tests/budgets.tsv:L543
  --> tests/test_append_cleanup.c [tested_by] [INFERRED] tests/budgets.tsv:L542
  --> tests/test_pub_stage.c [tested_by] [INFERRED] tests/budgets.tsv:L102
  --> tests/test_mtime_barrier.c [tested_by] [INFERRED] tests/budgets.tsv:L535
  --> src/meta/meta_apply/README.md [guided_by] [EXTRACTED] src/meta/meta_apply/README.md:L1
  --> tests/test_append_receipt.c [tested_by] [INFERRED] tests/budgets.tsv:L541
  --> tests/test_lane_bootstrap_recovery.c [tested_by] [INFERRED] tests/budgets.tsv:L551
  --> tests/test_namespace_session.c [tested_by] [INFERRED] tests/budgets.tsv:L613
  --> src/meta/meta_apply/publish.c [implemented_by] [EXTRACTED] src/meta/meta_apply/publish.c:L1
  --> tests/test_readdir_migration_race.py [tested_by] [INFERRED] tests/budgets.tsv:L201
  --> tests/test_retained_create_authority.py [tested_by] [INFERRED] tests/budgets.tsv:L285
  --> tests/test_meta_cmd_namespace.py [tested_by] [INFERRED] tests/budgets.tsv:L545
  --> src/meta/meta_apply/publish.h [contract] [EXTRACTED] src/meta/meta_apply/publish.h:L1
```

`graphify explain server/raft_host`, before `Connections (61)` with 59
`tested_by` (the literal `server/raft_host` rows only), after
`Connections (145)`: `implemented_by`, `guided_by` and 143 `[tested_by]
[EXTRACTED]` (rows tagged `server/raft_host` or `server/raft_host.<child>`).

Checks: `python3 tests/test_code_graph.py` 10 tests OK; `python3
tests/test_tests_for.py` PASS (unchanged file); `python3
scripts/check_doc_links.py`, `python3 scripts/status_layout.py` and `make
docs-check` pass.

## Environment and reproduction

macOS (Darwin 25.6), graphify 0.9.97 (`graphifyy`), its own interpreter via
`scripts/rebuild-code-graph.sh`; system Python 3.9.6 for the tests. No host,
no cluster, no build.

```sh
git show 33cd0a839:graphify-out/graph.json > /tmp/before-graph.json
graphify explain meta/meta_apply.publish --all --graph /tmp/before-graph.json
sh scripts/rebuild-code-graph.sh
graphify explain meta/meta_apply.publish --all
graphify explain server/raft_host --all | grep -c '\[tested_by\]'
python3 scripts/tests_for.py server/raft_host | wc -l
python3 tests/test_code_graph.py
python3 tests/test_tests_for.py
```

Remove `/tmp/before-graph.json` afterwards.

## Limitations

- `tested_by` stays a catalogue route, not coverage: a row says the command
  exercises the tag, not which lines it reaches.
- The edge count is per file, the selector's per command; they differ where
  two commands name one file (`client/efs_fuse`: 56 commands, 55 files) or a
  command names no tracked test file.
- `graphify explain` prints confidence, not `evidence_method`; the parent tag
  is in the JSON edge (`inherited:meta/meta_apply`), not the printed line.

## Actions outside the tree (for the user)

- The local `~/.claude/CLAUDE.md` graph rule: say `graphify explain <tag>`
  first when the component is known, `graphify query` only to locate an
  unknown owner, then `explain`.
- The coordinator's brief template, its "Graph rule" sentence: the same
  order, `explain` first.
