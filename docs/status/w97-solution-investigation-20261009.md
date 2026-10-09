# W97 solution investigation — 2026-10-09

Branch: **devel-persistence-pressure**, created from da653c33. NUC resumed
successfully. Existing data retained; TCP/direct I/O and virtual commands
disabled throughout. Final installed source **a770290a**, HELLO build
**a770290a-pressure**; diagnostics are disabled after measurement.

## Implemented fix: independent Raft groups keep their priority messages

The production outbox_pri_replace helper selected the first matching message
**type** in a per-peer queue without checking its **Raft group**. A group-2
reply could replace a queued group-0 reply, and heartbeats had the same issue.
These groups have independent log/commit/read progress. A deterministic test
using the actual compiled helper fails on the original code because the other
group's ACK disappears. It passes with the group-isolation fix.

Commit a770290a limits same-type replacement to the same group. It retains
existing same-group coalescing, queue/byte bounds, caller ownership on capacity
refusal, and emergency vote eviction of a heartbeat. No wire, durability,
read-authority or store-format change is made. tests/test_host_outbox.py is
part of the standard test recipe; host pressure policy and Raft/store tests
pass with diagnostics off and on.

This is a reproduced correctness defect. Its exact frequency and share of the
live slowdown were not measured; the experiment does not establish it as the
single cause of receiver pressure.

## Rejected fix: retaining a suffix after an older reply

The first candidate (c5ff653e) retained outstanding-window tracking when an
older successful reply covered only part of it. A synchronous-network
regression failed twice before the change and passed afterward, including
lost-reply timeout, rejection retry and fresh-read progress. Existing
Raft/store suites also passed.

The live result did not meet acceptance: late throughput regressed and
concurrent_appends exceeded the 30-second budget in the full core POSIX suite.
Candidate: **211/212**, peer **64/64** in 234.5 seconds. Restoring the baseline
produced **2/2 append-only passes in 4.6 seconds** and **212/212 full core
passes in 69.7 seconds** with the same jobs/timeout flags. The candidate was
reverted in **6005fc5d** before the group-isolation build. These controls are
sequential; the precise regression mechanism is not isolated. Preserve actual
asynchronous sender/coalescing/probe and wall-driven tick behavior in future
models rather than inferring host acceptance from synchronous fixtures.

A ten-second strace of the rejected peer append gate shows six-byte writes
around 0.1–1.4 seconds. A separate 49 Hz user-cycle profile collected only
36 samples, with no reported sample loss; it is too sparse for a dominant-hot-
path conclusion. It shows LSM/transaction-resolution work occurs, not that
CPU or hardware is the established limiter. These attaches occurred after
timed measurements and affected only the rejected functional-gate phase.

## Clean five-minute observations

All timed workloads use eight workers, seed 970001, fixed readable payload
bytes and original tiny-file size/depth/count distributions. Fresh output
paths and cooperative SIGUSR1 stop preserve current writes. No build or
functional suite overlaps the clean runs. The earlier exploratory run
included compilation and is retained separately, not used as a throughput
baseline. The group trial's higher target prevents early completion; all
measured prefixes remain below the earlier 500,000-file target.

| Run | First 100 s | Middle 100 s | Last 100 s | n3 inbox drops |
|---|---:|---:|---:|---:|
| Baseline 4b149e88 | 112.18 files/s | 60.52 | 48.44 | 91,932 |
| Rejected partial-reply c5ff653e | 122.24 | 72.20 | 38.73 | 79,300 |
| Group isolation a770290a | 139.63 | 43.70 | 70.05 | 129,365 |

Completed files including final drain: **22,407 / 23,592 / 25,639**.
The partial-reply candidate reduces leader data-send counters from 235,836
to 148,748 (about 37%), but fails sustained-performance/live-gate acceptance.
Group isolation records 260,462 accepted core data-send callbacks; its n3
drops comprise 40,458 data requests and 88,907 empty requests. These send
counters count acceptance into the host sender path, not actual wire
transmissions; queued replacement and transport outcomes remain separate. Do not claim pressure is eliminated or
hide the worse middle interval and higher drop count.

| Run | n3 mean sync sections, early/middle/late | n4 mean sync sections |
|---|---|---|
| Baseline | 3.347 / 7.489 / 6.876 ms | 2.880 / 6.545 / 6.157 ms |
| Partial reply | 3.612 / 7.878 / 6.426 ms | 3.148 / 6.802 / 6.087 ms |
| Group isolation | 4.034 / 6.688 / 7.576 ms | 3.511 / 6.182 / 6.643 ms |

Device-wide NVMe write/flush completion times rise within all three runs.
For group isolation, writes average **2.35 / 4.70 / 5.02 ms**, flushes
**2.05 / 3.77 / 4.06 ms**, busy time **85.8 / 91.8 / 88.2 percent**.
Counters include all device users and queue time. No readable NVMe hwmon
sensor was exposed by the unprivileged probe. Do not infer thermal, firmware
or cache behavior from these timings.

## Validation and remaining work

Group-isolation final gates: **212/212 core POSIX**, **64/64 peer** in
32.5 seconds, clean cluster/mount restart, and **128 cold sample payloads**
across all four retained test trees. Both mounts match independently predicted
seeded lengths and content. This is sample verification, not a full tree scan
or power-loss test. Final six process environments confirm diagnostics and
virtual commands off, TCP and direct I/O. No active loader is retained.

**W97 remains open.** Group isolation is implemented and functionally tested;
a consistent sustained-performance improvement is not established. These are
single sequential runs on a growing namespace with differing prefixes,
scheduling, leaders and device/cache state, not randomized identical-state
repeats. Time-aligned windows also cover different completed portions of the
seeded workload. Histogram quantiles are bin upper bounds, dump boundaries
are approximate and observer overhead is uncalibrated. No hardware ceiling
or full W64/W73 acceptance is claimed.

Next investigate bounded handling of extending small-entry windows and empty
commit/read probes using the real asynchronous host queues; measure per-group
physical sends, replacement/eviction, receipt/consensus ACK and queue residence.
Separate Raft, KV WAL and payload persistence plus competing I/O and scheduler
waits. Test adaptive flow control/coalescing with actual read/probe/timeout
semantics, fixed-work as well as fixed-time repeats, idle/mixed p99 and full
fault/recovery gates. Never enlarge queues or remove durability as a substitute
for that evidence.

[Clean baseline](../../results/measure/20261009-w97-clean-baseline/README.md),
[rejected candidate](../../results/measure/20261009-w97-partial-reply/README.md),
[group-isolation evidence](../../results/measure/20261009-w97-group-isolation/README.md).
