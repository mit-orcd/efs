# Benchmark-only synchronous writes — Oct 7

## Purpose and linked item

Measure isolated O_SYNC engine-write cost and compare writer admission variants
under synchronous writes, as a diagnostic for the writer-investigation campaign
documented in `docs/how-it-works/performance.md` ("Isolated writer
investigation" → "synchronous writer measurements"). No dedicated W number; run
added by commit `e03efb488` ("bench: add isolated O_SYNC engine-write
diagnostics and evidence").

**Measurement caveat (Oct 7):** these short bounded-write runs mixed initial
file creation and replacement. Per-run p99 averages below are historical
summaries, not pooled p99. The [corrected latency
study](../20261007-latency-validation/SUMMARY.md) separates population from
timing and retains every observation. Do not use these older tail figures as
steady-state acceptance evidence.

## Verdict

PASS as a diagnostic: all 40 randomized runs returncode 0 with zero errors;
eight perf/strace cases pass; Linux full unit suite plus overwrite
O_SYNC/final-fsync-failure tests pass; profiler tests pass (17), including sync
opt-in/case labeling. No acceptance claim: O_SYNC exposes persistence costs but
cannot isolate device-cache behavior or verify hardware power-loss guarantees.

## Source commit and build identity

Commit `e03efb488` adds the `--sync`/`--data-sync` benchmark-only option.
`efs-bench --bench data --sync` opens fragment files with O_SYNC; production
store objects are compiled without EFS_BENCH_BUILD and ignore this
option/field — no daemon CLI or default change. Both matched binaries include
identical O_SYNC/storage changes and the committed buffered extent fix; "before"
uses the pre-fairness writer, "fair" uses reserved per-slot admission (commit
`b3c40474a`), isolating admission rather than every historical storage change.
Binary hashes not recorded in the run.

## Environment

- xorinox host (NUC), local `efs-bench` runs; no cluster, VMs off.
- Roots: root1 `/data1/efs/evidence/bench`, root2 `/home/efs/additional-work-dir/bench`.
- Buffered body and checksum are separate writes, each synchronous; final
  exact-length handling also fsyncs. Direct writes retain their existing
  body/checksum writev. This deliberately conservative diagnostic is not
  equivalent to one flush per complete PUT.

## Method

Driver: `efs-sync-admission-ab.py` (retained in this directory), importing
`bench_profile` from `/tmp/efs-writer-study-20261007/scripts`. 40 randomized
untraced runs: five repeats × two binaries × buffered/direct × two roots;
auto/12 writers, QD16, 64 KiB fragments, 64 MiB window, 1.5 s per phase; seed
17231; per-run temp dirs under each root removed afterward. Separate eight
perf/strace cases (retained as `before-*/fair-*` flat/baseline/strace files).
CPU samples and aggregate syscall-time percentages do not measure wall-time
wait fractions.

## Results

| Mode/root | Before GiB/s | Fair GiB/s | Before/fair throughput CV | Mean p99 before/fair ms | Worst max before/fair ms |
|---|---:|---:|---:|---:|---:|
| Buffered/root1 | 0.052 | 0.014 | 169% / 52% | 1014 / 582 | 1596 / 801 |
| Buffered/root2 | 0.026 | 0.026 | 4.5% / 1.7% | 62.6 / 67.4 | 1550 / 87.6 |
| Direct/root1 | 0.107 | 0.025 | 144% / 106% | 784 / 557 | 1746 / 798 |
| Direct/root2 | 0.071 | 0.075 | 7.3% / 5.4% | 21.5 / 26.9 | 1517 / 48.0 |

Root1 means are dominated by intermittent faster repeats. Some phases have only
105–300 completed operations; p99 is consequently close to the extreme samples.
Do not infer a root1 throughput regression from these nonstationary short runs.

Separate fair root1 buffered diagnostic: service maximum 561 ms; admission
maximum 561 ms. Fair root1 direct: service maximum 399 ms and admission maximum
399 ms. Long storage-service waits remain independently of admission bypass.
Root2 traced write averages: buffered 2579 µs, direct writev 2907 µs; futex
waits dominate aggregate thread syscall totals, which must not be treated as
proof of a lock defect. Direct profiles still show XFS allocation work.

## Limitations and remaining gates

- Nonstationary short runs; longer repeats and device latency/flush
  instrumentation are needed.
- O_SYNC does not establish SSD power-loss guarantees; no sync/durability
  relaxation was performed.
- Tail figures superseded for acceptance purposes by
  `results/measure/20261007-latency-validation/`.

## Evidence index

| File | sha256 (16) |
|---|---|
| results.json | 593ce61559beacbc |
| efs-sync-admission-ab.py | 91f9e0222f039de8 |
| efs-sync-unit.log | 925d182f3caa8087 |
| efs-sync-cli.log | 2ee35eb554c2c68c |
| profile-tests.log | 0771b0ed38fd237b |
| before-root1-buffered/flat.txt | ad86f3f34b484cba |
| fair-root1-direct/strace.txt | 0388a724fd3bafdf |
| before-root2-direct/strace.txt | b29f145e4df1d7da |

**Raw evidence note (2026-10-23):** the bulk data in `bench` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
