# AS — demand-read capacity: a bounded notification instead of timed retries (2026-10-28)

Coordinator report: branch `agent/devel-as`, head `9c4bfd171ccc5450ae852a2597351150e543730a`
(docs-only branch; measurement-only task, no product change). Gate on xorinox:
`TOTAL 546 FAIL 1 NOTRUN 11 STATUS FAIL (854s) OVERBUDGET 16` — the FAIL is the
untouched-base K55 slow-mode fsync EIO, identical signature on every gate of
this commit. Evidence: this page; raw data xorinox
`~/efs-artifacts/taskAS-20261028/`. NOTRUN: nothing in this task's scope.
Product defects found: none from this task's mechanism (the gate's K55 base
failure is unrelated and already registered).

**Verdict: PASS — measured, no change.** The `demand_read_alloc` wait loop
**never engaged** in any run: ~5.1 million demand allocations across 10 runs,
including 32 readers over a 2 GiB working set (above the client's 384 MiB
bound) with 32 saturating writers, **and** a deliberately clamped budget
(32 MiB per tier), produced **zero first-failures, zero sleeps, zero waits**.
The AS–AU family's rule for this task is explicit: the cost exists only under
allocation pressure, and the first measurement decides whether the task
exists. Under every pressure configuration measured here, it does not exist.

## Purpose and checklist item

Task AS of the AS–AU wake-ups/polling block
(`docs/status/alpha-1.0-2plus1-checklist.md`, AS checkbox):
`src/client/read.c::demand_read_alloc` retries failed allocations with 1 ms
sleeps (first 100 attempts) then 10 ms sleeps until the inherited RPC deadline
or the ~30 s fallback; releases in `src/client/bufpool.c::efs_buf_free` and
thread-credit destruction never notify sleepers. The candidate change was a
bounded notification; the measurement came first.

## Tested commit and build identity

- Source: devel `9c4bfd171ccc5450ae852a2597351150e543730a`, branch
  `agent/devel-as` (no product commit).
- Measurement build: clean devel plus the temporary, uncommitted counter
  instrumentation described below (built 2026-10-29 on xorinox, 16 CPUs);
  gate build: clean devel `efsd 9c4bfd171... (branch agent/devel-as)`.

## Method

Temporary, uncommitted instrumentation of `demand_read_alloc`
(`dr_prof_patch.py`, retained in the artifacts): calls, first-failures,
attempts, nanosleeps, blocked time per call (total/max) and final failures in
relaxed atomics, a 2 s TSV dump thread (`EFS_DR_PROF_FILE`, scratch-build-only
reader). The loop logic, deadlines and sleeps are byte-for-byte the original
behavior; the instrumentation is proven live by the ~5.1M counted calls.

Loads: a fresh 3-member loopback TCP cluster per run, a 2 GiB dataset (16 ×
128 MiB files, above the client's 384 MiB total buffer bound) pre-written
through one mount, then a 60 s measure window on a fresh reader mount: 32
readers doing 32 KiB reads at random offsets (every partial chunk goes through
`demand_read_alloc`; whole-chunk reads bypass it via the `.ext` path — the
first smoke run taught us that), plus 0/8/32 writers rewriting their own files
to saturate the pool. 3 reps each. One extra run with the client budget
clamped to the minimum (32 MiB per tier: `EFS_DCACHE_HARD_BYTES`,
`EFS_DCACHE_DRAIN_BYTES`, `EFS_DCACHE_READ_BYTES` — existing settings) to
force pressure. Per-op read latencies and `/proc` thread/RSS sampled.

## Results

| config | reads | demand calls | first-fail | sleeps | wait (total) | read p99 | read errors |
|---|---|---|---|---|---|---|---|
| 32 readers, 0 writers (3 reps) | 725k–1.41M | 0.79M–1.55M | 0 | 0 | 0 | 3.2 / 3.2 / 10.5 ms | 0 |
| 32 readers, 8 writers (3 reps) | 31k–419k | 37k–465k | 0 | 0 | 0 | 17.1 / 26.9 / 216.8 ms | 0 |
| 32 readers, 32 writers (3 reps) | 11k–19k | 12k–18k | 0 | 0 | 0 | 1.63 / 1.19 / 1.24 s | 0 |
| 32 readers + 32 writers, clamped 32 MiB/tier | 13k | 14,892 | 0 | 0 | 0 | 628.6 ms | 0 |

The read-latency degradation with writer load is ordinary throughput
contention (CPU/device), not the wait loop: there is no failed allocation
anywhere in the data. Demand-read buffers are transient — allocated per
partial-chunk batch and freed at batch completion — and their dedicated
headroom (drain + read tiers, which writes cannot spend) never exhausted in
these loads, even clamped to 32 MiB per tier.

## Why no change

The task's own stop rule applies: a healthy read never enters the loop, and
under the heaviest pressure this harness could generate the loop is still
never entered — there is no wake-up cost, no release-to-read latency and no
context-switch overhead to remove. Building the notification mechanism would
be new synchronization on a path that never runs, with only its lost-wake
risk to show for it. The 1 ms/10 ms retry ladder and the 30 s fallback stay
as the safety net for pathological cases this measurement could not create
(e.g. buffers pinned by some future path); no defect is claimed for them.

## Gate

`make test-all` on xorinox via `scripts/make_test_summary.sh`, 8 jobs, clean
(unmodified devel) tree:

`TOTAL 546 FAIL 1 NOTRUN 11 STATUS FAIL (854s) OVERBUDGET 16 BUILD 26s`

The one FAIL is `test_b6_unavailable.py` slow mode with the identical
`{'F0': ('EIO', 60.06), ...}` signature as every other gate of this commit —
the untouched-base **K55** failure (deterministic 3/3 on unmodified devel,
registered by the AG/AJ gates). NOTRUN 11: the nine no-RDMA-device commands,
`test_cluster_stop.py` and the sanitizer probe: host limits.

## Limitations

- The measurement covers read pressure from unaligned demand reads with
  saturating writers, at default and clamped-minimum budgets. A pathological
  pin that permanently holds every read buffer was not constructible with the
  existing fault hooks (the drain-fault worker is a server-side fault build);
  if such a path ever appears, the loop and its wake-up cost become real and
  this task can be reopened with the retained harness.
- No product change, so no new regression test, budgets row or settings
  inventory row; the deadline and 30 s fallback are untouched.

## Artifacts

xorinox `~/efs-artifacts/taskAS-20261028/`: `measure/` (9 matrix runs + the
clamped-budget run, JSON + logs), `dr_prof_patch.py`, `as_measure.py`,
`as_run.sh`, `matrix.log`, `gate.log`, `summary.txt`, `timings.tsv`,
`PRUNED.txt`, `SHA256SUMS` (manifest sha256: `9fc7f2e192d8d7bd93fb8789501a20ccc3fa92b2656e359cb709d7c7a4b49e00`). Per-run
stores and both build trees deleted after the runs; the measurement
reproduces from devel `9c4bfd171` plus the retained patch and driver.
