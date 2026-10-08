# W95 indexed publication comparison

AMD single host, buffered I/O, 512 objects per sample, three balanced samples per configuration. QD4 uses two directories on the same filesystem. All operations include the requested durable publication barriers; every sample verified all objects. No profiler or strace ran during timing. This is finite creation, not sustained churn or hardware power-loss proof.

| QD | Cohort | Model | Mean ops/s (range) | Mean p99 µs |
|---|---|---|---|---|
| 1 | 1 | files-fragment | 3048 (2874–3200) | 476 |
| 1 | 1 | files-tiny | 3601 (3572–3634) | 354 |
| 1 | 1 | indexed-tiny | 842 (812–863) | 1602 |
| 1 | 32 | files-fragment | 3069 (3006–3120) | 12564 |
| 1 | 32 | files-tiny | 3864 (3775–3960) | 9599 |
| 1 | 32 | indexed-tiny | 30733 (30222–31201) | 1370 |
| 4 | 1 | files-fragment | 6807 (6748–6868) | 886 |
| 4 | 1 | files-tiny | 8235 (7842–8509) | 827 |
| 4 | 1 | indexed-tiny | 2518 (2336–2667) | 2076 |
| 4 | 32 | files-fragment | 6605 (6575–6628) | 20095 |
| 4 | 32 | files-tiny | 8150 (8110–8195) | 16253 |
| 4 | 32 | indexed-tiny | 73121 (70684–77764) | 1959 |

Individual indexed publications are slower: full manifest replacement and extra persistence barriers outweigh packing. Cohorts of 32 substantially improve this finite-create case. These results do not select a production format. Full snapshots introduce write amplification and must be compared under sustained conditional overwrite/delete/recreate and mixed-size workloads before any production proposal.

Raw commands, counters and samples are in `results.json`; individual logs retain BENCH_OK verification and allocation results.
