# Tiny-file performance follow-up — W93–W96

**Current implementation (Oct 8):** The review below describes the pre-merge baseline. W93 batching/fault fixes, W94 atomic server leases, W95 tiny principles/durable timing, and W96 tail repair/reservation are now implemented and tested in focused portions. Client adoption, churn/recovery experiments and remaining performance acceptance are still open. [Current checkpoint](w93-w96-implementation-20261008.md).


Registered Oct 8, 2026 after seven NUC profiling/implementation rounds.
All four items are **open investigations/experiments**, not completed fixes.
The current experimental deployment is `c22754236b38-tiny-r7` on
`devel-find-stats`; source changes remain uncommitted in the isolated
`efs-find-stats` worktree. These documents do not mean the devel checkout
contains or has accepted that candidate.

| W | Scope | Next action |
|---|---|---|
| [W93](../backlog/work-items.md#w93) | Broader durable Raft group commit | Attribute barriers and test bounded batching across queued requests. |
| [W94](../backlog/work-items.md#w94) | Atomic CREATE plus open lease | Remove the separate HOLD round while proving lease-adoption races safe. |
| [W95](../backlog/work-items.md#w95) | Tiny-file data persistence and packing | Compare independent storage principles in efs-bench; refine P4.1. |
| [W96](../backlog/work-items.md#w96) | KV WAL preallocation | Adapt logical EOF/recovery correctly, then measure the KV workload. |

W93 and W94 are the recommended first priorities within this set. W96 is a
narrower storage experiment; W95's format/packing alternatives need more design
and recovery work. This records a proposal, not a promise of a particular rate.
W90 is scratch expiry, W91 is `.efs/` command infrastructure, and W92 is
`.efs/find`/`.efs/stats`; their scopes and acceptance remain separate.

## Measured starting point

Four cohosted NUC daemons, two FUSE mounts, TCP and buffered server I/O.
n1 leads group 0, voters n1/n2/n3; n2 leads group 2, voters n2/n3/n4.
n1/n2 use XFS on `/data1`'s SATA drive, while n3/n4 share the NVMe behind
`/data2` (`/home`). Group 0 can form a quorum on the SATA replicas; group 2
requires an NVMe replica. Whole-folder scheduling leaves workers on slower
folders longer; all eight sampled active parent directories mapped to group 2.
This explains much of the startup-to-steady slowdown without establishing that
every other cost is exhausted. W66 still owns general metadata topology/routing.

Alternating eight-worker runs created 256 files with 256-byte payloads per run,
using the same group-specific directory sets before/after round 7:

| Group | Before files/s | After Raft preallocation files/s |
|---|---:|---:|
| 0 | 159.9, 175.2 | 147.2, 163.5 |
| 2 | 18.7, 18.9 | 31.1, 31.8 |

These are short comparisons across one deployment/restart, not randomized A/B/A
or proof of a sustained whole-workload gain. Round 7's random-tree startup was
98 displayed files/s at 30 s; the untraced 27–57 s interval was approximately
68 files/s. Progress batches can lag by up to 504 files. Later lifetime rates
include deliberate profiling/test pauses. The 1,000–10,000 files/s target is
unmet, not ruled out by this experiment.

The separate generator trace measured openat 90.6 ms, close 77.8 ms and fstat
20.8 ms per call. Roughly 190 ms of serial per-file work with eight workers
is consistent with about 42 files/s in that instrumented window. Reaching
1,000 files/s with eight workers requires a mean complete cycle around 8 ms;
10,000 requires about 0.8 ms. These are concurrency arithmetic, not predictions.
Additional GETATTR/permission/coherence costs remain worth attributing; they
do not justify removing checks or reviving stale metadata caches.

The client and both leaders were profiled separately, with an additional n3
follower profile. On-CPU perf percentages identify executing code; they are not
wall-clock wait shares. One n3 pump thread spent 7.394 s of an eight-second
strace window in 1,024 Raft fdatasync calls. Fragment syncs averaged 22.4 ms.
Counts include other replication/background work and must not be divided by
an unrelated generator sample to invent operations-per-file ratios.

## Shared measurement and acceptance protocol

1. Preserve the existing namespace and record exact source patches, binary
   hashes, transport/I/O mode, device placement, leaders, filesystem headroom,
   and background load. Drain active writers before any deployment/restart.
2. Hold folder distribution and payloads fixed. Compare each group, a balanced
   mixed-group case, the original random-folder generator, and mixed large
   writers. Use repeated A/B/A where practical; report both startup and long
   untraced windows, rates and p50/p95/p99 latency. Keep generation pauses out
   of timed windows; do not mistake a new folder mix for an engine gain.
3. Attribute per-file CREATE/HOLD/stat/PUT/REPORT work, entries per durability
   batch, queue/lock time, flush latency/count, writes, physical bytes, RSS and
   follower lag. Collect separate client/leader/follower CPU profiles and
   elapsed syscall or time-weighted off-CPU evidence; trace outside rate windows.
4. Change one principle at a time, retain failures and compare equivalent
   durability. Run the item's concurrency/fault/recovery gates and full POSIX
   and two-client suites. Process crash tests do not simulate physical power
   loss. No acceptance based only on microbenchmarks or a warm 30-second peak.

Baseline round-7 private gates passed POSIX 216/0/1 (16.0 s), peer 64/64
(10.4 s), nine virtual-interface gates and persisted restart. They are
regression evidence for the prior candidate, not acceptance of W93–W96.

## Evidence

- [Before group comparison](../../results/measure/20261008-nuc-tiny-create/round-7/group-comparison-before.json)
  and [after](../../results/measure/20261008-nuc-tiny-create/round-7/group-comparison-after.json).
- [Separate perf/strace files](../../results/measure/20261008-nuc-tiny-create/round-7/after/)
  and [sync summary](../../results/measure/20261008-nuc-tiny-create/round-7/sync-summary.json).
- [Storage principle probe](../../results/measure/20261008-nuc-tiny-create/round-7/log-space-probe-r7.json).
- [Build identities](../../results/measure/20261008-nuc-tiny-create/round-7/identity.txt),
  [source patch](../../results/measure/20261008-nuc-tiny-create/round-7/source.patch),
  and [validation](../../results/measure/20261008-nuc-tiny-create/round-7/full-validation.log).

The full text and binding limits for each item are in the work-item register.
