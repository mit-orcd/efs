# W90 focused long comparison — October 8, 2026

Eight cases passed in 27 minutes 35 seconds (14:35:18–15:02:53 UTC).
Four alternating repeats per layout, 180 measured seconds each, 64 KiB payload,
1 GiB containers, four blocking workers, 2 GiB data capacity per worker,
95% occupancy, direct I/O, batch-32 persistence, 50/50 read/replacement mix,
seed 42, 90% of accesses to a 10% hot set. Two roots share one XFS NVMe.

| Metric | Files | Punched containers |
|---|---:|---:|
| Median ops/s | 12,157 | 22,123 |
| Sample ops/s range | 11,917–12,241 | 21,846–22,209 |
| Median read p99 | 0.316 ms | 0.315 ms |
| Median write p99 | 6.143 ms | 2.591 ms |
| Worst observed write | 945.809 ms | 532.833 ms |
| Median measured worker CPU/op | 62.94 µs | 49.13 µs |
| Median final allocated bytes | 8,160,313,344 | 8,162,666,496 |

Punch delivered 82% higher median throughput, 58% lower median write p99 and
22% less measured worker CPU per operation. Each repeat's punch/files throughput
ratio was 1.81–1.83, with non-overlapping observed throughput ranges. Final allocated
space was essentially equal (punch about 2.24 MiB higher). Both layouts still had
large rare write stalls: inspect max latency, not just p99.

24,632,965 total operations completed. All exit codes and error counts were zero;
every final live object was verified, both layouts ended with zero dead-record
backlog, and target operation totals reconcile with process totals. Punch's peak
pending retirement count was bounded to 32 per worker. Scratch directories are
empty. Source archive hashes match the manifest and committed source files.

This supports choosing punch for **this simulator workload on this NVMe**.
It does not establish crash recovery, production efsd improvement, multi-device
scaling, independent create/delete lifetimes or superiority across other sizes,
occupancies, persistence policies and concurrency. Timed runs consume different
lengths of the same seeded generator, not identical-length operation traces.
Worker-local retirement also does not model shared-range locking. No host cache
drops, mount changes or production service changes were made.

[Detailed comparison](COMPARISON.md), [raw results](results.json), commands,
stdout/stderr, device metadata, source archive and launch script are retained here.
Preserved executable and remote source remain under
`xorinox_efs:/data1/efs/bench/w90-focused-20261008` (source is in the frozen
`/data1/efs/bench/w90-long-20261008T143207Z/src` directory).
The earlier broad run was deliberately interrupted and is excluded from these
results. No additional long performance run is needed to establish the observed
repeatability of this focused comparison.
