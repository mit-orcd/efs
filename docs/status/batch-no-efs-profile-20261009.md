# W93: batching and receiver pressure with .efs disabled — 2026-10-09

NUC source commit **c227bcbd**, build ID **c227bcbd-batchobs**, TCP/direct I/O.
All four servers have EFS_COMMANDS_SERVER=0 and both mounts EFS_COMMANDS=0;
the synthetic .efs directory is absent on both mounts. Existing data retained.
Capture: **13:04:59–13:09:59 UTC**, eight original tiny-file workers, fresh
tiny-batch-no-efs-20261009 directory. Cooperative stop completes at 300.565 s,
exit 0, with 24,545 files in 58 populated directories. The last progress
sample precedes the drain, so interval file counts exclude final in-flight work.

**The decline persists without catalog work:** three 100-second windows yield
**118.40 → 81.69 → 42.37 files/s**. This rules out enabled .efs refresh as a
necessary condition for this run's decline. It does not exclude additional
catalog overhead when enabled, nor establish the historical decline's exact
cause. Restart, leader placement, random paths and uncalibrated observer
overhead prevent a matched gain claim against earlier enabled runs.

| Node | Entries per WAL sync | Mean sync section | Nonempty inbox messages | Dropped frames |
|---|---:|---:|---:|---:|
| n1 | 1.001 | 0.101 ms | 1.07 | 0 |
| n2 | 1.165 | 0.113 ms | 1.11 | 0 |
| n3 | 2.206 | 5.226 ms | 31.69 | 112,982 |
| n4 | 1.474 | 4.570 ms | 4.45 | 0 |

n3 mean sync sections rise **3.211 → 8.443 → 6.634 ms**; n4
**2.870 → 7.272 → 6.100 ms**. n3 mean inbox depth is
**30.04 → 252.58 → 14.94**; dropped frames per window are
**55,103 / 42,638 / 15,241**. Thus queue/drop pressure and persistence
latency coexist. Falling drops later do not explain the slowdown by themselves.
The measurement does not isolate device service time from scheduling,
filesystem, concurrent I/O or replication retry feedback.

Every measured nonempty-request tail mismatch is **behind** the local tail:
n2 16,461, n3 84,450, n4 60,013. Ahead and equal-index term mismatch counts
are zero. These are eligibility checks (including rechecks), not unique
logical operations. Behind-tail traffic can overlap valid replication and
must still go through Raft's normal matching/ACK path; do not discard it
merely because it is ineligible for strict append batching.

Active cross-request batches contain one request each. Their exact
ineligible boundary reasons are:

| Node | Behind-tail boundary | Empty request boundary | Total ineligible boundaries |
|---|---:|---:|---:|
| n2 | 2,480 | 234 | 2,714 |
| n3 | 1,061 | 215 | 1,276 |
| n4 | 3,421 | 354 | 3,775 |

These boundary counters count once before finishing a batch, unlike general
eligibility counters. n3 host segments end on group switches 42,261 times
(83.1%); count cap 1,773 (3.5%), end-of-drain 6,802. No byte boundary,
sync error or tail-ahead/term mismatch is observed. One request may itself
contain multiple entries; intra-message batching remains separate.

Device-wide /proc/diskstats intervals corroborate persistence pressure on
**nvme0n1** (n3/n4 backing device): write completion time
**2.22 → 4.51 → 3.80 ms**, flush completion time
**1.76 → 4.16 → 3.73 ms**, busy time **87.9% / 86.0% / 92.5%**.
SATA sda write completion stays near 0.14–0.17 ms. These counters include
all device users and queue time; NVMe busy percentage is not a saturation
proof. They strengthen the persistence-latency finding without isolating
firmware/cache/thermal behavior or proving no software improvement remains.
Raw derived intervals: disk-intervals.json.

**Next W93 work:** trace group-specific retransmission/in-flight handling and
the relationship between sync blocking, inbox overflow and behind-tail replay.
Investigate safe scheduling/coalescing only with Raft matching, durability,
ACK/read-order and fault gates intact. Larger caps are not justified by these
observations alone. Separate device/filesystem latency analysis is still
needed before any hardware-ceiling claim.

Raft and durable-store tests pass with observation disabled and enabled.
The startup script's 30-second node-1 replay timeout required an idempotent
retry; no formatting or force-discard occurred. After capture the cluster is
restarted with diagnostics off and .efs still disabled. See final-state
evidence for verified settings and peer reads; this is not a new full POSIX
or crash/fault gate acceptance.

[Raw evidence and method](../../results/measure/20261009-batch-no-efs/README.md).
