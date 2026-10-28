# EFS defect-navigation graph

The canonical graph is `graphify-out/graph.json`. It is a directed multigraph:
parallel relationship types are retained. Source hashes and extractor identity
are in `source-manifest.json`; `GRAPH_REPORT.md` records limitations.
`graph.html` is rebuilt as a community overview above 5,000 nodes. This is
navigation for a source review, never a correctness or performance verdict.

## Scope and rebuild

`scope.json` selects tracked first-party C/header/Python/shell source in `src`,
`include`, `tests` and `scripts`, including live oracles and fault models.
Architecture and user docs, the current decision/work-item ledgers and explicitly
listed contracts are indexed. A proposal remains a proposal; a historical status
inside a retained ledger is not promoted to a current verdict. Dated results,
archive/review discussions, generated architecture duplicates and vendored
implementations are excluded. `.graphifyignore` also constrains ordinary scans;
the positive manifest is authoritative for the canonical build.

Install `graphifyy==0.9.97` in a dedicated environment (the build records the exact
extractor module hash; revalidate on upgrades). From the repository root:

```sh
make code-graph-check
python3 scripts/install-code-graph-hooks.py
```

The shell wrapper selects the interpreter from the installed graphify launcher's
shebang. Alternatively run `scripts/build-code-graph.py` directly in the Python
environment that owns graphify. Extraction is local, four workers, zero LLM/API
calls, and refuses missing configured inputs or changed sources during the run.
Stage new source/configuration files before rebuilding: the scan uses tracked
files, not every file present on disk. Do not use generic `graphify update .` as
the canonical builder: it does not recreate EFS's contract/oracle links.

The optional local hook installer preserves existing hooks and their non-graphify
behavior. On the primary checkout it replaces generic graphify rebuilds with this
builder; linked worktrees retain the generic hooks' skip behavior. Other branches
without this configuration retain their original hooks. A hook rebuild changes
generated files after a commit; stage an explicitly reviewed graph snapshot when
it should be included in a commit. Hooks are local Git configuration, so each clone
must run the installer. No system/global Git setting is changed.

`--out PATH` builds a review candidate elsewhere; `--validate-only` checks an
existing output. For byte comparisons use `PYTHONHASHSEED=0`, the same extractor
and the same input tree. The source base commit may differ when input bytes do
not; compare the manifest's per-file hashes as well as the base commit.

## How to query

Use an exact seed and a relationship filter for code paths:

```sh
graphify query host_apply --seed host_apply --context calls --budget 1500
graphify explain W145-snapshot-unproven-tail
```

`contains`, `calls`, `imports`, `indirect_call` and other AST relationships retain
the extractor's confidence labels. Exact C include paths resolve to selected
headers. Unresolved imports have explicitly labelled boundary nodes: they may be
external or unresolved project-local dependencies. Missing call targets are
reported and omitted, never manufactured as resolved calls.

Markdown headings and literal symbol mentions are indexed deterministically.
`mentions_symbol` is not `calls`, `requires` or a verified behavioral contract.
Ambiguous symbol names are left unlinked. `references_path` from the Makefile is
only a literal path mention, not evidence every platform builds or runs it.

`known-bugs.json` supplies three reviewed navigation checks. `documents_finding`,
`affects`, `exercises` and `regression_for` are explicitly curated relationships,
not independently discovered AST calls. Validation also requires actual AST calls
on the W127/W145 production paths and the W125 C oracle-to-implementation edge,
and checks the oracle's distinguishing source literals. Missing/reversed calls,
missing oracle links and changed oracles fail validation. A curated issue label
alone cannot pass these checks. Update a changed case only after reviewing its
new source and test, never merely to make validation green.

## What this does not establish

The extractor is not a C preprocessor, whole-program analyzer or concurrency
checker. Function pointers, embedded generated C/shell, conditional builds and
macros may be incomplete. Parse warnings and unresolved counts are recorded in
the report. It does not infer lock ownership, state writes, barriers, durability
ordering or happens-before. Those need source inspection, targeted annotations
and executable tests; inventing such edges would make defect searches misleading.
The graph does not replace the full test suite or live TCP/RDMA gates.
