# W66 — Metadata scaling implementation gap

Recorded Oct 5 2026 following the user's question about 100–1,000 clients
connecting to a metadata leader. Status: **open enhancement and protection
protocol; staged engine foundation tested on AMD**. W66 is indexed in the
status queue. Default production topology remains unchanged; active-client
capacity and the D36 recovery contract are not yet proved.

[Status queue](work-queue.md#1a-the-work-queue) ·
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
records group-dependent read latency on the NUC. It supplies a bounded
measurement/routing investigation for W66 phase 1. It does not establish the cause, implement
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

## Single-leader concentration review — Oct 21

**Analysis only; no code change, nothing measured for this section (NOTRUN).**
The user objected that metadata effectively lives on one instance, which
contradicts EFS being a parallel file system.

### How this came up

The objection followed large-directory problems on 2026-10-21. Local-first
placement required a migration, whose pacing and listing correctness had
separate defects; these are not inevitable consequences of adaptive placement:
- A directory with more than `EFS_DIR_SPREAD_MIN` (65,536) entries switches
  LOCAL → SPLITTING → HASHED. A background migrator then moves its existing
  names from the one local shard to 64 hashed lanes.
- **The split was paced for hours.** `HOST_SPREAD_MAX=8` bounds queue
  attempts per maintenance pass across queued directories, not eight successful
  moves per directory per second. Requeues, failures and other maintenance
  reduce progress. The archived baseline measured 464–472 moves/min; about
  2.3 h for 64.5 k moves is an extrapolation, not a completed baseline run.
  Repeated scans from the head also admit quadratic merged-key work in the
  number of leftovers/tombstones, although that cost was not yet visible in
  baseline pass timings through 16.4 k moves.
- **Readdir lost names during the split.** W125 closure test C7 (1,000,000
  files in one directory on orcd-mini) listed 999,990 names, and `rm -rf`
  left 5 files behind. Names at the moving split frontier were missed for
  the hours the directory stayed SPLITTING
  ([readdir split frontier](readdir-split-frontier-20261021.md), code fix
  `f8b7a7818`, focused private-cluster PASS; original orcd-mini C7 rerun NOTRUN).
- **An archived migrator improved one bounded run.** In a 500 k-file
  create workload, HASHED was reached 1,124 s after creation started; 64,516
  existing names moved, not 500 k. Creation itself took 1,063 s. New names
  already used hashed placement. The candidate used a resumable scan, named
  moves, and a 4,096-attempt / 200 ms pass budget, achieving 79–92 moves/pass.
  Baseline and candidate ran concurrently on the same physical host alongside
  other load; this is not an isolated speedup measurement or general scale gate. It is parked at tag
  `archive/split-migrator-linear-20261021`
  (`docs/status/split-migrator-linear-20261021.md` there) and not merged.

The user stopped further large-directory testing (W125 C1 rerun, C9 and C10
NOTRUN) and questioned the premise itself: a directory, and metadata in
general, shouldn't sit on one shard or leader and migrate later; it should
always be spread reasonably. That led to the broader question below of
whether metadata is served by one node at all. A first assessment overstated
several points; an independent review against devel `08e7fb297` corrected
it. This section records the corrected, code-backed position.

| Claim | Code-backed status |
| --- | --- |
| Exactly two metadata groups | **Correct.** Odd shards go to group 0 and even shards to group 2 (`efs_raft_shard_group`, `HOST_NGROUPS 2`), so at most two leaders exist. |
| Every node votes in both groups | **Only at three nodes.** At four or more nodes, `group_voters()` puts group 0 on nodes 1/2/3 and group 2 on 2/3/4 (`src/server/raft_host.c`). Extra nodes do not automatically add metadata group leadership or replica placement under this default; explicit membership changes are separately supported. |
| Followers never serve reads | **Incorrect.** A follower gets a fresh ReadIndex from the leader, waits for its own apply index and reads locally. Namespace routing prefers leaders, which concentrates traffic, so follower reads are a routing and qualification question, not a new protocol. |
| The balancer colocates leaders | **Only when enabled.** The common-voter preference (`host_balanced_voter`) applies only with `EFS_METADATA_BALANCE=1` (opt-in). orcd-mini does not set it, so its observed placement (node 3 leading both groups) was an election outcome. |
| Spreading leaders roughly halves leader load | **Not established.** Followers still apply every mutation, group load can be very uneven, and cross-group operations add coordination. ORCD-test captures already showed separate leaders while the active group churned and timed out. |
| The runtime contradicts the architecture | **Qualified.** Architecture §7.1 states the runtime still maps shards to two fixed groups. This is an acknowledged implementation gap toward the required multi-Raft runtime. |

Observed load (passive orcd-mini profiles, single snapshots, workloads not
controlled):
- On `9099380f3`, lock waits were 15.6% of the leader's context switches,
  nearly all on the Raft host mutex `h->mu` (submit 28%, REPORT 14%,
  retained CREATE 13%, unlink 8%) — [profile](orcd-mini-profile-9099380f3-20261017.md).
- On `f4a373b13`, the leader used about 5.8 cores and each follower about
  2.5 — [profile](orcd-mini-profile-f4a373b13-20261016.md).

These show one busy leader. They do not show how much spreading would gain.

### The design point the first assessment missed: transaction locality

More groups help only if ordinary operations touch a small, bounded number
of groups. A create touches the parent dentry, the child inode, directory
accounting, the session and publication records. If those land in different
groups, cheap operations become distributed transactions. Raising
`HOST_NGROUPS` alone also leaves the per-node host mutex, pump, WAL and KV
contention in place.

### Position for the W66 decision page

1. Treat four dimensions separately: replica placement, leader placement,
   read routing, and execution ownership (host mutex, pump, WAL, KV).
2. Measure first: per-group traffic and actual blocked time per completed
   operation, before promising gains from any change.
3. Treat follower reads as existing machinery whose routing needs evaluation.
4. Design more groups with bounded transaction fan-out, plus migration,
   versioning and recovery. Don't replicate every group to every node.
   **Open conflict:** the review suggests three replicas per group, while
   [D36](node-count-protection-20261009.md) requires four-node, two-loss
   protection that three-voter groups cannot provide. The replica count per
   group must satisfy D36.
5. A leader-spreading A/B is worthwhile. It must not be presented as the fix
   for the ~8 s publication stalls until measurements connect the two.
6. Directory spreading: the earlier review favored adaptive spreading to
   limit small-directory coordination. The user's stated requirement is
   reasonably spread metadata; the design must honor it. Bounded hashed
   placement with lazy lane allocation is an alternative to evaluate, not
   an assumption that every directory must coordinate with every group.

Remaining gates: the decision page itself, per-group traffic and blocked-time
instrumentation (the step 1 metrics in the
[Raft host mutex plan](raft-host-mu-plan-20261016.md)), and the leader-spreading
A/B. All NOTRUN.

### Independent review of the origin narrative — Oct 21

**Documentation/code review against devel `061007586`; no new cluster test
(NOTRUN).** Reviewed the source and the archived migrator evidence via
`git show archive/split-migrator-linear-20261021:docs/status/split-migrator-linear-20261021.md`.
The corrected account above distinguishes measurements, extrapolation and
uncompleted acceptance. The narrative is useful; it does not prove that one
placement policy causes all three issues (slow migration, missing listings,
concentrated metadata execution).

Reviewer corrections and recommendations:

- **Correct my earlier replica advice.** Three replicas per group was not a
  sufficient recommendation under D36. Select replica placement and durable
  commit/recovery evidence against that protection contract. Simply increasing
  to four voters does not itself establish two-loss recovery or availability.
- **Honor the user's distribution objective without assuming maximum fan-out.**
  Treat reasonably distributed metadata as the design requirement. Evaluate
  bounded hashed lanes from directory creation, with lazy allocation of used
  lanes and explicit locality for child inodes, accounting and transaction
  records. This can avoid the initial local-to-hashed transition without
  materializing every possible lane or involving every node in every update.
  My adaptive-placement suggestion is a tradeoff to explain, not permission
  to override the requested direction. The choice of lane count, group mapping
  and accounting protocol still needs design and measurement.
- **Separate directory placement from execution capacity.** Many hashed lanes
  mapped to only two consensus groups do not create dozens of independent
  metadata servers. More groups also need routing that does not require a
  common dual-group host, bounded transaction participants, and measured
  scheduling/lock isolation. Replicas still apply committed writes; leader
  redistribution alone cannot promise a proportional CPU reduction.
- **Retain migration correctness even with hashed-from-birth directories.**
  Existing LOCAL/SPLITTING directories and future topology movement still need
  valid listing cookies, no silently omitted stable names, replay-safe moves,
  layout-epoch checks and restartable ownership transfer. The parked migrator
  is evidence and possible reusable work, not an accepted deployment candidate.
  Its named-command encoding must be reviewed for replay/downgrade safety:
  an old decoder ignoring the supplied name is not equivalent application.
- **Keep beta fixes separate from the scaling redesign.** The listing fix can
  be requalified independently. The ORCD-test REPORT deadline failures occurred
  with separated leaders too, so they remain their own diagnosis and acceptance
  gate. Full payload integrity does not turn failed closes into success.

Recommended decision-page sequence: state the protection and directory-placement
requirements; specify bounded authority/transaction fan-out; compare leader/read
routing options on the existing topology; then stage versioned group distribution
and legacy-state migration. Measure independent-directory and hot-directory
workloads separately, including per-group offered/achieved rate, blocked time,
WAL/apply delay, p99 and errors. No claimed speedup, W66 closure, or deployment
authorization follows from this review.

### Assessment of the independent review — Oct 21

**Documentation/code review against devel `01eed2cdd`; no cluster test
(NOTRUN).** The review's corrections are verified against the code and the
archived evidence:
- **Pacing.** `host_dir_spread_pass` pops up to `HOST_SPREAD_MAX` (8) queued
  directories per maintenance pass, one move attempt each. Attempts per pass
  across queued directories is the precise description. With a single
  requeued directory this comes to about 8 per pass, which matches the
  measured 464–472 moves/min.
- **Baseline.** It was stopped at 16,784 moves and FINISH was NOTRUN, so the
  2.3 h figure was an extrapolation.
- **Candidate run.** 1,124 s from creation start, 64,516 moves; creation took
  1,063 s. The earlier "500 k-file split to 1,124 s" wording was wrong.

Where this assessment differs or adds:

1. **Born-hashed placement reuses existing machinery, but alone it doesn't
   distribute execution.** HASHED directories already lazily register used
   lanes in a 64-bit bitmap, keep per-lane timestamps, and use a fixed
   64-lane permutation (architecture §7.4). Creating directories HASHED
   removes the LOCAL → SPLITTING transition, and with it the migrator and
   the frontier listing defect, for all new directories. But the 64 lanes
   still map onto two consensus groups, so it fixes the migration problems
   without spreading metadata execution. Distribution needs more groups.
   Sequence: born-hashed placement (correctness and simplicity), then
   versioned group distribution (scale). The cost to evaluate is the loss of
   LOCAL CREATE co-location (dentry and inode in one atomic entry) for
   small directories.
2. **Legacy migration: necessary for topology change, not necessarily for
   legacy LOCAL directories.** Adding groups or moving lanes between groups
   needs a restartable, replay-safe move protocol regardless. For directories
   that are LOCAL today, the project's no-backward-compatibility policy
   allows a reformat-based cutover instead of in-place migration (orcd-mini
   was freshly formatted on 2026-10-19). Which way to go is a user decision.
   It shouldn't be assumed in either direction.
3. **Named `DIR_MIGRATE` encoding: valid concern, narrower exposure.** Daemons
   refuse mismatched build IDs (daemon HELLO), so mixed-version voters can't
   apply the same log concurrently. The remaining exposure is a downgrade
   that replays a retained log, where an old decoder would move a different
   name. The fix consistent with the project's fail-closed practice is a new
   command or sub-tag that old decoders refuse, not an optional trailing
   field. **Correction from the follow-up review below:** current unknown-code
   handling does not provide that refusal, and HELLO is not a universal
   replay or per-connection Raft version fence.
4. **Measurement plan: agreed.** Add per-lane occupancy and lane-to-group
   mapping to the per-group metrics. With two groups, "64 lanes" is at most
   2-way at the consensus level, so lane counts alone overstate spread.
5. **D36: agreed.** The voter count per group follows from the D36 recovery
   proof, not from throughput arguments.


### Follow-up review: placement cost and replay fences — Oct 21

**Review of devel `9ed70419f`; code/document inspection only. Runtime and
compatibility gates NOTRUN.** No deployment or stored data changed.

The corrected migration measurements stand. I support bounded, lazy,
born-HASHED placement as the next design increment, with these corrections to
the assessment above:

1. **New opcodes do not currently fail closed.** In
   `src/server/raft_host.c`, `apply_dir_cmd` returns `EFS_OK` for unknown
   directory sub-tags; the outer metadata apply dispatcher also falls through
   to `EFS_OK` for an unknown top-level command. Merely assigning a new code
   can therefore silently omit an effect on replay instead of rejecting it.
   Explicit rejection of some retired commands does not establish rejection
   of all unknown commands. A named-migration format needs a store/log version
   fence enforced before replay by every supported reader, or an explicitly
   unsupported downgrade with a fresh-format cutover. Adding rejection to a
   new binary cannot repair an already released old decoder. Test the actual
   old binary against the new retained log/store, and verify refusal before
   any apply progress; do not substitute a same-build unit test.
2. **HELLO is a join check, not sufficient proof of universal isolation.**
   `src/server/cluster.c` and the HELLO handler reject mismatched joining
   builds. However, `src/server/handler.c` dispatches `EFS_MSG_RAFT` directly
   to the Raft inbox without a preceding daemon-build declaration requirement
   in that handler. The protocol declaration guard applies to selected data
   messages. Thus the claim that mixed-version voters *cannot* apply together
   is stronger than the checked mechanism establishes. Retained-store restart
   and peer reconnect require explicit compatibility tests as well as join
   rejection; a network gate cannot protect offline replay.
3. **HASHED does not inherently lose dentry/inode co-location on every
   CREATE.** `host_create` allocates the child on `dsh`. With expiry disabled,
   its HASHED/SPLITTING transaction branch is taken for first use of a lane
   whose group differs from the parent's group. Established lanes, and
   same-group first use, use the ordinary proposal path. Other admission and
   read barriers still cost work. Measure cold first-touch versus established
   lanes separately, including consensus participants, RPCs and latency.
   Describing all small-directory CREATEs as losing a single-entry commit
   overstates the current implementation's cost.
4. **Removing the split transition removes that failure mechanism, not a
   proof obligation.** Before landing born-HASHED initialization, test empty
   root/mkdir, concurrent first lane registration, complete readdir/cookies,
   timestamps and counts, rename/link/unlink, retry deduplication and restart.
   Preserve the existing listing fix for retained SPLITTING directories.
   Existing lazy-lane machinery is useful evidence of feasibility, not a
   passing result for the new initialization path.
5. **Fresh format is a valid alternative to legacy conversion.** My earlier
   blanket requirement to migrate existing LOCAL directories was too strong.
   A specifically authorized fresh-format deployment can avoid that work.
   If existing data must survive a changed lane-to-group mapping, a versioned,
   restartable transition remains necessary. Neither a no-backward-compatibility
   policy nor a previous reformat authorizes deleting the current ORCD
   campaign's retained stores and evidence.

Recommended order: validate born-HASHED initialization and first-touch costs
on a private cluster, then design retained-data routing epochs and additional
groups. Two groups can already distribute leadership across two nodes; more
than two leaders requires more groups. Neither 64 lanes nor adding groups
alone guarantees balanced execution. Record lane-to-group mapping, actual
traffic and leader placement, including shared WAL/KV bottlenecks. Keep D36's
replica requirement independent of these throughput choices. No speedup or
correctness acceptance is claimed by this review.

### Assessment of the follow-up review — Oct 21

**Code inspection against devel `c6d8a67ff`; no runtime test (NOTRUN).**
All five points are verified or accepted. The earlier assessment's points 1
and 3 were wrong as written:
- **Fail-open apply (confirmed).** `apply_dir_cmd` returns `EFS_OK` from its
  `default:` case, and the metadata apply dispatcher ends with `return EFS_OK`.
  Any command or sub-tag outside the explicitly reserved ranges is silently
  skipped on apply or replay. A new sub-tag would therefore not be refused by
  any current binary.
- **HELLO scope (confirmed).** The daemon HELLO build check gates joining.
  `EFS_MSG_RAFT` frames go straight to `server_raft_host_inbox` with no
  per-connection build declaration, so "mixed-version voters cannot apply
  together" was too strong.
- **CREATE co-location (confirmed).** `host_create` allocates the child on the
  dentry's lane shard (`dsh`). Established lanes, and same-group first use,
  keep the single-proposal path; only cross-group first touch takes the
  transaction branch. The earlier cost statement overstated it.
- Points 4 (born-HASHED needs its own correctness gates) and 5 (fresh format
  needs explicit, per-cluster authorization and never covers the current
  ORCD campaign's stores) are agreed.

Opinions beyond W66:

1. **Fail-open apply is a correctness defect in its own right, independent
   of directory placement.** Any future command applied by a replica that
   doesn't know it is silently dropped, so replicas can diverge without an
   error. It should be fixed now, separately and cheaply:
   - the apply dispatcher and every sub-tag switch refuse unknown codes with
     `EFS_ERR_PROTO`, halting apply as the reserved tags already do;
   - a store/log format version record is checked at startup, before any
     replay, so a binary that predates it refuses the store.
   This only protects binaries built after the fix. Released binaries stay
   fail-open, so "downgrade with a retained log is unsupported" must be
   stated and honored operationally, consistent with the project's
   no-backward-compatibility policy.
2. **Raft connections need a version fence too.** Require a build/format
   declaration on each peer connection before Raft frames are accepted, or
   carry it in the Raft frame header. Then a restarted voter on a different
   build is refused at reconnect instead of exchanging entries.
3. **Order.** Fix (1) and (2) first: they are small, testable with an actual
   old binary against a new retained log, and every later format change
   (born-HASHED init, named migration, regrouping) depends on them. Then
   validate born-HASHED initialization and first-touch costs, then routing
   epochs and more groups.


### Implementation review of the fail-open proposal — Oct 21

**Reviewed devel `f52e1dedc`; source inspection only, runtime gates NOTRUN.**
The direction is right: address compatibility before introducing new retained
encodings. The three safeguards are complementary, but calling them cheap
or treating decoder return codes as sufficient understates their scope.

- **Fatal decoding must survive the apply wrapper.** `src/raft/raft.c`
  stops `apply_committed` before advancing `last_applied` when its callback
  returns an error. However, `src/server/raft_host.c:host_apply` converts
  decoder results for several command families (including LOCK) to
  `EFS_OK`, recording the original result only as a proposer verdict.
  It then passes the persisted group cursor to `efs_kv_lsm_apply_end`.
  Thus changing every unknown sub-tag to `EFS_ERR_PROTO` alone does not
  guarantee fail-closed apply. Validate the encoding before mutation and
  before business-verdict normalization, or carry a distinct fatal decode
  classification through that wrapper. Preserve legitimate CAS/conflict
  rejections as consumed commands; making all PROTO results fatal would
  also change existing policy/SALT semantics. Tests must inspect both the
  persisted KV cursor and Raft applied index, not merely the decoder result.
- **Stopping apply is not necessarily stopping service.** A callback error
  proves the current apply loop stops; it does not by itself prove that the
  daemon exits, withdraws readiness, stops serving reads, or avoids retrying
  the same incompatible entry indefinitely. Specify an observable incompatible
  state, refuse readiness and affected service, and retain the offending
  group/index/term/tag diagnostic. Do not skip or truncate the entry to regain
  availability. Distinguish this permanent incompatibility from transient
  storage errors that can legitimately retry.
- **A newly added store marker cannot make an old binary check it.** The
  previous assessment's startup-marker sentence contradicts its subsequent
  caveat. Only readers implementing that fence can reject unsupported versions.
  Define missing-marker behavior, supported legacy-store handling, and durable
  marker creation/update ordering before new-format writes. Cover snapshots,
  restore/import and WAL replay, not just the normal startup path. Existing
  pre-fence binaries remain unsupported for downgrade unless an independently
  verified old-reader refusal mechanism exists.
- **Peer fencing needs an explicit contract.** An exact build-ID requirement
  is a reasonable conservative policy under coordinated upgrades; a protocol
  or format capability version is a different, broader compatibility policy.
  Pick one deliberately. Check both directions on every new/reconnected Raft
  connection and every supported transport, before handing frames to Raft.
  Bind the declaration to the expected peer identity; declaring a build is
  compatibility checking, not authentication. Keep join, peer transport and
  offline store compatibility as separate gates.
- **Report the defect without inventing an incident.** The inspected unknown
  command paths can cause divergence when replicas interpret an entry
  differently; this review has not reproduced such divergence in a running
  homogeneous cluster. It also does not establish that every decoder or
  every unknown sub-tag behaves alike. Preserve the specific confirmed paths
  and add an inventory of command families and malformed-length handling.

Acceptance should include: unknown top-level and directory/LOCK sub-tags
through the full apply wrapper; truncated encodings with no partial effects;
unchanged cursors after rejection and restart; valid business rejections that
still advance; incompatible startup/snapshot restore; and mixed-build peer
reconnect over TCP and RDMA. Run both compatibility directions. An actual
pre-fence binary against a new store/log is a characterization test: it may
reproduce unsafe acceptance, and must not be reported as a refusal gate PASS.
Use disposable private fixtures for these tests, never retained ORCD stores.

My recommendation is a focused compatibility-hardening change before new
wire/on-disk encodings, with the fatal-decode path and its full-wrapper tests
first. Born-HASHED design and read-only analysis can proceed independently;
not every initialization change inherently changes an encoding. Keep the
new format and transport gates blocking any change that actually requires
them. No implementation or runtime fix is claimed by this documentation commit.

### Assessment of the implementation review — Oct 21

**Code inspection against devel `44ec54a50`; no runtime test (NOTRUN). No
code changed.** The review's central claim is confirmed. In
`src/server/raft_host.c`, `host_apply` maps the apply result of a long list
of command families (among them CREATE, UNLINK, LOCK, SETATTR, LANE_*, REAP_*,
PACK_*, GC_ACK and CLOSE_*) to `ret = EFS_OK`, including `EFS_ERR_PROTO`.
With `ret == EFS_OK` it writes the group cursor through
`efs_kv_lsm_apply_end`. A malformed or unknown entry in those families is
therefore consumed and the persisted cursor passes it. The earlier
"cheap fix" wording is withdrawn: returning PROTO from decoders is not
sufficient.

I agree with all five points. Additions:

1. **Use a distinct fatal class, not PROTO.** PROTO already carries business
   meaning (policy CAS, SALT, refusals) and must keep advancing.
   - Introduce a separate result, for example "unsupported encoding", that
     decoders return only for unknown codes and impossible lengths, checked
     before any mutation.
   - `host_apply` never normalizes it. **Corrected by the review below:** if
     an apply scope was opened, it must call `apply_end(kv, NULL)` to abort
     and unlock, rather than skip cleanup. The cursor stays put, and the
     group enters a sticky incompatible state: apply stops,
     readiness is withdrawn, reads and writes for that group are refused, and
     group/index/term/tag are kept as a diagnostic. No retry loop, no skip.
   - This keeps every existing verdict policy unchanged and makes the fatal
     path testable through the full wrapper.
2. **Choose two policies on purpose: build identity for peers, format
   version for stores.**
   - Peer connections: exact build-ID equality, as the join-time HELLO
     already does. That matches the no-backward-compatibility rule and
     coordinated upgrades, and is the simplest check to state.
   - Stores, snapshots and imports: a monotonic format version, not the build
     ID. A build ID changes with every commit and would block every ordinary
     upgrade of a retained store. The format version changes only when an
     on-disk or log encoding changes.
3. **Born-HASHED may not need a new encoding.** Directory rows already carry
   a layout field with LOCAL, SPLITTING and HASHED values, and current
   binaries read HASHED directories. If born-HASHED only sets the existing
   field at MKDIR and reuses existing lane registration, it adds no new log
   or store encoding. In that case it can proceed before the compatibility
   fence, as the review allows. Confirm per change; any new command or field
   stays blocked behind the fence.
4. **Agreed:** a pre-fence binary run against a new store or log is a
   characterization test. It is expected to show unsafe acceptance and
   documents why downgrade is unsupported. It is never a refusal PASS.


### Final implementation constraints for compatibility hardening — Oct 21

**Review against devel `bf1949f37`; source inspection only. Runtime gates
NOTRUN.** The separate fatal classification and separate peer/store policies
are sound directions. These corrections are needed before implementation:

1. **Abort the apply scope; do not skip its cleanup.**
   `efs_kv_lsm_apply_begin` in `src/kv/kv_lsm.c` takes `l->writers` for write
   and establishes thread-local staging. `efs_kv_lsm_apply_end(kv, NULL)`
   clears staged effects, resets the scope and releases that lock without
   committing a cursor (also documented in `include/efs/kv_lsm.h`). Skipping
   `apply_end` after a successful begin leaks the scope and writer lock.
   Validate before opening the scope where possible; if validation fails
   inside an opened scope, abort exactly once on the same thread, retain the
   fatal classification, then fence service. Include a test proving another
   group using the shared KV can still make progress after this rejection.
   Audit non-KV side effects separately; aborting staged KV does not undo them.
2. **"Any error ... regardless" is too broad.** The current wrapper restores
   IO/NOMEM errors for an explicit list of command families; begin and commit
   failures also override the apparent success. The confirmed problem is
   decoder/encoding errors reaching business-verdict normalization, not an
   unconditional consumption of every failure. Preserve transient error
   handling while making the fatal classification immune to every later
   normalization branch, not merely the first assignment to `ret`.
3. **A format version covers interpretation as well as byte structure.**
   Unchanged bytes can acquire incompatible semantics. For example, changing
   MKDIR apply to initialize HASHED rather than LOCAL changes replay behavior
   unless the layout choice is already encoded or replay is explicitly
   versioned. Reading existing HASHED rows successfully does not prove that
   historical MKDIR logs will replay identically. Check that concrete path
   before declaring born-HASHED format-neutral. Bump compatibility when
   necessary for command meaning, routing rules or snapshot interpretation,
   not only when adding a field. A monotonic number alone is insufficient:
   define supported versions, missing-marker policy and upgrade ordering.
4. **Sticky means no retry on the unchanged incompatible build, not permanent
   inability to recover.** Refuse operations depending on the affected group,
   including cross-group requests and cached/read fast paths. Keep status and
   diagnostics available. Define how a compatible restart rechecks the
   offending entry and resumes from the unchanged cursor. Do not reuse the
   existing automatically cleared `kv_incomplete` condition as this fence
   without changing its lifecycle. A passing recovery test upgrades the
   reader and consumes the retained entry exactly once; it never deletes it.
5. **Separate unsupported-format and malformed-entry diagnostics.** Both may
   require the same fatal control flow, but an unknown supported-version tag
   or invalid length is not necessarily evidence of a newer legitimate
   format. Record the reason without claiming corruption or version skew
   has been demonstrated. Validate complete entries, including all members
   of batches, before mutations; do not limit validation to top-level tags.

I agree with exact-build peer fencing for coordinated upgrades and a separate
persistent compatibility version. The next useful step is implementation and
focused tests, rather than another round of agreement prose: abort/unlock,
unchanged durable and Raft cursors, service fencing, compatible restart, and
unchanged business/transient-error behavior. Keep this as compatibility work
linked from W66; it is not evidence that metadata distribution or the ORCD
qualification gates have passed. No code or deployment changed in this review.

### Assessment of the final constraints — Oct 21

**Code inspection against devel `8ccfeb9d6`; no runtime test (NOTRUN). No
code changed.** All five constraints are verified or accepted:

1. **Abort, don't skip (verified).** `efs_kv_lsm_apply_end(kv, NULL)` clears
   the staged memtable and cleanup marks and releases `l->writers` without
   committing a cursor (`src/kv/kv_lsm.c`). Skipping it would leak the writer
   lock. The earlier "skip `apply_end`" wording was wrong.
2. **Scope of the normalization (accepted).** `host_apply` restores IO/NOMEM
   for listed families, and begin/commit failures override the result. The
   confirmed defect is decoder and encoding errors reaching verdict
   normalization, not consumption of every error.
3. **Born-HASHED is not format-neutral (verified; the previous assessment's
   point 3 is withdrawn).** In `mkdir_batch` (`src/meta/meta_apply.c`) the
   `want_layout` carried by the MKDIR entry is the *parent's* layout, used to
   place the dentry. The new directory's own row is zero-initialized
   (`memset(&row, 0, …)`), so its LOCAL layout is implicit and not recorded
   in the log. Making MKDIR create HASHED directories would change what
   historical MKDIR entries replay to. Born-HASHED therefore needs the
   child's layout carried explicitly in new MKDIR entries (absent meaning
   LOCAL) or a versioned replay rule. Either way it depends on the
   compatibility fence, so the order is: fence first, then born-HASHED.
4. **Sticky but recoverable (agreed).** A compatible restart rechecks the
   retained entry from the unchanged cursor and consumes it exactly once.
   `kv_incomplete` is not reused as the fence.
5. **Separate diagnostics, whole-entry validation (agreed),** including every
   member of batch commands.

This review thread has converged: no open disagreements remain on the
compatibility work. The remaining items are implementation choices, best
settled in code with focused tests: abort and unlock, unchanged durable and
Raft cursors, service fencing, compatible-restart recovery, unchanged
business and transient-error behavior, and an explicit child layout in
MKDIR. W66 distribution decisions stay open; this is compatibility work
linked from W66, not evidence for it.


### MKDIR replay-path review — Oct 21

**Reviewed devel `be1541f28`; code inspection only, runtime tests NOTRUN.**
The abort/unlock correction stands. Compatibility hardening has converged at
the requirements level, but the MKDIR conclusion needs path-specific wording:

- **Same-group operation replay has an implicit child layout.**
  `apply_create_cmd` dispatches S_IFDIR CREATE entries to the mkdir helpers;
  `mkdir_batch` in `src/meta/meta_apply.c` interprets `want_layout` as the
  parent's layout and zero-initializes the child's row. Unconditionally
  changing that apply-time default would reinterpret historical entries.
  This establishes semantic incompatibility, not that every born-HASHED
  implementation must change the byte format in the same way.
- **The transaction path already carries the child row.**
  `server_raft_host_mkdir` builds `crow`, serializes it with
  `efs_meta_pack_inode`, and supplies `v_cino` to `host_prep_async` as an
  `EFS_TXN_PUT`. Its log therefore carries the child's serialized layout.
  Old transaction records retain their recorded LOCAL row. Audit and test
  both paths: changing only `mkdir_batch` would leave transactional mkdir
  producing LOCAL children, while changing only the transaction builder
  would leave the fast path unchanged. Include root creation separately.
- **Explicit child layout is my preferred operation-format solution, not
  the only possible one.** Preserve the old opcode/encoding's LOCAL meaning;
  introduce a strictly validated versioned encoding for new requests, with
  child layout separate from parent routing layout. A versioned replay rule
  is also possible, but must distinguish historical entries deterministically.
  A new global startup format number must not retroactively reinterpret all
  old MKDIR entries. An existing transaction encoding might also express the
  new row, at a cost that must be measured rather than assumed acceptable.
- **"Absent means LOCAL" must mean a recognized legacy encoding.** A
  truncated new-format record is malformed, not a legacy request with a
  missing optional field. Recognize versions and lengths unambiguously,
  including the existing operation-ID trailer. Test truncation, unknown
  layout values and unsupported versions through the full apply wrapper.

The focused MKDIR acceptance fixture should mix legacy operation entries,
legacy transaction entries and new directory creations across both paths.
Replay from a clean store and from a snapshot plus log suffix; compare child
layouts, inode/dentry placement, lane registration, complete listings and
operation-ID outcomes. Verify retry/restart creates one visible directory
with one allocation and one parent-link update. "Exactly once" here concerns
durable effects; execution may be attempted again after an aborted apply.
Keep the unknown/malformed rejection tests and cursor checks from the prior
review. These are required tests, not results from this documentation review.

One remaining wording correction: IO/NOMEM retry exceptions are explicitly
listed by command family, not a proof that every storage/memory error in every
handler retries. Do not generalize beyond that inspected wrapper. Proceed to
implementation with these gates; no further agreement-only review is needed.

### Assessment of the MKDIR replay-path review — Oct 21

**Code inspection against devel `2f5cada24`; no runtime test (NOTRUN). No
code changed.** The path distinction is verified:
`server_raft_host_mkdir` zero-initializes the child row `crow` (so LOCAL),
packs it with `efs_meta_pack_inode` into `v_cino` and proposes it as an
`EFS_TXN_PUT`. Transactional MKDIR entries therefore record the child's
layout explicitly. Only the operation path (`apply_create_cmd` →
`mkdir_batch`) leaves it implicit. The previous assessment's point 3 is
corrected accordingly: the semantic incompatibility is specific to the
operation path.

All points are accepted. My remaining opinions:

1. **The transaction path already shows the target property.** A replayed
   entry carries the exact row it creates, so its meaning can't drift with
   apply-time defaults. For the operation path I prefer the same property:
   a new, strictly validated operation encoding that carries the child
   layout separately from the parent routing layout. The old opcode keeps
   its LOCAL meaning unchanged.
2. **Row encoding is itself a format surface.** Because transaction entries
   embed `efs_meta_pack_inode` output, any future change to the packed row
   (new fields, new layout values) is a log-format change too. The
   compatibility version must cover the packed row encoding, not only
   command opcodes.
3. **Agreed:** a truncated new record is malformed, never a legacy request;
   versions and lengths are recognized unambiguously, including the
   operation-ID trailer.
4. **Agreed:** the mixed-history MKDIR fixture (legacy operation entries,
   legacy transaction entries and new creations; replay from a clean store
   and from a snapshot plus log suffix) is the right acceptance gate. IO/NOMEM
   retry is limited to the inspected wrapper's listed families.

I concur that no further agreement-only review rounds are needed. The next
step is implementation against these gates, when the user starts it.


### Review of the transactional-row conclusion — Oct 21

**Reviewed devel `2f3313885`; source inspection only, runtime gates NOTRUN.**
I agree with the explicit child-layout proposal and mixed-history replay gate.
No change to their implementation order is needed. Two qualifications make
this a precise handoff rather than a broader safety claim:

- **Transaction rows avoid this particular apply-default drift; they are
  not generally proven format-safe.** `pack_inode` writes layout at byte 56.
  `unpack_inode` in `src/meta/meta_apply.c` requires only `n >= INO_VAL`,
  copies that byte without checking the layout enum, and ignores extra bytes.
  `res_add_put` in `src/meta/txn.c` calls that decoder for inode values.
  Successful decoding there therefore does not establish support for a new
  layout value or a longer row format. Other callers may impose checks;
  this inspection does not demonstrate a complete malformed-row exploit or
  a running-cluster divergence. Preserve the narrow conclusion: historical
  transactional MKDIR records retain their serialized child-layout value.
- **Include embedded values in the compatibility boundary.** A known
  transaction opcode can carry an unsupported inode-row encoding. Define
  accepted row versions, lengths and layout values at the relevant validated
  apply boundary, before committing effects or advancing the cursor. Do not
  simply tighten this shared decoder to exact length without auditing its
  existing callers and supported formats. Test an otherwise valid transaction
  carrying an unsupported row/layout, including snapshot/import entry paths,
  and verify rejection through the full wrapper. Neither a known opcode nor
  successful byte unpacking is sufficient evidence of compatibility.

For the proposed fast path, explicit layout need not mean logging the entire
inode row. A deterministic versioned operation containing all required inputs
can preserve replay just as well. Keep parent-routing layout and child layout
separate, retain legacy operation semantics, and avoid introducing a second
row-construction implementation that disagrees with the transaction path.
The acceptance comparison should check resulting state across paths, not
require their log bytes or transaction counts to match.

The recommendation remains implementation plus the already specified gates.
This review adds embedded-row validation coverage; it does not claim the
transaction path is broken for today's valid rows, or that compatibility,
born-HASHED initialization, or W66 scaling is implemented or qualified.
