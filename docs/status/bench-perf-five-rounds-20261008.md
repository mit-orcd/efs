# W113: five container profiling rounds — October 8, 2026

**Scope:** benchmark-only `devel-bench`, xorinox_efs private
`/data1/efs/bench/w90-perf5-20261008`. Production storage/services, global
filesystem/device policy and shared `devel` source were not changed.

**Evidence:** [profile, measurement and syscall ledger](../../results/measure/20261008-w90-perf-five-rounds/README.md).
Every round has measured-phase CPU profiles and two independent unprofiled
samples. A separate 60-second eight-worker baseline/final comparison includes
measured-phase hardware counters. Full traces, perf.data and binary snapshots
remain remote; source archives and reports are retained in the ledger.

**Accepted changes:** optimized synthetic payload generation, bounded physical
punch retirement batches with grouped actual allocation accounting, and dirty
namespace tracking to avoid redundant container directory fsync. Retired slots
become reusable only after successful punching; free-space pressure drains early;
all remaining punching is timed. Required body barriers, checksums and full
final expected-version verification remain. Profile-control errors invalidate
runs. Synthetic generation improvements are benchmark overhead reductions,
not production-engine improvements.

**Rejected:** sorting/coalescing random retirement batches did not demonstrate a
throughput gain and was removed. Earlier no-op/pre-rollback trials remain
explicitly labeled rather than counted as improvements.

**Observed:** four-worker mean throughput increased about 33% over the starting
container path. The longer eight-worker sample increased from 31,127 to 35,803
ops/s (15%); CPU time/op decreased about 40%. Eight-worker write p99 increased
from 2.927 to 3.423 ms. These sequential shared-host samples are not statistical
confidence intervals, SSD endurance gates, or production cluster measurements.

**Strace:** baseline and final measured-phase read/write counts match completed
operations; all transfers are complete aligned records within the 1 GiB files.
Punched bytes match every retirement; final backlog is zero. No syscall errors
or measured opens/closes/ioctls. Allocation-stat calls per retirement decreased
from 2 to about 0.125; unchanged-directory flushes decreased to zero. Required
body flush counts match the benchmark counters. Trace parsing was corrected
for unfinished-call whitespace while replaying saved traces; this is not an
application fault claim.

**Still open:** W113 broader lifetime/endurance gates and production promotion.
The volatile worker-local index is not durable recovery, reader-pin authority,
distributed ownership or EFS GC. W44/W71/W75/W87 remain separately governed.
See [churn contract](../how-it-works/storage-churn.md) and the earlier
[shared-range checkpoint](bench-scaling-ranges-20261008.md).


**Final validation:** 47 targeted unittest methods plus CLI ownership/parsing
checks pass; benchmark/daemon build checks have no compiler warnings; all five
documentation gates pass. Source identities verify, every profile has zero lost
samples, and completed scratch fixtures are removed.

Numbering correction (Oct 12): this benchmark is W113. Historical `w90-*`
fixture and evidence paths retain their names; W90 denotes scratch expiry.
