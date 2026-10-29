# AG — the client buffer pool mutex during slab growth (2026-10-28)

Coordinator report: branch `agent/devel-ag`, head `9c4bfd171ccc5450ae852a2597351150e543730a`
(docs-only branch; measurement-only task, no product change). Gate on xorinox:
`TOTAL 546 FAIL 1 NOTRUN 11 STATUS FAIL (915s) OVERBUDGET 15` — the one FAIL is
`test_b6_unavailable.py` slow mode, an **untouched-base** failure (the deployed
tree is unmodified devel; the intel1 gate and a solo base rerun reproduce it
deterministically), registered as **K55**. Evidence: this page; raw data
xorinox `~/efs-artifacts/taskAG-20261028/`. NOTRUN: RDMA-region cost inside
slab growth (xorinox is TCP; on TCP `efs_rdma_zc_region_add` is a registry
append under `g_zc_lock` — hardware MR registration is lazy and elsewhere).
Product defects found: one pre-existing on devel (K55); none from this task's
mechanism. No product change.

**Verdict: PASS — measured, no change.** `g_bp_mu` wait share never exceeded
**0.0004 %** at 32 concurrent I/O streams (threshold: 5 %); a cold slab growth
holds the mutex **25–54 µs** and happens a handful of times per client lifetime
(≤ 10 slabs grown in any run, budget ceiling 12 at default bounds).

## Purpose and checklist item

Task AG of the Stage 1 hot-path serialization block
(`docs/status/alpha-1.0-2plus1-checklist.md`, AG checkbox):
`src/client/bufpool.c` guards reservations, allocations, frees and metadata
charges with one mount-wide mutex `g_bp_mu`; a cold slab allocation holds it
across `malloc(SLAB_BYTES)` (32 MiB), `efs_rdma_zc_region_add` and the free-list
fill. Confirmed serialization point, never measured; measure-first task.

## Tested commit and build identity

- Source: devel `9c4bfd171ccc5450ae852a2597351150e543730a`, branch
  `agent/devel-ag` (no product commit; the branch carries only this page and
  the checklist/README updates).
- Measured build (instrumented efs-fuse; see Method): same commit, built
  2026-10-29 on xorinox; gate build: `efsd 9c4bfd171... (branch agent/devel-ag,
  built 2026-10-29 on xorinox)`.

## Host and configuration

xorinox (16 CPUs), quiet (load 0.00 before start; Task T had finished). Each
run: a fresh 3-member loopback TCP cluster (`tests/k33_daemons.py`,
`EFS_RAFT_OBS=1`, quota 4G, stores under `$HOME/efs-taskAG/tmp`), one FUSE
mount of the instrumented efs-fuse; stores `edelete`d after each run.

## Method (measurement)

Temporary, **uncommitted** instrumentation of `src/client/bufpool.c`
(transformer and drivers retained in the artifacts): every
`g_bp_mu` acquisition wrapped with wait/hold accounting in relaxed atomics, the
slab-growth section inside `buf_alloc` timed separately, a 2 s TSV dump thread
(`EFS_BP_PROF_FILE`, scratch-build only, never a product setting). `perf lock`
is unusable on xorinox (perf/kernel mismatch); the in-process counters are the
primary source, as the task allows.

Loads, 60 s measured window per run, 3 repetitions: 1/8/32 writers rewriting a
bounded per-writer working set (16 MiB files, 1 MiB writes, fsync on close),
plus `mix32`: 24 writers + 8 readers over a 512 MiB pre-written dataset. A 10 Hz
`stat` loop on the same mount gives the kit's per-RPC latency; daemon `raft-obs`
lines give pump/lock high-waters. Cold window = the first 60 s from client boot
(all slab growth happens there); with a 60 s measure window the two coincide,
so the table reports the whole window and the growth columns isolate the cold
cost.

Wait share denominator: wall × efs-fuse thread count (65–133 threads from
`/proc/pid/status`).

## Results

Medians of 3 runs (spread in parentheses where notable):

| config | write MiB/s | acq/s | mean wait | mean hold | wait max | hold max | growths (sum/3) | growth max | wait share |
|---|---|---|---|---|---|---|---|---|---|
| 1 writer | 52.7 (44.9–52.8) | 3,002 | 62 ns | 60 ns | 355 µs | 44 µs | 0* | 44 µs | 0.00028 % |
| 8 writers | 61.9 (55.2–64.8) | 3,633 | 54 ns | 69 ns | 30 µs | 54 µs | 2 | 54 µs | 0.00029 % |
| 32 writers | 40.3 (37.5–49.6) | 2,532 | 69 ns | 80 ns | 503 µs | 36 µs | 1 | 25 µs | 0.00034 % |
| 24 w + 8 r | 35.0 (34.8–40.7) | 4,487 | 61 ns | 68 ns | 751 µs | 105 µs | 20 | 29 µs | 0.00040 % |

\* growth deltas inside the window miss the first seconds before the first 2 s
dump; `slabs_end` per run (2 / 8 / 9–10 / 9–10) bounds total growth, and
`growth_max` is a cumulative maximum, so the per-growth cost is exact.

Kit cross-checks: `stat` p50/p99 0.34/9.5 ms (1 writer) degrading to
90.8/2011 ms at 32 writers on 16 CPUs — load, not the pool: `g_bp_mu` wait does
not move. Daemon `raft-obs` pump/lock high-waters were the usual loopback
values (pump_hold_max ≤ 65 ms worst single spike, tick_max ≤ 0.8 ms).

Observations:

- The pool is nearly free-running: ~2.5–4.5k acquisitions/s at these throughputs
  (a 128 KiB chunk charge per alloc/free pair), mean wait ~60–70 ns, mean hold
  ~60–105 ns. Contention does not grow from 1 to 32 streams.
- Slab growth, the suspected long section, costs 25–54 µs (malloc of 32 MiB is
  an mmap; pages are touched on first use, outside the lock's lifetime cost
  here) and happens ≤ 12 times per client at default bounds. Amortized over any
  run it is unmeasurable in the totals.
- Throughput differences between configs track CPU oversubscription of the
  FUSE/client stack, not the pool lock (wait share flat while throughput moves
  2×).

## Gate

`make test-all` on xorinox via `scripts/make_test_summary.sh`, 8 jobs, clean
(uninstrumented) tree of the same commit:

`TOTAL 546 FAIL 1 NOTRUN 11 STATUS FAIL (915s) OVERBUDGET 15 BUILD 21s`

The one FAIL is `EFS_B6_MODES=slow python3 tests/test_b6_unavailable.py`, the
same `slow: fsync refused: {'F0': ('EIO', 60.06), ...}` signature as the intel1
gate of the same commit (Task AJ). **Base comparison:** this branch carries no
product change (the deployed tree is `git archive` of unmodified devel
`9c4bfd171`) and a solo rerun of the test on intel1 reproduced the identical
failure — the untouched base fails the same way, deterministically (3/3).
Registered as **K55** (both checklist tables, blocks alpha: yes; row and trace
in `aj-export-reference-lock-20261028.md`). NOTRUN 11: the nine no-RDMA-device
commands, `test_cluster_stop.py` (no `~/git/cluster`),
`test_sanitizer_gate.py` (probe NOTRUN): host limits.

## Limitations and NOTRUN

- TCP only (xorinox has no RDMA device). On an RDMA client the slab-growth
  section additionally pays `efs_rdma_zc_region_add`; on this build that call
  is a bounded registry append (no verbs registration at add time), so the
  measured 25–54 µs stands for TCP; the RDMA column of the same measurement is
  NOTRUN here — node9901 runs are the natural follow-up if AG is ever revisited
  after Task AI's RDMA work.
- No product change, so no new regression test, no budgets row, no settings
  inventory row; the exact-accounting invariants (credits, aggregate/metadata
  bounds, drain/read headroom, thread-exit refunds) are untouched.

## Artifacts

xorinox `~/efs-artifacts/taskAG-20261028/`: `measure/*.json` (one per run),
`measure/*.log`, `matrix.log`, `bp_prof_patch.py`, `ag_measure.py`,
`ag_run.sh` (the temporary tooling, for reproduction), `gate.log`,
`summary.txt`, `timings.tsv`, `SHA256SUMS` (manifest sha256: `02b4c9e802971d624a7d20d9e7fce9859ee27156b512c980ac9f9e1b5d2920fd`).
Scratch trees deleted after the run; the measurement is reproduced from devel
`9c4bfd171` plus the retained patch script.
