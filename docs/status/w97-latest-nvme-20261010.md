# W97 latest merged NUC slowdown and NVMe observation — Oct 10

Status: investigation in progress. User requests merging all current commits,
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
Its fixture `/data1/efs/gc-direct-vt7vrt5g` and raw logs remain preserved.

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

After native/private gates pass, safely drain both service mounts and all four
daemons, preserve stores/rollback binaries, install the exact merged build.
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
`/data1/efs/w97-latest-build`. Native/live gates and service deployment are complete. Continuous observation
is active on this exact merged build; final observation and restoration remain
pending.
