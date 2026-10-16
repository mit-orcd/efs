# W66 — Metadata-group performance follow-up

Status: **open investigation and benchmark-control follow-up**, recorded Oct 8.
This note supports [W66](metadata-scaling.md), without registering another
work item, closing the package, or claiming a confirmed daemon defect.

## What group 0 and group 2 mean

These are independent metadata Raft groups, each with its own leader, replicated
log and quorum. The [inode shard](../../include/efs/kv_key.h) retains the inode's
low bits; [group selection](../../include/efs/raft.h) assigns odd shards to group
0 and even shards to group 2. Root inode 1 belongs to group 0.

With four NUC daemons, group 0 has voters on nodes 1/2/3, and group 2 on nodes
2/3/4. With three Xorinox storage daemons, both groups use all three voters.
See [voter selection](../../src/server/raft_host.c). Both groups use the same
Raft implementation; neither is an intended fast or slow tier. Group IDs do not
select the data-I/O mode. The file-inode group below
labels inode placement; chunk lanes and other metadata operations can involve
both groups.

## Next bounded investigation

1. Record actual file inode IDs/groups before timing and keep both placement
   classes in results; do not change the production shard mapping or
   repeatedly discard slow samples.
2. Record leaders and terms before/after each measured phase, actual RPC
   destinations/forwarding, ReadIndex wait/retry time, apply lag, pending reads,
   and WAL/sync latency.
3. Profile matched group-0/group-2 cases separately from unprofiled rates to
   distinguish client waiting, forwarding, consensus/apply work and data I/O.
   Carry any confirmed routing issue into W66 with a focused regression and a
   repeated before/after gate.

## Live tiny-file / ReadIndex investigation, Oct 8

A read-only investigation of the installed NUC `devel-find-stats` build
`befaee95` reproduces the slow group-2 path without changing the workload,
daemons, leadership, storage settings or durability. This is W66 phase-1
evidence, not an implemented fix.
[Compact evidence and exact probe](../../results/measure/20261008-nuc-read-waits/README.md).

Twelve alternating `raft-getattr` probes per cell target existing file inodes
11434383 (group 0) and 1946256 (group 2), through nodes 1 and 2. All 48 succeed.
Timing includes CLI launch, connection and reply; no strace is attached during
these probes.

| Inode group | Contact node | Median ms | Mean ms | Min–max ms |
|---|---|---:|---:|---:|
| 0 | 1, leader | 1.63 | 1.68 | 1.25–2.11 |
| 0 | 2, follower | 1.38 | 1.46 | 1.19–2.00 |
| 2 | 1, non-host | 16.53 | 16.17 | 1.96–29.50 |
| 2 | 2, leader | 18.74 | 19.70 | 3.63–37.40 |

Group-0 leadership remains node 1, term 327; group 2 remains node 2, term 369.
Direct contact with the group-2 leader does not remove the latency, so an
extra client-to-nonleader hop alone does not explain it. Group 0's commit index
is unchanged during this probe while group 2 advances by 86: write pressure is
unequal. One inode per group and 12 samples per cell do not isolate topology,
metadata size or hardware as a cause.

### Why equal indices can still wait

The installed `host_view_covers` requires a completed read round begun after
request admission, in the admitted term, plus a covering read index and applied
state. `host_read_index` admits an arrival to round `previous + 1`; arrivals
while a round is pending wait for a later round. Equality of `want`,
`read_index` and `applied` alone does not establish fresh quorum authority.
The `read-sleep` line prints its view from **before** sleeping, rather than a
fresh post-wake view, and omits requested/current round and acknowledgement
state. It cannot by itself diagnose apply lag or a lost wakeup. Removing this
freshness requirement would weaken linearizable reads.

### Evidence for a pump/quorum bottleneck

The earlier 30-second strace window records 99 group-2 leader read sleeps on
node 2, 20.04–32.03 ms (mean 23.55), versus one on node 1. Metadata WAL
`fdatasync` averages 0.34/0.37 ms on nodes 1/2 and 9.49/9.80 ms on nodes 3/4;
Raft-log sync averages 0.14/0.16 ms versus 5.26/5.24 ms. Nodes 3/4 share the
NVMe-backed home filesystem; nodes 1/2 use the SATA-backed /data1 filesystem.
This establishes a measured path asymmetry, not its device-level cause.

Group 0's voters are nodes 1/2/3, allowing a quorum on nodes 1+2; group 2's
voters are nodes 2/3/4, requiring at least one of nodes 3/4. Source inspection
shows each host's shared pump drains messages and then performs KV WAL sync
and applied-index persistence before servicing its next cycle. Raft append
batch persistence can also occur during inbox processing under the host lock.
A heartbeat/read acknowledgement need not itself append a log record, but can
queue behind this work. This makes slower replica pump service a concrete
candidate for group-2 read-round latency; no per-round heartbeat/ack timeline
was captured, so causal attribution remains open.

### Next implementation/acceptance gate

Add bounded per-round diagnostics recording admission/required round,
completed round, term, ack mask, send/receive timestamps and pump phases,
including off-lock durability time. Existing `EFS_READ_PROF` and Raft
observation switches are startup-gated; enabling them on these installed
processes would require a separate planned restart. First use an isolated
matched-load setup with controlled inode placement and equivalent group write
pressure. Compare both storage paths under equivalent sync load, then repeat
with common replica backing storage to separate topology from media/pump delay.
Retain correctness and close durability throughout. Do not skip ReadIndex,
reuse pre-admission rounds, move live metadata, or tune filesystem barriers as
a shortcut. W66 remains open; no daemon fix is established by these captures.

## Intel observation — Oct 10, 2026

Integrated `d88899f3`, four direct-I/O/TCP daemons on two physical Intel hosts,
was tested while preserving the million-file trees and running the creation
workload. The private Intel2 POSIX run timed out after 30 seconds in
`concurrent_appends_two_proc`. Three unchanged production probes returned
PASS/FAIL/PASS at 11.2/33.6/22.0 seconds (failure includes cleanup); the timeout
was not increased or reclassified as a skip.

Two subsequent instrumented diagnostics used the same three persistent append
writers and forty unique eight-byte records each. Both verified all 120 records
with no duplicates or extras. An odd-shard/group-0 inode completed in 9.574 s:
maximum OPEN 7 ms, median write 135 ms, maximum write 924 ms, maximum CLOSE
317 ms. An even-shard/group-2 inode completed in 19.881 s: maximum OPEN 17 ms,
median write 241 ms, maximum write 1961 ms, maximum CLOSE 1253 ms. The production
client also logged REPORT_CHUNKS calls taking approximately 1.1–1.3 seconds.

These are two observations with different inodes/time windows and concurrent
background work, not a matched controlled group comparison or proof of a
specific persistence cause. Both hosts have separate NVMe devices for `/data1`
and the `/home`-backed second storage location. The bounded append publication
path and durability barriers remain unchanged. Longer diagnostic success does
not satisfy the original 30-second acceptance budget or close W66/W97. The
original timeout and all subsequent outcomes remain retained in Intel evidence.
