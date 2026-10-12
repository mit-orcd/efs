# Project rules

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

Existing committed raw evidence is historical. Migrate it to adequate summaries
and reproduction instructions before removing it; keep unresolved findings and
release verdicts traceable. Do not treat missing raw captures as a passing gate.
Update `docs/status/README.md` whenever an active status page or verdict changes.
