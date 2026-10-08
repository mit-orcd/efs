# W90 matched layout simulator — October 8, 2026

Implemented on `devel-bench`; source identity is preserved in each manifest and
`sources.zip`. Remote source, preserved binaries and full results:
`xorinox_efs:/data1/efs/bench/w90-comparison-20261008`.

- [Functional comparison](functional/COMPARISON.md): 12 passing cases,
  files/punch/compact, 80/95% occupancy, two workers and two roots, 64 KiB,
  direct I/O, batch-32 barriers, 90% of access to a 10% hot set. 2,000 measured
  operations per worker, alternating order over two repeats. All fixed-step
  cross-layout trace/count/live-set checks passed, all final objects verified.
  These use 1 MiB containers and 8 MiB capacity per worker to exercise cleaning.
- [1 GiB containers](one-gib/COMPARISON.md): four passing timed files/punch cases,
  80% occupancy, two workers, 2 GiB capacity per worker, 1 GiB containers, same
  payload/locality/barriers. Two alternating repeats of ten measured seconds.
  Timed workloads consume different-length prefixes; full verification passes.

The two roots share `/data1` on one XFS NVMe. This validates target dispatch and
accounting, not multi-device scaling. No production service, mount option or
cache state was changed. Test scratch directories are empty after completion.

The short 1 GiB samples report median 6,548 ops/s for files and 8,145 for punch,
with median write p99 4.015 ms and 2.207 ms respectively. Two ten-second samples
are functional/preliminary evidence, not a stable winner claim or endurance test.
Raw metrics include allocation, cleanup backlog and final verification counts.

Validation logs accompany this ledger: 11 storage test methods (including 54
layout/occupancy/I/O/persistence combinations), eight runner tests including
mismatch rejection and source-archive creation, CLI checks, warning-free Linux
build and the documentation gate. No production code was modified.
