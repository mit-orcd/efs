# W92 clean Xorinox acceptance — Oct 9, 2026

User explicitly authorized wiping all three dedicated storage volumes. Historical
wrong-placement metadata was erased, not repaired; its provenance remains unknown.
Production binaries: 47e7b886, devel-find-stats, files/direct-I/O/TCP, runtime user efs.
Ordinary mounts opt out of commands; the dedicated acceptance mount opted in and
was drained/removed afterward. Normal services and mounts were restored.

Independent oracle: 1500 files × 32 bytes, 16 directories, four creation threads;
creation took 5.376 seconds. Exact .efs/find paths and .efs/stats counts/bytes pass.
Deleted 75, cross-directory renamed 75, appended 17 bytes to 15: final 1425 files,
16 descendant directories, 45855 logical bytes. Sampled payload SHA256 checks pass.
The same model passes after full daemon restart and a fresh FUSE mount. Evidence:
[oracle](oracle.log), [fixture](clean_catalog_oracle.py), [runtime](runtime.log).
The restart rejected an outdated checkpoint (rc=-14) and rebuilt all 4096 partitions
in 137 ms; this proves rebuild correctness, not checkpoint reuse in this run.
Root daemon rollups include the test directory itself (17 directories); scoped
command rollups correctly report 16 descendants.

Post-restart node-1 sample: RSS 28824 KiB, cache allocated usage 5752 KiB. These
single samples are not a scaling/RSS ceiling measurement. Publication samples at
1500 files were 140 ms; final mutation refresh 146 ms. No engine performance claim.

[Quota unit output](quota-tests.log): default and 12000-file RAM/disk differential
checks pass; 1 MiB limit refuses before allocation and 3 MiB refuses during raw
partition ingestion. Complete native/code gates pass in the private build; the
architecture link checker was excluded because historical result archives were
not copied there. Unit/private live coverage is detailed in the existing W92 ledger.

Still open: incremental derived rollups, aggregate cache-space control, resumable
builds, larger measured scale/fault/platform acceptance and synthetic 2^32 index gate.
No pushes; no acceptance of billions of real files.
