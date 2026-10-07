# Latency validation — nuc, Oct 7

Measurement fix: `8f24defb`. The data benchmark now prepopulates bounded
windows before timing, counts every operation in a mergeable histogram, reports
p99 bounds and retains the exact observed integer-microsecond maximum. The
former collector could silently stop after 1,048,576 samples per worker; the
short earlier nuc runs were below that cap, but they mixed creation and
replacement and supplied weak evidence about the tail.

The matched study compares current direct I/O with the **private conditional
extent-retention candidate**, both using the same corrected benchmark and fair
writer admission. The candidate patch is retained here for reproducibility;
it has not been applied to the daemon.

Configuration: nuc, auto four writers, QD16, 64 KiB fragment payloads, 64 fragments
per worker (1,024 resident fragments / 64 MiB payload), direct writes, each root
separately, O_SYNC off/on. Root 1 is SATA MK0800GEYKE/XFS; root 2 is Samsung
970 EVO NVMe/XFS through `/data2/efs -> /home/efs/additional-work-dir`.
The four existing efsd processes remain running; host load and device/mount
provenance are retained. This is an engine benchmark, not a FUSE or cluster
end-to-end result, power-loss durability test, or open-loop arrival model.

Run order: five randomized 30-second untraced repetitions per configuration,
then eight separate 30-second perf captures, then eight separate 5-second
strace captures. Report generation follows all workloads. Traced throughput
is diagnostic only. Histograms are pooled by counts; per-run p99 ranges are
reported separately. A count above 10,000 is a heuristic, not a confidence bound.


## Untraced results

Forty accepted runs: five 30-second repeats per configuration. One initial
SATA/current/non-sync run overlapped a CLI check and was replaced after all
profiles, traces and report generation; the original remains marked excluded.
All 40 accepted runs have zero errors, 16 active workers and exact sample-count
coverage. They contain **11,767,159** measured operations in total; each run has
at least **16,582** observations. These are measured closed-loop store-call
tails, not device or FUSE request latency.

GiB/s below is the mean ± sample standard deviation of five repeats. P99 is
pooled from all operation counts, shown as its histogram interval. The interval
is quantization error, **not a statistical confidence interval**.

| Device / O_SYNC | Current GiB/s | Candidate GiB/s | Current pooled p99 ms | Candidate pooled p99 ms | Current / candidate worst max ms |
|---|---:|---:|---:|---:|---:|
| SATA / off | 0.4069 ± 0.0023 | 0.4071 ± 0.0034 | 7.232–7.263 | 6.080–6.111 | 74.3 / 32.5 |
| SATA / on | 0.3010 ± 0.0056 | 0.3584 ± 0.0078 | 7.648–7.679 | 5.984–6.015 | 30.7 / 29.1 |
| NVMe / off | 1.3919 ± 0.4119 | 1.8500 ± 0.4508 | 2.752–2.767 | 1.736–1.743 | 799.4 / 30.5 |
| NVMe / on | 0.0383 ± 0.0002 | 0.0341 ± 0.0003 | 62.720–62.975 | 40.704–40.959 | 92.6 / 61.4 |

| Device / O_SYNC / version | Throughput range GiB/s | Per-run p99 upper-bound range ms | Total samples |
|---|---:|---:|---:|
| SATA / off / conditional | 0.401–0.410 | 4.415–7.295 | 1,000,629 |
| SATA / on / conditional | 0.345–0.364 | 4.191–6.687 | 880,861 |
| NVMe / off / conditional | 1.060–2.122 | 1.279–2.335 | 4,546,607 |
| NVMe / on / conditional | 0.034–0.035 | 39.423–44.031 | 83,865 |
| SATA / off / current | 0.405–0.411 | 6.783–7.519 | 1,000,003 |
| SATA / on / current | 0.295–0.308 | 5.503–8.639 | 739,858 |
| NVMe / off / current | 0.821–1.787 | 1.919–4.511 | 3,421,027 |
| NVMe / on / current | 0.038–0.039 | 54.783–66.047 | 94,309 |

The candidate improves SATA O_SYNC throughput about 19% and mean NVMe non-sync
throughput about 33%. SATA non-sync throughput is effectively unchanged.
NVMe O_SYNC throughput **regresses about 11%**, despite a lower p99. NVMe
non-sync variation remains large in both versions; retain the full ranges,
not only the best run. Current code has an observed 799 ms NVMe outlier that
is invisible to its pooled p99. Histograms alone cannot identify that stall's
cause. Thirty seconds does not prove the SSD has reached thermal or flash
steady state, and the five repetitions are not a worst-case bound.

## Separate perf captures

Eight 30-second captures at 199 Hz, cycles + DWARF, gated to the timed loop.
All requested flat, per-thread, caller reports and available C annotations
were generated successfully. No lost samples were reported. Sample counts
range from 870 to 12,618; low-percent ranks in the 870-sample capture remain
weak evidence. Sources and matching binaries are retained with raw profiles.

| Device / O_SYNC | Current / candidate samples | Approximate cycles per completed write, current → candidate |
|---|---:|---:|
| SATA / off | 3,657 / 1,813 | 201.5k → 68.2k |
| SATA / on | 3,613 / 2,434 | 254.7k → 114.0k |
| NVMe / off | 12,618 / 2,446 | 227.4k → 69.7k |
| NVMe / on | 1,182 / 870 | 348.9k → 217.4k |

Current non-sync profiles show XFS extent-busy trimming and btree leaf-key work
at 6.21% + 5.90% on SATA and 4.73% + 5.33% on NVMe. The NVMe caller report
connects this to `do_truncate` / `xfs_itruncate_extents_flags` and allocation
under the write path; `do_truncate` has 25.84% inclusive share. Candidate
profiles remove these leading allocation/truncation costs. This supports the
CPU-saving mechanism of preserving existing extents. These single-capture,
period-weighted cycle estimates are not repeatability bounds or elapsed time.

Candidate residual work includes kernel spinlocks, path lookup, direct-I/O
page handling and writer-pool condition-variable handoffs. Its NVMe non-sync
flat profile has `_raw_spin_lock` at 3.44%, while individual C functions are
small. There is no single new C hot spot that justifies changing fairness or
removing length verification from this evidence.

Perf materially perturbs the workload: for example candidate NVMe non-sync
ran at 0.610 GiB/s under perf versus an untraced mean of 1.850 GiB/s. Profile
artifacts were written on the host filesystem, also on the NVMe. Do not turn
profiled throughput, CPU sample percentages or cycle counts into untraced
latency claims.

## Separate wall-clock syscall traces

Eight new 5-second `strace -f -ttt -T` captures, after all perf workloads.
The complete traces contain setup, but the summaries here admit only syscalls
that both start and finish inside the benchmark's begin/end markers. Resumed
syscalls are reconstructed. All eight traces parse without missing completions;
**each timed writev count equals its benchmark operation count exactly**.
Twenty-four futex calls cross a phase boundary in each capture and are excluded.
Concurrent syscall wall durations may sum to more than elapsed wall time.

| Device / O_SYNC | Current / candidate writev calls | Mean writev wall time µs, current → candidate | Candidate fstat mean µs |
|---|---:|---:|---:|
| SATA / off | 17,938 / 18,079 | 320.6 → 281.9 | 39.0 |
| SATA / on | 15,223 / 15,528 | 632.5 → 462.8 | 35.3 |
| NVMe / off | 18,625 / 19,239 | 217.9 → 102.3 | 41.4 |
| NVMe / on | 3,043 / 2,832 | 6246.4 → 6753.9 | 7.7 |

The candidate adds one `fstat` per completed write. Neither version issues
`ftruncate`, `fsync` or `fdatasync` in these timed fixed-size direct-overwrite
phases; current truncation is the `O_TRUNC` open flag. Thus the previous
candidate's redundant-barrier bug is absent. NVMe O_SYNC remains slow within
`writev`: about 6.25 → 6.75 ms in this separate diagnostic. Candidate `fstat`
is only 7.7 µs there, so its added syscall alone cannot explain the throughput
regression. The trace is consistent with a filesystem/storage wait difference,
but does not isolate firmware cache behavior, device flushes or block-layer
causes. Ptrace overhead makes these syscall means unsuitable as native latency.

## Decision and retained evidence

The measurement fixes are committed. Keep the direct storage candidate
experimental: there is evidence of reduced XFS CPU cost and improved tails,
but no universal throughput win. A deployment decision needs the unresolved
NVMe synchronous regression and long-tail variability investigated separately.
No daemon storage implementation or cluster deployment changed in this task.

Small raw benchmark outputs, pooled summaries, perf reports, syscall summaries,
source patch, scripts and validation notes are retained in this directory.
Full `perf.data`, complete straces and matching executables are downloaded to
`logs/efs-bench-latency-20261007` (ignored by Git); they also remain on nuc at
`/tmp/efs-nuc-latency-20261007`. `manifest.json` records binary SHA256s,
configuration, ordering and the one excluded/replacement measurement.
Earlier short-run evidence is retained with an explicit measurement caveat.
