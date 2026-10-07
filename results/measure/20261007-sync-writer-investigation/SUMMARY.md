# Benchmark-only synchronous writes — Oct 7

**Measurement caveat (Oct 7):** these short bounded-write runs mixed initial file creation and replacement. Per-run p99 averages below are historical summaries, not pooled p99. The [corrected latency study](../20261007-latency-validation/SUMMARY.md) separates population from timing and retains every observation. Do not use these older tail figures as steady-state acceptance evidence.

`efs-bench --bench data --sync` opens fragment files with O_SYNC. The profiler
passes it via `--data-sync` and labels synchronous cases. Production store
objects are compiled without EFS_BENCH_BUILD and ignore this option/field;
there is no daemon CLI or default change. Buffered body and checksum are
separate writes, each synchronous. Final exact-length handling also fsyncs
before success, covering a possible shrinking tail or empty write. Direct
writes retain their existing body/checksum writev. Thus this deliberately
conservative diagnostic is not equivalent to one flush per complete PUT.

Both matched binaries include identical O_SYNC/storage changes and the committed
buffered extent fix. Before uses the pre-fairness writer; fair uses reserved
per-slot admission. This isolates admission, not every historical storage change.
40 randomized untraced runs: five repeats × two binaries × buffered/direct ×
two roots; auto/12 writers, QD16, 64 KiB fragments, 64 MiB window, 1.5 seconds
per phase. All pass. VMs off. Separate eight perf/strace cases pass; CPU samples
and aggregate syscall-time percentages do not measure wall-time wait fractions.

| Mode/root | Before GiB/s | Fair GiB/s | Before/fair throughput CV | Mean p99 before/fair ms | Worst max before/fair ms |
|---|---:|---:|---:|---:|---:|
| Buffered/root1 | 0.052 | 0.014 | 169% / 52% | 1014 / 582 | 1596 / 801 |
| Buffered/root2 | 0.026 | 0.026 | 4.5% / 1.7% | 62.6 / 67.4 | 1550 / 87.6 |
| Direct/root1 | 0.107 | 0.025 | 144% / 106% | 784 / 557 | 1746 / 798 |
| Direct/root2 | 0.071 | 0.075 | 7.3% / 5.4% | 21.5 / 26.9 | 1517 / 48.0 |

Root1 means are dominated by intermittent faster repeats. Some phases have only
105–300 completed operations; p99 is consequently close to the extreme samples.
Do not infer a root1 throughput regression from these nonstationary short runs.
Longer repeats and device latency/flush instrumentation are needed.

Separate fair root1 buffered diagnostic: service maximum 561 ms; admission
maximum 561 ms. Fair root1 direct: service maximum 399 ms and admission maximum
399 ms. Long storage-service waits remain independently of admission bypass.
Root2 traced write averages: buffered 2579 us, direct writev 2907 us; futex
waits dominate aggregate thread syscall totals, which must not be treated as
proof of a lock defect. Direct profiles still show XFS allocation work.
O_SYNC exposes persistence costs but cannot isolate device-cache behavior or
verify hardware power-loss guarantees. No sync/durability relaxation performed.

Linux full unit suite and overwrite O_SYNC/final-fsync-failure tests pass.
Profiler tests pass (17), including sync opt-in/case labeling.
