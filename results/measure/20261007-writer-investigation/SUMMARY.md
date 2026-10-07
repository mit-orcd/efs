# Writer investigation — completed Oct 7

The interrupted xorinox batch completed: 108 configurations, 324 randomized
uninstrumented repeats and 108 separate DWARF profiles all passed. QD1/16/64,
inline/two/automatic writers (12 automatic), buffered/direct reads and writes,
and each storage root plus both were measured with 64 KiB fragments and a
bounded 64 MiB window. Profiling excludes population, worker startup and cleanup.
Source/binary provenance is in manifest.json; full source snapshot and reports
remain under ignored logs/efs-writer-study-20261007 and remote /tmp study paths.
No production deployment, cluster restart or filesystem tuning was performed.

## Findings and production changes

- **Fixed:** inline mode previously skipped root-selection initialization.
  New inline writes now use all roots; overwrites retain their existing root.
  This is shared production writer code, not just benchmark code.
- **Fixed:** repeated buffered O_TRUNC overwrites discarded XFS extents and paid
  allocation/btree costs. The actual shard writer now preserves buffered extents,
  checks the completed length with fstat and truncates only a stale longer tail.
  Failed length checks/truncation return IO. Shrink, grow, empty writes and checksum
  lengths are tested. Direct I/O and existing durability barriers are unchanged.
- **Worth fixing next:** two-writer admission contention. At QD64 it averaged
  4.48 ms admission versus 113 µs service in one diagnostic. A separate gated
  context-switch run recorded 59 switches/job versus 8 inline and 11 automatic.
  Simply replacing broadcast with signal was rejected after 59/180 experiments:
  maximum waits rose from ~62 ms to 1.5 seconds despite better median/p99.
  Design fair FIFO admission with reserved handoff and anti-bypass protection;
  validate worst waits, deadlines, shutdown and throughput before adoption.
- **Operational follow-up:** root2 sync costs substantially more: append+fsync
  averaged 5.49 ms versus 0.48 ms; create/rename/directory-fsync 8.25 versus 0.91 ms.
  Root1 is HP FX900 Pro 4 TB XFS/noatime/logbsize256k; root2 is Kingston 1 TB
  through rl-home XFS/relatime/logbsize32k. Hardware and filesystem differ;
  these measurements cannot attribute the gap to one setting. Do not weaken sync.
- **No rewrite justified:** cached reads spend ~52–58% of sampled CPU in existing
  AVX512 BLAKE3 and ~13–18% in kernel copies. After the buffered fix, write CPU
  shifts to copy_page_from_iter_atomic (~40%), rather than extent allocation.

## Matched buffered overwrite acceptance

36 randomized three-second runs (three repeats each, 12 writers), all error-free.
Means of throughput and per-run p99 below. These are warm bounded overwrite
results, not physical-media bandwidth or FUSE/end-to-end throughput.

| Roots | QD | Before GiB/s | After GiB/s | Before p99 µs | After p99 µs |
|---|---:|---:|---:|---:|---:|
| root1 | 16 | 3.79 | 25.91 | 377 | 69 |
| root1 | 64 | 4.10 | 16.31 | 9401 | 2407 |
| root2 | 16 | 3.18 | 25.51 | 6305 | 69 |
| root2 | 64 | 3.11 | 16.38 | 12957 | 2400 |
| roots2 | 16 | 5.37 | 22.89 | 595 | 180 |
| roots2 | 64 | 3.69 | 18.93 | 12957 | 4917 |

Throughput improves about 4–8× and p99 improves across these configurations.
Rare maximum-latency stalls remain: root2/QD16 mean max is 290 ms after versus
260 ms before; one combined-root run reaches 641 ms. Background QEMU load was
active and 26/108 original configurations had throughput CV above 15%.
Do not claim every tail improves or a dedicated-host maximum.

A 12-run cold-create primitive control (16 callers, 1024 fresh files) completes
in 3–5 ms in both variants; too short/noisy to establish a small effect. The
fixture emulates legacy open flags and omits the new fstat; it is a control,
not a literal old-binary end-to-end comparison. 42 raw device runs also pass.

Separate stats-on/off runs show about 5–7% overhead in stable two-writer cases;
other comparisons are inconclusive under background stalls. Statistics remain
disabled by default. perf sched was unavailable because tracefs is restricted;
software context-switch counters and short strace were used without changing
permissions. Traced/instrumented rates are not acceptance baselines.

## Validation and next help

Final Linux full unit suite, actual-store overwrite/error/EINTR tests, CLI smoke
and fault cases pass. Four final isolated buffered/direct write/read profiles
and report generation pass. Sustained 32-producer routing tests pass on Linux
and locally with ASan/UBSan. Existing profiler tests pass (16). Their fault-injection fixtures intentionally
print report FAIL; the suite exits successfully and real profiles all pass.

Next useful user validation: deploy normally, run a real append/overwrite/read
workload plus the POSIX suite on a quiet cluster, retaining RSS, throughput and
latency logs. The present gain is specific to buffered overwrites; do not infer
a direct-I/O or first-write gain. Fair writer admission remains separate work.
