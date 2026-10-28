# Project rules

## No backward compatibility

EFS is not in production. Unless the user explicitly says otherwise, never
design, implement, test or require backward compatibility: no preserved old
log, store, snapshot, wire or row encodings, no migration of retained data,
no downgrade support, no mixed-build clusters, no old-client fallbacks. A
format or placement change takes effect on a freshly formatted cluster.
Keep fail-closed refusal: a binary meeting an encoding, store or peer it
does not support stops with a clear error and never silently skips or
misreads it. Do not add legacy-compatibility gates to reviews or plans.

## Evidence and reproducibility

Commit concise evidence summaries and clear reproduction instructions. Do not
commit raw run artifacts, including daemon/build logs, profiler recordings,
packet captures, syscall traces, per-operation dumps, generated test payloads,
or copied runtime storage (Raft/WAL/data directories). This applies to both
`docs/` and `results/`, including compressed files and symlinks to raw captures.

A committed evidence summary must state:

- The purpose, linked W/D item or release gate, and PASS/FAIL/NOTRUN verdict.
- The exact tested source commit, relevant changes, and binary/build identity.
- The environment: cluster/hardware, topology, configuration, enabled features,
  transport, buffered/direct I/O, and fixture size/quota.
- Exact commands or a committed reproduction script, prerequisites, workload,
  concurrency, durations/deadlines, expected checks and cleanup instructions.
- Results and relevant failures, baseline/comparison, limitations and remaining gates.

Keep reproduction scripts in the repository; keep their raw output outside Git
or under ignored `/artifacts/`. Where raw captures are retained externally,
record their location and useful hashes in the summary. Reproduction must not
depend on an unavailable private capture. Small deterministic regression
fixtures needed by tests are source assets, not run artifacts.

On test hosts, `/data1/efs` holds only infrastructure at the top level:
cluster stores (`n1`–`n4`), `src`, `releases`, `devops`, `deps`, `logs`,
mounts, client directories and VM images. Raw evidence that a summary cites
goes under `/data1/efs/evidence/<name>` (cite that path), and scratch run
trees go under `/tmp` or a run directory that is deleted when the run ends.
Never leave work directories at the top of `/data1/efs`.
Once a summary records the method, prune bulk from cited evidence: copied
runtime stores, generated payloads and build trees are recreated from the
commit and the method, not kept. Keep text logs, results and profiler
captures, write a `PRUNED.txt` listing what was removed, and note the
deletion on the citing page. Keep bulk only while a page marks it as kept
for an open investigation.

Raw evidence committed before 2026-10-21 was moved off devel. It is preserved
unchanged at tag `archive/raw-evidence-20261021`; read
it with `git show archive/raw-evidence-20261021:<path>`. Devel keeps the
summaries under `results/`, and links to removed files name the archive path.
Keep unresolved findings and release verdicts traceable. Do not treat missing raw captures as a passing gate.
Update `docs/status/README.md` whenever an active status page or verdict changes.

## Canonical defect-navigation graph

When `tools/graphify/scope.json` exists, rebuild with
`sh scripts/rebuild-code-graph.sh`, not the generic `graphify update .` command.
See `tools/graphify/README.md` for directed relationship semantics, known-bug
validation and local hook installation. Run `python3 tests/test_code_graph.py`
after rebuilding. Treat missing edges and parser warnings as coverage gaps;
curated test links and literal document mentions are not runtime proof.
