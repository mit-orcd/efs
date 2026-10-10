# W66 — Metadata scaling implementation gap

Recorded Oct 5 2026 following the user's question about 100–1,000 clients
connecting to a metadata leader. Status: **open enhancement and protection
protocol; staged engine foundation tested on AMD**. W66 is indexed in the
status queue. Default production topology remains unchanged; active-client
capacity and the D36 recovery contract are not yet proved.

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

## Four-voter foundation and fresh AMD baseline — Oct 10

Host and Raft engine now share membership validation, allowing four voters
with quorum three. Joint 3→4→3 changes and cold snapshot membership restart
pass. Tests cover every one-node omission and all six two-loss pairs; real
SYNC stores retain an exact command copy after abrupt process exit and removal
of any pair of store directories. Two cold survivors cannot elect, authorize
reads or commit new writes. A negative control fails on the parent engine.
Fatal ASan/UBSan exposed and fixed a null zero-length move in empty snapshot
installation. See [raw evidence](../../results/measure/20261010-w66-foundation/README.md).

This does **not** satisfy D36: copy survival lacks independently recoverable
commit authority. Durable commit proof and verified quorum restoration remain,
as do versioned routing, group distribution, migration and measured scaling.
The existing default 0x7/0xe placement has not been activated as four voters.

Fresh private AMD direct-I/O/TCP baseline: 800 successful GETATTRs, idle cell
p50 0.96–0.97 ms and write-loaded p50 1.43–1.51 ms, including CLI startup.
Both group leader-loss/rejoin gates and cold four-daemon restart pass. Leaders
were distributed across nodes 2/3; this is one host and two groups, not proof
of metadata scale or independent failure domains.

### Membership persistence correction — Oct 10

Recovery audit found ignored membership-save errors and snapshot/config ordering
that could discard the membership log before saving its replacement. Failed
saves now fence the replica and prevent apply advancement; local/imported
snapshots save membership before dropping history. Invalid cold/incoming
snapshot membership and load IO are refused. Ten assertions fail against the
prior engine; Raft/disk/simulator and fatal sanitizer runs pass with the fix.
AMD direct/TCP private POSIX 212/212 and peer 64/64, staged leader-loss replay,
cold bytes and physical GC pass; installed service restored. See
[separate evidence](../../results/measure/20261010-w66-foundation/membership-fix/README.md).
W66 protection, generalized topology and measured scaling remain open.

### GETCHUNKS endpoint/leader-hint retry correction — Oct 10

GETCHUNKS now retries endpoint loss and missing/self NOT_PRIMARY hints through
another voter while retaining exact FileID/range, caller deadline and outputs.
Sixteen attempts remain bounded. Controlled tests fail on the previous function
and pass on the fix. Full native/docs and separate AMD direct/TCP POSIX 212/212,
peer 64/64, lane leader-death replay, cold bytes and GC pass. The earlier live
readback assertion lacked an error code, so it is not causally attributed to
this fix; the gate now prints that code on failure. See
[routing evidence](../../results/measure/20261010-w66-foundation/routing-fix/README.md).

### Explicit private four-voter integration — Oct 10

A separate AMD direct/TCP fixture reconfigured both groups through production
`raft-change` to 0xf, verified all four hosts report converged membership,
passed 800/800 reads and both leader-loss/rejoin cases, and preserved membership
and exact target inode identity through cold restart. See
[raw results](../../results/measure/20261010-w66-foundation/four-voter-live/README.md).
Both leaders initially occupied one node: replica count is not leadership
balancing or additional useful capacity. Installed topology was untouched.

The [protection/routing implementation proposal](w66-protection-routing-design-20261010.md)
makes the remaining protocol and gates explicit. D36 leaves post-two-loss
availability unresolved. The recommendation is verified preservation followed
by replacement nodes and quorum restoration before writes resume; the user has
been asked to select that contract. Durable commit evidence, recovery fencing,
versioned routing/migration and measured scaling still prevent W66 closure.

### Quorum-preserving leader distribution — Oct 10

The engine supports voluntary handoff to a recently responsive, durably
caught-up voter. The source fences reads/new proposals before sending a
TimeoutNow trigger; the target still needs the normal election quorum,
durable term/vote and campaign/pressure admission. Lost/failed/stale triggers,
joint configuration, lagging/offline peers and two-of-four partitions cannot
confer authority. Fatal sanitizer and engine/wire gates pass.

`EFS_METADATA_BALANCE=1` opts into deterministic preferences over each group's
actual voter set. The shared pump waits two seconds after election/configuration
and backs off thirty seconds after a submitted/failed handoff. Re-election
retains the backoff. Default is off; this does not change group membership or
the fixed odd/even map. Legacy preferences put group 0 on node 1 and group 2 on
node 3; the explicitly configured four-voter fixture uses nodes 1 and 2.

Separate private AMD direct/TCP legacy and four-voter gates pass 800 reads each
and 36 exact-inode recovery probes each. Preferred placement is verified after
initial election, leader rejoin and cold restart. These CLI measurements include
startup and use two mutation writers; no throughput improvement or active-client
capacity claim follows. See [leader-distribution evidence](../../results/measure/20261010-w66-leadership/README.md).

This is a tested prerequisite, not W66 closure or D36 activation. The durable
commit-evidence/recovery protocol, persisted versioned routing, migration,
additional group capacity and controlled hotspot/scaling gates remain open.

The full staged gate exposed an unresolved BUSY on peer OPEN during concurrent
append (core POSIX 212/212, peer 63/64). No handoff occurred at that failure
boundary. OPEN now retries the exact retained lease edge within the caller's
earlier deadline or a 30-second ceiling; timeout keeps cleanup ownership.
The one-shot negative control fails the new regression, while the fixed
ownership/deadline and existing serialization tests pass. The failure is
preserved in the [OPEN evidence](../../results/measure/20261010-w66-leadership/failed-peer/README.md).

The corrected OPEN candidate then passed twenty repeated concurrent append
races (16,000 exact records), full peer POSIX 64/64, cold exact bytes and drained
GC queues; independent physical inventory is empty. Canonical native tests and
all five documentation checks pass. Installed AMD direct/TCP service is restored.
See [completed regression evidence](../../results/measure/20261010-w66-leadership/open-fixed/README.md).
