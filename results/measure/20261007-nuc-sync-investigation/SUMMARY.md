# nuc_efs I/O and synchronous-write comparison — Oct 7

Created benchmark directories requested by the user. Root1 /data1/efs/bench:
SATA MK0800GEYKE, XFS/noatime/logbsize256k. Root2 /data2/efs/bench resolves under
/home/efs/additional-work-dir: Samsung 970 EVO 1 TB NVMe through rl-home,
XFS/relatime/logbsize32k. Four live daemons retained; no deployment/restart or
mount-setting change. Private source/build in /tmp/efs-nuc-sync-study-20261007.

80 randomized untraced runs: five repeats × buffered/direct × sync on/off ×
each root × before/fair admission. QD16, 64 KiB fragments, 64 MiB overwrite
window, 1.5-second phases. Auto selected FOUR writers on nuc (8 CPUs), versus
12 in the xorinox study. Both binaries use the same committed buffered storage
fix and benchmark O_SYNC support. Before uses writer.c from b3c40474 parent;
fair uses reserved per-slot handoff. All 80 runs pass, errors zero. Sixteen
separate perf captures/reports and sixteen strace cases pass. Additional four
wall-clock strace cases pass. Nuc admission and actual-store tests pass.

| Sync | Mode | Storage | Before GiB/s | Fair GiB/s | p99 before/fair µs | Worst max before/fair ms |
|---|---|---|---:|---:|---:|---:|
| off | Buffered | SATA | 8.623 | 11.646 | 910 / 245 | 9.9 / 3.2 |
| off | Buffered | NVMe | 8.252 | 10.378 | 956 / 284 | 5.1 / 3.0 |
| off | Direct | SATA | 0.402 | 0.412 | 49887 / 4379 | 489.0 / 29.4 |
| off | Direct | NVMe | 1.970 | 1.959 | 7517 / 1586 | 106.4 / 86.5 |
| on | Buffered | SATA | 0.311 | 0.308 | 29017 / 5626 | 87.6 / 29.0 |
| on | Buffered | NVMe | 0.017 | 0.016 | 603014 / 104830 | 1032.3 / 121.8 |
| on | Direct | SATA | 0.301 | 0.307 | 30233 / 4457 | 190.0 / 26.8 |
| on | Direct | NVMe | 0.036 | 0.034 | 264455 / 74997 | 614.4 / 100.7 |

Fairness improves buffered cached throughput on this four-writer host, unlike
the approximately 5% overhead observed with twelve writers on xorinox. Results
cannot determine a universal default writer count. Cached rates are page-cache
performance, not physical-media bandwidth.

The SATA device delivers ~0.30 GiB/s for synchronous writes; the NVMe only
~0.02–0.04 GiB/s. Hardware/cache/firmware and filesystem layouts differ, so this
is evidence of a persistence-path gap, not a causal diagnosis. Wall-clock strace
(-f -c -w), current code with O_SYNC: SATA write averages 386 µs, direct writev
703 µs; NVMe write 5771 µs, direct writev 6044 µs. Buffered final fsync averages
7 µs SATA versus 1167 µs NVMe. Tracing perturbs scheduling; these are separate
diagnostics, not untraced acceptance latency. Default strace -c reports syscall
CPU time, so those earlier summaries must not be interpreted as elapsed latency.
Aggregate futex totals overlap writers' storage waits and do not establish that
futex causes the persistence gap. No relaxation of sync/durability performed.

O_SYNC applies to each buffered body/checksum write separately, with a final
length fence fsync; direct retains its existing single body/checksum writev.
This is deliberately conservative, not one persistence barrier per PUT. Short
phases are diagnostic; longer workloads larger than RAM and flush/latency/device
instrumentation are still required for sustained-media conclusions.
