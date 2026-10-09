# W66 separate profiling runs — Mac efs1

Candidate 2b9da49f, four private buffered/TCP daemons on one Linux VM.
Installed daemon/stores unchanged; commands disabled. No production code change.

Three distinct workloads: cpu-clock frequency sampling (100 samples/cell),
syscall trace (50 samples/cell), explicit-period cpu-clock (100 samples/cell).
No perf/strace overlap. Each has eight GETATTR cells and independent mutation
workers. All workloads completed. Instrumented latency is diagnostic only.

Frequency perf captured only 8 samples: unusable for hot-path conclusions.
Explicit period `-e cpu-clock -c 1000000 -g` captured 1,032 samples, no lost
samples. Generated flat, by-thread and callers reports with the requested
thresholds. Highest daemon individual symbol is kernel __wake_up_sync_key
(3.20% of all samples); no dominant userspace CPU function established.
CLI startup and Python are included in denominator; do not read these as
daemon-only percentages or off-CPU time. Raw perf.data stays on efs1 under
/data1/efs-w66-fixtures/profile-20261009; report binaries remain available there.

Successful completed fsync/fdatasync calls in separate strace -ff -ttt -T -y:

| Path class | Calls | p50 ms | p95 ms | max ms |
|---|---:|---:|---:|---:|
| Raft fdatasync | 1505 | 9.471 | 15.358 | 35.194 |
| applied-index fdatasync | 779 | 8.036 | 12.772 | 28.013 |
| metadata temp fsync | 193 | 16.814 | 28.610 | 44.043 |

Aggregates overlap across daemons/threads; summed durations are not wall time.
Trace overhead applies; startup/shutdown included. JSON uses nearest-rank
percentiles over successful completed calls; unfinished/resumed calls excluded.
Path classification: /applied. first; /mdraft/ second; /meta/ plus .tmp. third.

Independent perf-only observation logs confirm shared-pump stalls: node4's
last two observations have drain maxima 17,344 and 21,914 us, persistence
maxima 29,692 and 31,744 us; apply maxima only 333 and 126 us. This supports
durability blocking pump progress, not a proven explanation for every POSIX
timeout. Existing baseline reproduces those failures.

No fsync removed, ACK advanced, replay boundary changed or checkpoint interval
relaxed. Such changes need durability/replay proof and overlap agent4's D29/D30
work. W66 routing/topology scaling remains open. Next isolate actual FUSE
timeout request timelines and evaluate a bounded durability/pump separation
against fault/restart tests in coordination with that owned package.

raw-evidence.tar.gz contains syscall files, phase logs and raw workload JSON.
Owned processes stopped and reservation released; installed efs1 PID18743
remained unchanged. W55/W58 untouched.
