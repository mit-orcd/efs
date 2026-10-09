# W66 — Metadata scaling implementation gap

Recorded Oct 5 2026 following the user's question about 100–1,000 clients
connecting to a metadata leader. Status: **open enhancement; documentation only**. W66 is indexed in the status queue. No
running-cluster capacity measurement or topology change was made for this item.

[Status queue](README.md#1a-the-work-queue) ·
[Architecture §7.1](../how-it-works/architecture.md#71-sharded-raft-and-linearizable-reads)

## D36 protection obligation — Oct 9

The user requires four-node maximum two-loss protection for authoritative
metadata; larger clusters add scale rather than raising the failure target.
Current three-voter groups cannot meet that requirement. W66 must select and
prove its metadata persistence/commit/recovery mechanism as well as routing.
Safe refusal without quorum cannot substitute for preserving committed metadata.
See [binding D36](node-count-protection-20261009.md). No topology change is made
or advertised by this note.

## Current implementation and intended behavior

The architecture specifies 4096 logical shards, independent Raft groups,
distributed leadership, and a multi-Raft runtime with shared reactors, batched
messages, coalesced heartbeats and group-committed WAL writes. The current
implementation does not provide that distribution:

- `include/efs/kv_key.h` documents the parity mapping: group 0 owns odd shards,
  group 2 even shards. Logical shard count therefore does not represent the
  number of independent leadership or consensus streams currently available.
- `src/client/inode_rpc.c` caches only two group leaders in `g_group_leader[2]`.
  Routing prefers a known leader because follower reads require a ReadIndex
  round trip and local catch-up. Both leaders may reside on one machine;
  actual placement needs measurement.
- `raft_dual_voter_conn` prefers a voter hosting both metadata groups for
  mixed REPORT batches. `src/server/raft_host.c` attaches groups 0 and 2 and
  contains paths requiring both groups locally or forwarding to such a host.
  This topology assumption must be removed as group count increases.

A connection endpoint may forward work internally; client connections alone
do not identify every processing bottleneck. A thousand idle connections and
a thousand clients doing create/stat/close storms are different loads. Capacity
depends on operation rate, hot-key distribution, transaction fan-out, WAL/apply
cost and outstanding work. No 1,000-client capacity claim is established.

## Implementation phases

1. **Measure the present topology.** Record leaders and replica placement,
   per-group operation rates, p50/p95/p99 latency, pending proposals and reads,
   apply lag, WAL latency, CPU and memory. Attribute forwarded requests and
   REPORT coordination to their endpoints and actual participant groups.
   Compare independent directories/files with shared-directory/shared-file
   workloads so a hot object is distinguishable from a global bottleneck.

2. **Generalize topology and routing.** Introduce a versioned mapping from
   logical shard to group, replicas and leader. Replace two-entry leader caches,
   parity-only group selection, fixed voter sets and group-wide anchor
   assumptions. Audit session establishment, transaction recovery, snapshots,
   GC/reaping, management reporting and every caller using group IDs. Define
   upgrade/bootstrap compatibility and movement of existing state before
   changing the mapping; changing a hash function alone is unsafe.
   Clients cache routing information, refresh on topology/leader changes and
   use bounded, reused connections to destination nodes.

3. **Run and distribute multiple groups.** Start with a small configurable
   group count and place leaders across nodes, then expand through measured
   stages toward the documented shard-group topology. Use shared scheduling,
   transport and storage batching; do not allocate a thread, connection or
   independent fsync stream per logical shard. Replica placement must retain
   quorum and failure-domain constraints. Leader balancing needs to account
   for load as well as leader count.

4. **Remove the dual-host requirement.** Partition REPORT work by authority
   and route it to the relevant groups. Cross-shard operations use a coordinator
   and bounded participant fan-out without requiring one node to host every
   group. Preserve transaction decision visibility, replay identity, atomicity
   and error semantics; a partial batch must not become successful publication.
   Routing retries must not turn ambiguous replies into duplicate effects.

5. **Control hotspots and overload.** Verify directory partitioning actually
   distributes work across independent groups. Shared-directory and shared-file
   hotspots need their existing spread/lane protocols and batching; additional
   groups alone do not split one authority. Amortize quorum-backed read checks
   and preserve coherent client caching. Add bounded outstanding requests,
   buffers and queues with per-client fairness and backpressure. Overload must
   not cause unbounded RSS, starve Raft progress or weaken consistency.

Follower reads may later offload useful work with quorum-backed authority and
an applied-index barrier. Round-robin reads without these checks and extra
replicas by themselves do not provide independent metadata write capacity.
The architecture's prohibition on clock-based leader leases remains in force.

## Validation and completion criteria

- Unit/simulator coverage: routing changes, leader changes, retries, unavailable
  participants, crash/restart, transaction recovery, snapshot import and GC
  ownership under multiple groups. Existing correctness regressions must pass.
- Linux cluster gates: independent workloads spread across leaders and show
  additional usable capacity as metadata nodes/groups increase; quantify
  scaling efficiency and any remaining shared bottleneck.
- Run staged 100-client and 1,000-client tests when infrastructure permits,
  recording offered load, achieved throughput, latency tails, errors, queue
  depth and RSS. State workload and hardware with every capacity claim.
  Include metadata storms alongside data writes, not only idle connections.
- Hot-directory/shared-file tests expose contention without incorrect results;
  overload tests keep memory bounded and permit recovery after load subsides.
- Failover and routing-map changes preserve committed data, linearizable reads
  and transaction semantics. Define numerical performance acceptance targets
  from the baseline before using measurements as a pass/fail gate.

Keep correctness work first, including W43/D25 integration and D27/0a.
This is a separate scalability phase, not a change to the current truncate
checkpoint. The exact group-count ramp, placement policy and migration protocol
remain to be designed and reviewed; this entry records the required enhancement
and proposed implementation approach, not an assertion that it is implemented.

## Oct 8 measurement follow-up

The [NUC metadata-group performance follow-up](metadata-group-perf-followup-20261008.md)
records eight passing functional cases whose rates correlate with file-inode
group placement. It supplies a bounded measurement/routing investigation for
W66 phase 1 and P4.1 engine comparisons. It does not establish the cause, implement
generalized multi-Raft routing, or close W66.


The Oct 8 live tiny-file follow-up now includes 48 successful read-only probes:
group-2 median attribute latency is 16.5–18.7 ms versus 1.4–1.6 ms for group 0,
including direct leader contact. Slow replica durability and shared pump service
are candidates; unequal group write load and absent per-round ack timelines
prevent causal isolation. See the same follow-up for read-round freshness,
pre-sleep log semantics, evidence and the next measurement gate. No runtime
change was made and W66 remains open.

## Tiny-file measurement follow-up — Oct 9

[W92 cache profiling](query-cache-tiny-profile-20261009.md) improved controlled
cache refreshes, but live file creation fell 225 → 85 files/s after advancing
other devel code and retaining a larger namespace. This does not identify a
W66 defect. Repeat with controlled revision, namespace/placement and foreground
profiles before assigning cause; preserve read authority and persistence.
