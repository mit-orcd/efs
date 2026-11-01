# W97 latest merged NUC slowdown and NVMe observation — Oct 10

**Tracking update, Oct 11:** The NUC filesystem/device persistence-latency
investigation in this report is now [W111](w111-nuc-persistence-investigation-20261011.md).
Dated W97 labels and measurements below are retained as historical evidence.
The [AMD overload/admission gate](amd-overload-admission-20261011.md) is closed
and does not depend on this NUC investigation.

Status: this merge/deploy/continuous observation is complete; W97 remains open. User requests merging all current commits,
deploying on NUC and observing whether the time-dependent decline persists,
including the NVMe cache hypothesis. No production persistence/SSD-setting change
is part of this experiment.

All 16 tips captured at this new boundary are ancestors of merged source
`6b8f05acb484b52284a7ffdd6c1da7b0476f35f6`; build is
`6b8f05acb484-w97`, branch `devel-persistence-pressure`. Other branch pointers
and checkouts are preserved. Changes include retained CREATE routing, durable
command policy and virtual NFS FORGET recovery. Full merged native tests pass.

## Pre-deployment gate evidence

The first private live invocation refuses before executing because fresh test
mount directories are absent; create empty dedicated directories and retain the
preflight failure. The second private invocation passes sync-write/APPEND,
dirty-stop, mutation shutdown, WAL and physical-GC checks, then POSIX is
211/212: concurrent_appends exceeds the unchanged 30-second case budget.
Its fixture `/data1/efs/evidence/gc-direct-vt7vrt5g` and raw logs remain preserved.

The append log has all 200 eight-byte reservations for inode 4212: first
11:55:41.389 UTC, last 11:56:11.163 UTC (29.774 seconds). Last close completes
11:56:11.233. The neighboring two-process case has all 120 reservations and
passes. No opid-admission BUSY diagnostic occurs. Reservation completion does
not itself replace the missing final payload/multiset oracle. The unchanged full repeat passes POSIX 212/212 (67.1 seconds), peer 64/64 and
the remaining durability/GC/cold checks. The original timeout is retained; it
is not relabeled. Deployment then safely drains both service mounts / all four
daemons and installs the merged build. A copied measurement preflight still
expects the old build ID and refuses before any loader starts. Its empty cell
and failure are retained; the corrected preflight expects the verified new ID.
A second measurement startup assumes PSI files that this kernel does not
expose; it safely drains after 11 files. Retain that cell and its tree. PSI
becomes explicitly unavailable rather than zero, and the final run uses fresh
`tiny-w97-latest-continuous-v2-20261010`. The measurement then starts without
repeating deployment or replacing stores. Controllers are frozen through
`742a6acb`; executable source remains `6b8f05acb484` throughout.

## Continuous experiment

Native/private gates passed on the unchanged repeat. Both service mounts and
all four daemons were safely drained; stores and rollback binaries were
preserved before installation of the exact merged build.
Use defaults: FLOW=1, 10 ms, direct IO/TCP, commands and optional overlap/no-op/
group-drain/early-retirement off; diagnostics on only for measurement.

One fresh balanced eight-worker tree, 1–512-byte actual payloads, bounded
per-worker work and explicit volume/RAM headroom. Twenty minutes writing,
ten-minute pause, twenty-minute resume with the same workers/directories and
no restart. Pause finishes in-flight operations and requires zero active
workers plus unchanged completed counts. Keep precise drain and resume times,
five-minute rates per group, durable cohort/barrier/marker and pressure counters,
NVMe/SATA host request/flush/write deltas, RSS/PSI and immutable read probes.
At end, cooperatively drain, require replica leader-cut catch-up, restore normal
diagnostics-off defaults and verify first/last exact bytes through both cold
mounts and active binary identities.

## NVMe hypothesis and access limits

The device is Samsung 970 EVO 1 TB firmware 2B2QEXE7. n3/n4 use XFS/LVM on that
NVMe, shared with root/home/swap; n1/n2 use XFS on SATA. Linux reports NVMe
write-back and SATA write-through. These settings are observed, not changed.
Volatile write-cache flush latency, NAND TurboWrite buffering and Linux page
cache are different mechanisms.

Samsung specifies 42 GB maximum TurboWrite buffering for this capacity;
its bulk cache/post-cache throughput specifications are not EFS fsync latency
or actual cache-occupancy telemetry.
[Samsung model-family cache specification](https://semiconductor.samsung.com/news-events/news/samsung-electronics-sets-new-performance-standards-for-nvme-ssds-with-970-pro-and-evo/),
[970 EVO datasheet](https://image.semiconductor.samsung.com/resources/data-sheet/Samsung-NVMe-SSD-970-EVO-Data-Sheet_Rev.1.0.pdf).

NVMe SMART/id-ctrl require root; the efs account is denied, and no SSD hwmon
sensor is exposed. A read-only optional root collector is provided and requested;
without its data, no SSD temperature/throttling conclusion is supported. Even
with SMART, cache occupancy may remain unavailable. A rebound after the drained
pause can also reflect replication backlog, GC or compaction settling. Device
write totals include other users and are not NAND write amplification.

Controls/provenance: `results/measure/20261010-w97-latest-nvme/` locally,
`/data1/efs/w97-latest-20261010` on NUC; candidate build is isolated at
`/data1/efs/evidence/w97-latest-build`. Native/live gates and service deployment are complete. Continuous observation
completed on this exact merged build. Final observation, restoration, cold
payload checks and active executable/default-option verification all pass.


## Results: slowdown repeats after rest

Eight balanced workers complete **138,881 files with zero loader errors**.
The initial 20-minute writing phase averages **58.75 files/s**; the resumed
20-minute phase averages **56.88 files/s**. These averages exclude the pause.
The pause drains in 5.000 seconds; completed counts then remain exactly 70,567
for 595.001 seconds. Same workers/directories, client/server processes and
configuration survive the pause. No restart or new tree explains the rebound.
A few completions in the nominal pause window are the measured drain tail,
not writes during its asserted zero-active-worker interval.

| Measured window | Files/s | NVMe host writes (GiB) | Mean NVMe flush (ms) |
| --- | ---: | ---: | ---: |
| Initial first minute | 152.47 | 1.70 | 1.33 |
| Initial second minute | 72.42 | 0.78 | 2.91 |
| Initial final minute | 52.85 | 0.63 | 3.65 |
| Resume first minute | 159.35 | 1.73 | 1.34 |
| Resume second minute | 71.45 | 0.74 | 2.88 |
| Resume final minute | 48.05 | 0.55 | 3.66 |

Five-minute creation rates are **76.30 / 52.73 / 52.76 / 53.23**, then
**77.92 / 51.55 / 49.61 / 48.45** after rest. NVMe averages about 92% IO busy
in sustained writing windows, with approximately 388–396 flush requests/s;
this is kernel device accounting, not a proof of maximum hardware throughput.
The nominal pause averages 0.523 MiB/s NVMe host writes and 1.009 ms flushes,
including the drain tail. Read probes, GC and monitoring continue during rest.
Both writing phases combined record 26.49 GiB NVMe host writes. Payload files
are only 1–512 bytes: much of the IO belongs to durable metadata/replication,
fragment persistence and background work rather than user payload bytes.

## Persistence and replication pressure remain

In the final five minutes before pause, n3/n4 durable Raft cohorts average
**4.20 / 1.07 entries**, while observed Raft syncs average **7.14 / 5.96 ms**.
n1/n2 syncs in that window average about **0.10 ms** on SATA. n3 pump-cycle
means are 5.60 ms in KV release and 3.42 ms in applied-marker persistence;
these means include cycles with no work, and must not be added to derive a
per-file latency. n3 mean inbox age is 45.07 ms versus 9.48 ms on n4.
Across the sampled windows, n3 records **75,109 inbox refusals**, n4 records
10, and n1/n2 record zero. No sampled sync-error increments occur. Queue
refusals are retained as pressure/retry evidence, not described as lost files.
All hosted replicas subsequently cover the captured leader commit cuts.
The sampled local commit-minus-applied maximum is zero; it does not measure
leader-to-follower index lag or exclude events between five-second samples.

Whole-run create/open p50/p95/p99 are 72.85/280.27/375.84 ms; CLOSE is
28.22/82.14/108.29 ms; total is 100.35/350.27/450.28 ms. These combine both
writing phases, not separate before/after distributions. Immutable read-probe
p99 is 23.74 ms before, 421.83 ms during mixed observation (including rest),
and 24.82 ms after. These OPEN/read/CLOSE probes are not pure disk-read latency.
Maximum sampled server RSS is n1 1.52 GiB, n2 2.46 GiB, n3 2.44 GiB and
n4 0.95 GiB. Kernel PSI files are unavailable, explicitly recorded as null.

## What this says about NVMe cache

A ten-minute creation pause restores roughly 159 files/s without a restart,
but the higher rate collapses again within the next minute. NVMe flush latency
rises with that decline. This establishes a repeatable load-dependent
persistence/pressure pattern on the latest merged code; it does **not** isolate
its cause. TurboWrite cache exhaustion, thermal throttling, filesystem/log
work, drive firmware behavior and replication feedback are still hypotheses.

Only about 2.48 GiB of host writes occur in the first two minutes of each phase,
compared with the vendor's **maximum** 42 GB TurboWrite region. That does not
prove a full/empty buffer: starting occupancy, available dynamic buffering,
internal NAND writes and unrelated activity are unobserved. Volatile cache
flush costs also differ from NAND buffer capacity. No root collector data
arrived, so SSD temperature, thermal counters and current power/APST features
remain unavailable. No cache or power settings were changed. It is premature
to claim a hardware ceiling or to close W97.

The next discriminating measurement is root NVMe health telemetry alongside
a matched sustained durable-write probe with EFS stopped, followed by the same
probe under EFS load. Separate preallocated overwrite and growing XFS WAL
cases to distinguish filesystem allocation/log behavior from the drive's
flush service time. Preserve durability and actual workload distributions;
do not infer performance from peak sequential bandwidth or disable flushes.

## Final service and acceptance

The unchanged repeat passes full POSIX **212/212**, peer **64/64**, synchronous
write/APPEND boundary and replay cases, dirty-stop/shutdown recovery, WAL-tail
repair and physical GC/cold gates. The earlier 211/212 append timeout remains
a material intermittent performance failure; a passing repeat does not erase
it. Both private fixtures and their original logs/oracles are retained.

After cooperative loader drain and replica catch-up, safely restart with
FLOW=1, 10 ms markers, optional overlap/no-op/group-drain/early retirement off,
commands off, diagnostics off, TCP/direct IO. Retained-store startup requires
bounded readiness retries; processes/stores are retained, never force-wiped.
Both mounts return. **128 independent exact first/last payload checks** pass
through both cold mounts; this samples bytes, not every file in the tree.
All four active efsd and both efs-fuse executable hashes match installed binaries,
and final environment/transport/default flags are asserted. All five installed
binary versions/hashes and frozen source/controller hashes are retained.
No owned loader/perf profiler/strace remains; the root telemetry stop marker exists.

Raw gates, failed attempts, host snapshots, perf tracepoint acknowledgements,
node counter windows, catch-up, cold verification and final identity are in
`results/measure/20261010-w97-latest-nvme/`. `observation-summary.py` reproduces
minute/phase rates, verifies controller hashes and zero final exit records,
and asserts unchanged paused counts. W93 remains closed by its separate
acceptance; this latest evidence keeps W97 open.

**Raw evidence note (2026-10-23):** the bulk data in `gc-direct-vt7vrt5g`, `w97-latest-build` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
