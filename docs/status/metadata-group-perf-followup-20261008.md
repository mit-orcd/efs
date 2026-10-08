# P4.1 / W66 — Metadata-group performance follow-up

Status: **open investigation and benchmark-control follow-up**, recorded Oct 8.
The [NUC checkpoint](nuc-storage-engine-ab-20261008.md) passes functional gates;
its placement-dependent rates do not establish an engine ranking. This note
supports existing P4.1 and [W66](metadata-scaling.md), without registering another
work item, closing either package, or claiming a confirmed daemon defect.

## What group 0 and group 2 mean

These are independent metadata Raft groups, each with its own leader, replicated
log and quorum. The [inode shard](../../include/efs/kv_key.h) retains the inode's
low bits; [group selection](../../include/efs/raft.h) assigns odd shards to group
0 and even shards to group 2. Root inode 1 belongs to group 0.

With four NUC daemons, group 0 has voters on nodes 1/2/3, and group 2 on nodes
2/3/4. With three Xorinox storage daemons, both groups use all three voters.
See [voter selection](../../src/server/raft_host.c). Both groups use the same
Raft implementation; neither is an intended fast or slow tier. Group IDs do not
select the physical storage engine or data-I/O mode. The file-inode group below
labels inode placement; chunk lanes and other metadata operations can involve
both groups.

## Evidence and limits

All eight NUC cases pass content verification, acknowledged-data recovery after
restart, deletion, and physical data reclamation. Session logs record 256
distinct file inode IDs per case. Five cases have even/group-2 file inodes and
read at 5.68–6.25 MiB/s. Three have odd/group-0 file inodes and read at
120.34–216.23 MiB/s. Initial leader placement alone does not explain this split:
fast and slow cases both include leaders split between nodes 1 and 2.

This is an observed correlation, not a causally isolated diagnosis. Candidates
include endpoint/forwarding behavior, ReadIndex and catch-up waits, authority
routing and cache effects. Preserve the raw waits and retries when investigating;
do not remove checksum verification, required persistence, or linearizable reads
to obtain a higher rate. The group-0 subset lacks containers/buffered, so the
current cross-mode matrix is not balanced by metadata placement.

The [raw evidence and reproducible checker](../../results/measure/20261008-nuc-engine-ab/README.md)
retain every sample, actual daemon argument, placement observation and failed
startup attempt. The runner's startup readiness race is repaired; it is separate
from the unresolved performance split.

## Comparison with Xorinox

Same application workload: one directory, 256 × 512 KiB files, four workers,
30-second read/churn phases, and durable 128 KiB overwrites with `fsync`.
These are logical client rates; delete counts namespace unlinks, with physical
GC checked separately. Ranges are min–max of two repeats and include slow runs.

| Containers | Metric | Xorinox, after rounds 7–8 | NUC |
|---|---|---:|---:|
| buffered | read MiB/s | 141.83–143.00 | 5.68–6.21 |
| buffered | durable overwrite MiB/s | 22.72–22.79 | 1.19–1.20 |
| buffered | delete files/s | 360.53–464.96 | 17.49–17.68 |
| direct | read MiB/s | 5.98–136.81 | 6.05–142.28 |
| direct | durable overwrite MiB/s | 3.70–22.65 | 1.00–3.20 |
| direct | delete files/s | 22.10–516.61 | 17.64–752.63 |

Sources: [Xorinox checkpoint](container-profiling-rounds7-8-20261008.md) and
[NUC checkpoint](nuc-storage-engine-ab-20261008.md). Some NUC peak reads/deletes
exceed the observed Xorinox peaks, but there is no consistent NUC speedup;
durable overwrite rates are substantially lower in these samples.

Hardware and topology also differ: NUC runs four daemons plus the client on one
bare-metal host, with two SATA-backed and two NVMe-backed roots. Xorinox uses
three storage VMs and a gateway VM, with VM disks sharing host NVMe. Both retain
installed-service load. Xorinox file-inode placement was not classified in that
comparison. These observations do not isolate virtualization overhead, raw
physical-drive performance, or a hardware-caused regression. The runtime source
is the same container implementation apart from comments/test instrumentation;
native CPU tuning, build metadata and topology differ.

## Next bounded investigation

1. Add benchmark placement controls and record actual file inode IDs/groups
   before timing. Select fresh test directories by observed placement, and
   verify the intended distribution; do not change the production shard mapping
   or repeatedly discard slow samples. Keep both placement classes in results.
2. Run a balanced matrix on the NUC: two file-inode groups × four engine/I/O
   combinations × two alternating repeats, with the existing payload, workers,
   duration and integrity/restart/physical-GC gates. This is 16 measured cases.
   Fix the client endpoint and settings, record source/binary hashes and backing
   devices, and retain shared-host load observations.
3. Record leaders and terms before/after each measured phase, actual RPC
   destinations/forwarding, ReadIndex wait/retry time, apply lag, pending reads,
   and WAL/sync latency. Hold leadership comparable where supported; otherwise
   report it as another experimental factor rather than attributing it to engine
   choice. Confirm the file-inode group remains the intended class.
4. Profile matched group-0/group-2 cases separately from the unprofiled rate
   matrix, using strace and perf to distinguish client waiting, forwarding,
   consensus/apply work, and data-engine CPU/I/O. Establish the specific slow
   path before changing daemon code. Carry any confirmed routing issue into W66
   with a focused regression and a repeated before/after gate.
5. Compare engines within the same placement/topology class. Repeat on Xorinox
   with the same controls before making a NUC-versus-VM claim. Integration with
   newer shared `devel` Raft changes is a separate revalidation step; do not mix
   revisions within a comparison.

Keep `files` as the default while this investigation is open. The balanced
benchmark controls, diagnosis and acceptance runs remain to be implemented.


## Live tiny-file / ReadIndex investigation, Oct 8

A read-only investigation of the installed NUC `devel-find-stats` build
`befaee95` reproduces the slow group-2 path without changing the workload,
daemons, leadership, storage settings or durability. This is W66 phase-1
evidence, not an implemented fix or a controlled engine comparison.
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
