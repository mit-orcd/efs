# Tiny-file performance follow-up — W93, W94 and W96

**Historical implementation checkpoint (Oct 8):** The review below describes the pre-merge baseline. W93 batching/fault fixes, W94 atomic server leases and W96 tail repair/reservation are now implemented and tested in focused portions. Client adoption, recovery experiments and remaining performance acceptance are still open. [Current checkpoint](w93-w96-implementation-20261008.md).


**Current status:** W93 closed Oct 10 under [its final acceptance](w93-atomic-apply-20261010.md); W94–W96 retain separate obligations in the work queue. The review and WAL failure below describe the dated pre-merge source, not current defects.

Registered Oct 8, 2026 after seven NUC profiling/implementation rounds.
All three items are **open investigations/experiments**, not completed fixes.
The round-7 experimental code and its retained evidence were committed as
`e6926d85` and merged into `devel` as `d4e70492` on Oct 8. The import includes
CREATE reply reuse, WAL/Raft `fdatasync`, already-durable Raft barrier avoidance,
and bounded Linux Raft preallocation. These are now part of devel; this does
not close W93, W94 or W96 or fix the W96 invalid-tail recovery defect.

The source review below deliberately records the **pre-merge** `cde67dca`
baseline. Its statements that those optimizations were absent/unmerged are
historical and superseded by the import above. Focused merged AMD Linux
Raft-store/core, KV-LSM, CREATE, tail-parser and open-lease regressions passed;
no new live-cluster deployment or performance comparison is claimed.

## Review against devel, Oct 8

Reviewed committed `devel` at `cde67dca` (production source unchanged from
`e1f44cc2` by the documentation merge). The unrelated working-tree W90–W92
edits are not implementation evidence. This is a source review and a local
WAL recovery reproduction; no new cluster performance results are claimed.

| W | What devel already has | Corrected remaining work |
|---|---|---|
| [W93](../backlog/work-items.md#w93) | Leader proposal holds, disk sync coalescing, multi-entry follower AppendEntries batching, and KV apply holds. | Measure the remaining serialized follower request barriers; design bounded cross-request durability/visibility gating. |
| [W94](../backlog/work-items.md#w94) | CREATE/HOLD flag and first-open/last-close lease serialization, but FUSE still creates with flags 0 then acquires HOLD. | Audit replay and hashed-directory lease atomicity, then prove safe client adoption. CREATE reply reuse is a separate unmerged precursor. |
| [W96](../backlog/work-items.md#w96) | KV WAL group commit and holds; O_APPEND; no KV or Raft preallocation in this branch. | Fix recovery-tail handling first, then measure bounded reservation without confusing allocation, logical EOF and durability. |

**Priority correction:** fix and gate the W96 recovery prerequisite below
before storage-performance changes. Then scope W94's reply reuse and atomic
lease work, and measure W93's follower barrier cost before choosing a batching
protocol. KV
preallocation remains an experiment after recovery is correct. All three
packages remain open. W90–W92 retain their existing scopes.

### Baseline and code findings

1. **The round-7 candidate is not the devel baseline.**
   [Raft log](../../src/raft/raft_log.c) and [KV WAL](../../src/kv/kv_wal.c)
   still call `fsync`; neither contains the candidate's allocation logic.
   [CREATE](../../src/client/efs_fuse.c) still uses `lookup_fill` after
   `efs_fuse_create_at`, which obtains attributes through the ordinary path;
   `efs_client_create_result` discards the returned inode after staging it.
   Reusing the CREATE reply is not present. The subsequent devel deadline
   fixes do not implement any of W93, W94 or W96. The old NUC numbers describe the
   experimental source patch, not measured current-develop performance.
2. **W93 must extend existing batching.** `host_propose` already uses shared
   disk holds, quiet local append and a bounded yield loop before release;
   `raft.c:on_ae_req` batches entries within one received request.
   `drain_inbox` still processes requests serially. Its receiver applies
   committed entries and emits replies before returning, so wrapping the
   inbox in a larger disk hold alone is unsafe: persistence dependencies
   must cover application/read visibility, successful replies and hard-state
   messages, including mixed groups, duplicates, conflicting tails and term
   changes. Preserve existing durable-byte/index tracking.
3. **W94's flag is not universally atomic.** The ordinary CREATE command can
   include the hold. First-use cross-group HASHED/SPLITTING directory lanes
   instead call `host_hashed_create_txn`, then issue a separate lease proposal.
   The earlier `host_opid_replay` return bypasses that post-create lease step.
   Test a retry after the namespace transaction commits but before the lease
   proposal completes; the existing flag is not proof of an atomic replayable
   CREATE/open operation. Client `open_edge_mu` protects known-inode local
   opens/closes, but does not itself cover a CREATE reply before adoption.
   Also close local/server resources if post-create attribute lookup or the
   FUSE reply fails. Test ordinary and first-use cross-group paths separately.
4. **W96 has a correctness prerequisite, reproduced locally.**
   `kv_wal_replay` stops at an invalid/truncated record without repairing the
   tail or returning the valid byte end; `kv_wal_open` then appends at physical
   EOF. The LSM open path invokes exactly replay followed by open. A focused
   production-WAL reproduction writes and syncs A, injects an invalid tail,
   replays A, successfully appends/syncs B, then replays only A on the next
   recovery. This establishes a WAL recovery defect; it does not establish
   end-to-end cluster data loss, since Raft/rebuild behavior needs its own gate.
   Repair/reject the tail before accepting new records and test the second
   restart. Exercise short writes, allocation failure, reset/compaction and
   concurrency as well. If reservation extends file size, explicit logical
   offsets and recovery-tail handling are mandatory; merely changing the
   allocation call or retaining O_APPEND behind an extended EOF is insufficient.
   [Reproducer and observed output](../../results/measure/20261008-w93-w96-review/README.md).

Functional production gates continue on AMD direct-I/O/TCP by default.
Reproduce the NUC per-device performance comparisons separately on equivalent
storage placement. Broader transport/I/O coverage belongs at the batch boundary
or where a change specifically affects that mode.

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
regression evidence for the prior candidate, not acceptance of W93, W94 or W96.

## Evidence

- [Before group comparison](../../results/measure/20261008-nuc-tiny-create/round-7/group-comparison-before.json)
  and [after](../../results/measure/20261008-nuc-tiny-create/round-7/group-comparison-after.json).
- [Separate perf/strace files](../../results/measure/20261008-nuc-tiny-create/round-7/after)
  and [sync summary](../../results/measure/20261008-nuc-tiny-create/round-7/sync-summary.json).
- [Storage principle probe](../../results/measure/20261008-nuc-tiny-create/round-7/log-space-probe-r7.json).
- [Build identities](../../results/measure/20261008-nuc-tiny-create/round-7/identity.txt),
  [source patch](../../results/measure/20261008-nuc-tiny-create/round-7/source.patch),
  and [validation](../../results/measure/20261008-nuc-tiny-create/round-7/full-validation.log).

The full text and binding limits for each item are in the work-item register.
