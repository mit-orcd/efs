# NUC existing durability batching measurement — 2026-10-09

Production build `800122c5e86d`, unchanged TCP/direct I/O configuration, was
observed for 300 seconds with a dedicated eight-worker tiny-file generator.
Twelve bounded WAL traces (three six-second windows per daemon) reconstruct
entry writes associated with sync starts; raw data and reproducible analysis
are in the [evidence report](../../results/measure/20261009-durability-batching/README.md).
No production code, deployment or durability setting changed.

W93 batching is present but modest for this workload: n2/n3/n4 average
1.45/1.34/1.39 entries per WAL sync. n3/n4 have 76%/73% single-entry barriers,
and mean sync elapsed time 7.3/8.0 ms, versus approximately 0.1 ms on the
SATA-backed peers. Approximate WAL-call reduction against one sync per entry
is 25–28% on n3/n4; this is not measured filesystem throughput gain or physical
flush reduction. Internal byte-cover watermarks are inferred at syscall
boundaries, and ptrace overhead is a limitation. Groups and foreground/background
traffic differ across nodes; entry counts are not file counts.

Untraced steady intervals remain slow (approximately 22–24 files/s), with NVMe
about 93% busy and flush await approximately 4 ms. This supports persistence
latency as the limiting path, not a hardware-ceiling assertion or an identified
thermal/cache/firmware cause. Different cache and XFS log configurations are
recorded as comparison variables, not proven causes. Fragment persistence remains
a separate cost.

Next controlled acceptance should instrument per-watermark entries/bytes,
hold/waiter depth and follower flush reasons, then compare bounded coalescing
policies on identical fixtures with idle and mixed-load p99, bounded resources
and cold/fault durability gates. Existing batch limits alone are not shown to
be the constraint. Preserve W93's covered correctness results and W53's separate
close-tail investigation; this observational run does not close either gate.
