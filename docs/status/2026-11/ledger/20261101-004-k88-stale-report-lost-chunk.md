# K88 stale REPORT

[K88 stale REPORT](../k88-stale-report-lost-chunk-20261101.md): **K88 FIXED, blocks alpha: no** (Task CK, Nov 1, intel1): `efs-bench`'s slot-less full-image records kept `base=0` after a background REPORT committed the chunk, the final REPORT went STALE and `report-stuck` dropped it; the committed image is now the CAS base and `efs-bench` fails on a recorded writeback error; 0 of 2960 loaded runs after (12 of 3360 before), gate 0 FAIL, TSan `data_commit` and `d4_progress` PASS; acceptance owed: `test_bench_published` in F1 on the candidate.
