# W93 NUC tracepoint rerun — 2026-10-09

After syscall tracepoint access was enabled, a fresh SSH session successfully
attached perf to the existing daemons. No cluster restart was needed. Unchanged
build `800122c5e86d` was recorded continuously for 300 seconds, 06:01:53–06:06:53
UTC, with 1,699,265 events and zero lost events reported. No strace, deployment,
production code or durability changes.

n3/n4 average 1.60/1.47 entries per WAL sync, median one, with 78%/69%
single-entry calls and mean elapsed 7.8/7.4 ms. Occasional n3 bursts reach 92
entries; short earlier traces understated that tail. These are verified-record,
syscall-boundary cohorts, not exact internal watermarks or committed-entry counts.
Recorder CPU is approximately 4.8% of one core. Creation rates are 33.2, 22.3
and 27.1 files/s across the three windows. NVMe flush await remains about 4 ms.
The run confirms persistence latency and modest typical coalescing without
proving a hardware ceiling or explaining the initial slowdown.

WAL command tags show several metadata stages per create/write/close and
background GC/reap traffic (about 14.5% on n2/n3). Do not equate entry or sync
counts with file counts. Next instrument internal cohort watermarks, hold/waiter
depth and batch flush reasons before policy A/B, preserving idle/mixed p99 and
cold/fault durability gates. Device/filesystem comparisons remain separate.

**Shutdown correction:** the previous background generator inherited SIGINT
ignored, so it continued scheduling after the stop request. Earlier draining
statements were incorrect. The rerun reused that loader rather than adding
another; it continues toward its bounded 50,000-file target without forced kill
or data loss. The archived collector now restores SIGINT for future launches.

[Raw tracepoints, reproducible analysis and limitations](../../results/measure/20261009-durability-tracepoints/README.md).
