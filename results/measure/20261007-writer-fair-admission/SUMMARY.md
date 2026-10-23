# Reserved writer admission — Oct 7

## Purpose and linked item

Validate the writer-admission fairness fix (fallback waiters form a FIFO per
slot; an empty slot is reserved for its oldest waiter so newer fast-path callers
cannot bypass it). Follow-up to the isolated writer investigation documented in
`docs/how-it-works/performance.md` ("Isolated writer investigation"), which
showed a single-waiter signal experiment caused starvation. No dedicated W
number; run added by commit `b3c40474a` ("writer: reserve empty slots for FIFO
fallback waiters", touches `src/server/writer.c` + `tests/test_writer_routing.c`).

## Verdict

PASS: all 40 matched benchmark runs completed with returncode 0 and zero I/O
errors; deterministic ordered-admission regression and sustained-contention
tests pass locally with ASan/UBSan and on Linux. Result is a deliberate
trade-off, not a universal improvement — see Results.

## Source commit and build identity

Fix commit `b3c40474a`. The two matched binaries ("before" = pre-fairness
writer, "fair" = reserved per-slot admission) both include the committed
buffered extent fix and unchanged direct I/O; only writer admission differs.
Binary hashes not recorded in the run.

## Environment

- xorinox host (NUC), local `efs-bench` engine-write benchmark; no cluster, VMs off.
- Storage roots: root1 `/data1/efs/evidence/bench`, root2 `/home/efs/additional-work-dir/bench`.
- Buffered and direct I/O both measured; 64 KiB fragments, 64 MiB bounded
  overwrite window (warm-window diagnostic, not sustained media or FUSE speed).

## Method

40 randomized untraced runs: five repeats × two binaries × buffered/direct × two
roots, order shuffled with fixed seed; QD16, writers auto (12), 1.5 s per phase.
Driver pattern matches `efs-sync-admission-ab.py` in the sibling
`20261007-sync-writer-investigation` run (per-run temp dirs under each root,
removed afterward). Unit-side: ordered-admission regression and sustained
contention under ASan/UBSan; five simulated-service controls with 16 callers,
12 writers, 8192 operations.

## Results

Simulated-service controls: admission maximum reduced from 83.2–83.5 ms to
0.197–0.224 ms, mean service unchanged at ~152 µs.

| Mode/root | Before GiB/s | Fair GiB/s | Mean p99 before/fair µs | Worst max before/fair ms |
|---|---:|---:|---:|---:|
| Buffered/root1 | 27.301 | 25.952 | 60 / 92 | 68.25 / 3.91 |
| Buffered/root2 | 27.553 | 26.230 | 60 / 90 | 23.50 / 51.75 |
| Direct/root1 | 4.666 | 4.311 | 714 / 1043 | 1217.97 / 604.14 |
| Direct/root2 | 3.273 | 3.393 | 517 / 598 | 297.19 / 50.06 |

Fairness has a cost: buffered throughput falls ~5% and p99 rises. Worst
observed direct waits shrink, but root1 throughput remains variable (CV 38%
before, 44% after). Root2 buffered maximum worsens in one run.

## Limitations and remaining gates

- Do not claim universal latency improvement: fair admission removes
  demonstrated starvation but does not resolve root1's device/filesystem
  variability.
- Warm-window diagnostics only; not sustained-media or FUSE acceptance numbers.
- Per the latency-study qualification in `performance.md`, these short bounded
  runs mixed population and replacement; use
  `results/measure/20261007-latency-validation/` for steady-state tail-latency
  evidence.

## Evidence index

| File | sha256 (16) |
|---|---|
| results.json | 3fef732e5d2b55d8 |
