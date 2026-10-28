# Defect-navigation graph scope and validation — October 28, 2026

**PASS for graph navigation and integrity checks; not product acceptance.**
Related findings: [W127 apply refusal](../backlog/work-items.md#w127),
[W145 snapshot tail](../backlog/work-items.md#w145), and the
[W125 packing contract](../backlog/work-items.md#w125).

## Source and scope

Built from `7045ca1f9a3804608b343206256c9d4b4df13cff` (graph configuration
on devel base `87ac399a3`). The exact per-input hashes and installed extractor
module hash are in `graphify-out/source-manifest.json`. Local graphifyy 0.9.97,
arm64 macOS, local four-worker extraction, no LLM/API/network call, no cluster or
runtime binaries changed. TCP/RDMA, I/O mode, quota and fixture size are not
applicable to this graph build. This work does not change any release verdict.

The positive selection is `tools/graphify/scope.json`: all tracked first-party
C/headers/Python/shell in `src`, `include`, `tests`, `scripts`; architecture and
user docs; selected current ledgers/contracts; the Makefile. Live harnesses and
fault models stay. Dated results, archived discussion, vendored implementation
and generated architecture duplicates are excluded. Document inclusion does not
turn a proposal into an accepted contract or make an old ledger status current.

The rebuilt graph has **16,232 nodes and 67,773 directed typed edges**, from
1,192 code files, 31 documents and one build definition. Parallel relationship
types and call-site evidence are retained. Clustering uses an undirected
projection only; the stored graph is a directed multigraph. The HTML presents
523 communities rather than attempting a full 16,232-node layout.

The previous on-disk graph was preserved before replacement under
`/Users/mike/Documents/Codex/artifacts/efs-graph-20261028/before-graph.json`,
with its HTML, report and full hashes in `before-sha256.json`: 22,774 nodes /
64,730 edges. This was an evolving local snapshot, not a controlled same-source
comparison. The new graph is about 28 MiB: fewer nodes does not mean fewer
bytes when parallel edges and provenance are retained. Size is not the gate.

## Checks and results

- **W127 navigation PASS:** the exact contract heading reaches `host_apply`
  and `tests/test_apply_unsupported.py`; the AST must independently contain
  `host_apply -> efs_kv_lsm_apply_end`. The Python harness extracts C, so its
  `exercises` link is explicitly curated, not a Python-to-C runtime call.
- **W145 navigation PASS:** the historical unproven-tail contract reaches
  `on_snap_req -> snap_installed` and the actual
  `test_snapshot_commit_stops_at_snapshot` oracle. That test asserts the
  snapshot cannot commit the follower's unproven tail.
- **W125 contract navigation PASS:** `tests/test_pack_switch.c:main` has an
  actual AST call to `efs_meta_apply_pack_switch`, with stale-source/live-count
  assertions retained. This is an existing packing correctness contract,
  not an independent explanation or reproduction of the W148 mismatch.
- **Nine graph/tool checks PASS:** real paths; missing production call;
  reversed call; a curated call substituted for AST evidence; missing oracle;
  changed distinguishing oracle text; dangling endpoint; scope selection;
  and actual hook installation/idempotence/original-hook preservation in a
  disposable Git repository, including a branch without this scope and the
  existing skip mechanism.
- **Native existing tests PASS:** the apply-return policy regression and the
  complete standalone Raft state-machine test, including the W145 oracle.
  These are scoped local checks. The full LSM apply-refusal harness, packing
  runtime suite, live cluster gates and full product suite were not rerun.
- **Structural integrity:** zero missing/dangling endpoints and no exact
  duplicate edges. A simple directed graph would collapse 2,388 additional
  call-site/relationship edges; the multigraph retains them.
- **Reproducibility:** a second fresh extraction with the same input hashes
  is compared byte-for-byte with the graph and manifest; no AST cache or
  semantic extraction cache is reused.

`graphify query host_apply --seed host_apply --context calls --budget 1500`
and `graphify explain W145-snapshot-unproven-tail` work against the new file.
Explicit seeds and relationship filters remain useful: narrowing the corpus
cannot eliminate ambiguous natural-language queries.

## Remaining graph limits

The extractor reports a partial parse in `include/efs/protocol.h` at line 322
(833 symbols recovered), and 49 code files with no extracted symbols. These
warnings and per-file symbol counts are retained in the manifest/report.
There are **1,353 unresolved call edges omitted**, rather than invented as
resolved dependencies. Unresolved imports have labelled boundary nodes and may
be external or unresolved project-local imports. Exact quoted C include paths
resolve to selected headers.

The health diagnostic also reports **33 self-call edges**. They are retained;
recursion and ambiguous callback resolution require source inspection, so this
is not a clean precision/recall certification. No absent edge proves absence of
a dependency. The graph does not infer locks, state writes, persistence ordering
or happens-before. Literal document mentions, Makefile references and curated
oracle links are distinct from executable calls and from test coverage.

## Reproduction and upkeep

From a checkout of the source above with graphifyy 0.9.97 installed:

```sh
sh scripts/rebuild-code-graph.sh
python3 tests/test_code_graph.py
# Use the interpreter owning graphify if it differs from python3:
python3 scripts/build-code-graph.py --validate-only
sh scripts/rebuild-code-graph.sh --out /tmp/efs-graph-reproduce
cmp graphify-out/graph.json /tmp/efs-graph-reproduce/graph.json
cmp graphify-out/source-manifest.json /tmp/efs-graph-reproduce/source-manifest.json
python3 tests/test_txn_apply_retry.py
cc -std=gnu11 -O1 -Iinclude tests/test_raft.c src/raft/raft.c \
  src/raft/raft_mem.c src/common/batch_obs.c -pthread -o /tmp/efs-graph-test-raft
/tmp/efs-graph-test-raft
make docs-check
```

The initial standalone Raft link omitted `src/common/batch_obs.c` and failed
with unresolved observation symbols. Adding the required production source
produced the passing command above; no product or test source was changed to
make it pass. Build/check logs are outside Git under
`/Users/mike/Documents/Codex/artifacts/efs-graph-20261028/`.
Temporary extraction directories are automatically removed. The generated
snapshot is intentionally committed; caches and per-run logs are not.

`python3 scripts/install-code-graph-hooks.py` installs local wrappers around
existing graphify hooks, preserving originals. On the primary configured
checkout they run the canonical builder instead of a generic update that could
lose curated links or change the graph's direction. Other branches and linked
worktrees retain the original hooks' behavior. Installation is per clone and
never changes global Git settings. The shared checkout's uncommitted D39 text
was excluded by building in a clean isolated checkout of committed devel.
