# Alpha 1.1 — architectural consolidation

**User-directed scope and approach, October 28, 2026. Status: OPEN;
implementation and qualification NOTRUN.** Based on the graph/source review at
`09d515675`; this page records a plan, not an implemented architecture or a
performance result. No product binary, cluster or release was changed.

[Status index](README.md) · [Alpha 1.0 gates](alpha-1.0-2plus1-checklist.md) ·
[Distributed roadmap](distributed-cluster-roadmap-20261022.md) ·
[Release placement ladder D38](decisions.md#d38--metadata-placement-ladder-across-the-alpha-releases-accepted-oct-22)

## Purpose and release boundary

Alpha 1.1 makes the qualified alpha 1.0 behavior easier to understand, maintain
and extend. Its goal is fewer independently managed states and clearer ownership
at subsystem boundaries, while preserving correctness, durability, bounded
progress and simple operation. File splitting and reduced line counts alone do
not establish success.

Sequence: alpha 1.0 acceptance → alpha 1.1 consolidation and requalification →
alpha 2.0, then 3.0 and 4.0 under D38. This adds an intermediate release; it does
not redefine that ladder. Retain 2+1 data on 3, 4 and >4 nodes, the alpha 1.0
metadata placement/protection contract, simple startup and supported node addition,
and the alpha 1.0 feature/support matrix as actually released. No central service,
mandatory tuning file or new user-visible behavior selector is introduced.

The alpha 1.0 Stage 1 tasks AB–AX and Stage 3 P3-A–P3-F keep their present scope
and gates until the 1.0 cut. At the cut each gets one of three dispositions,
recorded on its checkbox: **closed** (measured, no change needed, or fixed and
gated in 1.0); **1.1 input** (a measured cost that was not fixed in 1.0 and
touches a boundary this plan consolidates — it enters the A11-A inventory as a
requirement on that slice, not as 1.0 debt); or **1.0 blocker** (it
demonstrated a correctness failure: it gets its K row and is fixed before the
cut). Nothing is deferred silently, and nothing is done twice: an AB–AX fix
that reshapes `write.c` publication, `raft_host.c` status or the buffer pool
before 1.1 consolidates those same paths is work paid for twice, so unmeasured
items in those paths wait for their measurement, and measured ones are fixed
where the fix is local and otherwise become 1.1 input. Any newly demonstrated
in-scope correctness bug is handled in its owning release gate, not postponed
because this plan prefers architectural cleanup.

The same rule applies to `agent/task-h-repair` (214 commits, +1,987/−404 lines
across 30 source files at this writing, IN PROGRESS, unmerged): it is
consolidation-shaped work (receipt replay, allocator reservation, finisher
batching, resync image requests, packed-read re-probe) grown out of one
finding. It is split, not merged whole: each correctness fix with a retained
failure lands in 1.0 on its own branch under the K rule; the refactors become
A11-B/A11-C input with their evidence pages, and the branch is retired once
both halves are accounted for.

## Why this work is needed

The graph identifies heavily connected paths, and source inspection shows that
`server_raft_host_report` and `report_dirty_ino_run` coordinate routing, sessions,
lane state, publication, receipts, storage authority and recovery. At the reviewed
source, `raft_host.c` is about 25,600 lines and `write.c` about 9,400. These sizes
are indicators of concentrated responsibility, not a complexity score or proof
that functions should move arbitrarily.

One write has several distinct facts: bytes accepted locally, an admitted upload,
fragments stored, metadata published, durability proven and cleanup permitted.
They need not become true at the same time. Several structures represent these
facts, and callers currently participate in transitions across subsystem boundaries.
Repairs have added valuable guards; consolidation must preserve those guards'
proofs while reducing duplicated coordination. Ordinary and packed data also have
separate implementations of similar buffer, I/O and durability work.

**Baseline at `cb1a6a434` (A11-A refines these; every slice is judged against
them).** `raft_host.c` 25,580 lines with 60 `h->mu` acquisition sites;
`write.c` 9,445, `inode_rpc.c` 3,832, `meta_apply.c` 10,862. Nine register
rows since Oct 23 are repairs to one surface, the client publication/REPORT/
receipt path and its server side (K13, K14, K38, K39, K40, K43, K47, W166, and
K32 as its performance face); that surface is where the alpha fixes landed and
where the next consolidation pays off first. A11-A records, for each operation
family: the number of call sites that decide completion or retry, the locks a
REPORT or a read takes end to end, and the number of distinct identities a
write carries (ticket, publication id, receipt, span base, epoch). Success is
those counts going down with the failure oracles unchanged.

The graph is navigation evidence only. It does not establish lock ownership,
happens-before, persistence ordering or complete callback coverage. Every proposed
boundary must be checked in source and exercised by tests.

## Scope and work packages

| ID | Work | Required artifact / acceptance |
| --- | --- | --- |
| A11-A | Inventory operation lifecycles and authority | Current-state diagrams and ownership tables for write/flush/fsync, read/visibility, truncate, packing/reclaim and shutdown/recovery; each linked to code and tests |
| A11-B | Consolidate write/publication ownership | One explicit coordinator contract per operation family, with immutable identities, transitions and completion obligations; callers stop duplicating retry/cleanup decisions |
| A11-C | Separate consensus hosting from operation orchestration | Reviewed interfaces between Raft admission/proof/persistence, metadata operations and background work; `raft_host.c` no longer acts as the implicit owner of every concern |
| A11-D | Consolidate equivalent storage mechanisms | Ordinary/packed I/O and durability responsibilities compared; shared primitives only where identity, ordering and failure guarantees match; differences explicitly retained |
| A11-E | Simplify client state and scheduling boundaries | Cache versus authority rules, buffer/index lifetime, publication ownership and wait/budget ownership explicit; remove duplicated state only with a replacement invariant |
| A11-F | Remove superseded machinery and qualify the integrated result | Obsolete paths/configuration removed, current-format docs/tests reconciled, graph rebuilt, full acceptance evidence retained |

### A11-A: establish the model before changing the implementation

For each operation record: who owns its memory, who may advance its state, what
identifier survives retries, which metadata group or storage fact is authoritative,
what locks and ordering are required, when a reply may be sent, and who completes
or cleans up after interruption. Include cross-group operations and all failure
exits, not only the successful path.

The write model must distinguish at least these facts; they are not one global
linear sequence shared by every path:

| Fact | Authority that the model must name |
| --- | --- |
| Local bytes accepted / dirty | Client operation and buffer owner; not durable publication |
| Upload admitted | Exact ticket and live session authority |
| Fragment stored | Exact object, holder and storage completion state |
| Metadata published / visible | Committed publication and its identity/receipt semantics |
| fsync satisfied | Required fragment durability proofs and matching boot/epoch authority |
| Retired / reclaimable | Terminal metadata/session/ledger authority; not an absent reply |

Packing, partial writes, failed replies, truncation and recovery can create branches
or obligations between these facts. Keep their necessary identities distinct; a
single lifecycle model does not mean replacing all epochs/tickets with one counter.
Choose the first bounded slice from this inventory, with a retained failure oracle
and measurable ownership reduction. No whole-system rewrite is prescribed.

### A11-B/C: define interfaces around authority and completion

A metadata operation should request a proof or proposal through a narrow contract,
not reproduce Raft routing, persistence and apply-wait rules. The contract names
what it proves, its lifetime and invalidation conditions. Raft continues to own
term/log/configuration rules and durability ordering. Operation modules own their
business state machines and immutable retry identities.

A client publication coordinator should own the outcome and remaining obligations
of that publication. Cache eviction, timers, RPC wrappers and callers must not each
infer whether the same operation is complete. An ambiguous result remains
ambiguous until its authority resolves it; a missing receipt is not an abort.
Separate bytes stored, visibility, durability and cleanup in the API results.

Start with behavior-preserving boundaries and executable contracts. Remove a
redundant check only after demonstrating that another proof covers the same scope,
term, identity and ordering. Moving code without changing ownership is an
intermediate mechanical step, not completion of A11-B or A11-C.

### A11-D/E: share mechanisms without erasing distinct guarantees

Compare ordinary and packed storage paths for identity validation, aligned I/O,
namespace durability, retry handling, quota and reclamation. Reuse validated
primitives where the contracts match. Do not force segment publication/ledger
semantics into ordinary ticket semantics simply to share code.

For the client, inventory authoritative records versus reproducible caches and
indexes. Each buffer, cache entry, ticket and pending completion needs one explicit
lifetime owner, with documented transfer rules. Preserve aggregate memory bounds,
read/drain headroom, fairness and per-operation deadlines across handoffs. Prefer
one completion mechanism per equivalent wait condition; no universal queue or
mount-wide lock that serializes unrelated operations.

### A11-F: delete replaced paths and reconcile documentation

Land one current behavior for each supported operation. Remove obsolete adapters,
state fields, flags and test branches after their replacements pass the gates.
Temporary development scaffolding must have a removal condition and cannot become
a shipped A/B mode. Tests for real historical failures remain as regression tests
against the current implementation; do not delete them merely because the old
function or field has gone away.

No backwards compatibility work: no old format decoders, retained-store migration,
mixed-build clusters or downgrade matrix. If a format change is justified, use a
fresh-format qualification cluster and fail closed on unsupported data. This plan
authorizes neither destruction of a particular cluster nor erasure of failed
investigation evidence.

## Proposed first slices (the user picks the order; record it as a D row)

1. **Client publication coordinator** (A11-B, A11-E): one owner for a REPORT's
   outcome and remaining obligations — retry identity, receipt resolution,
   stale-span replay, unit abort, degraded marks — where today `write.c`,
   `inode_rpc.c`, the dcache and the K38/K39/K40/K43 repairs each infer
   completion. Oracles that must keep failing on the old code and passing on
   the new: `test_k38_shared_chunk_pressure.py`, `test_pub_unit_forward_abort.py`,
   `test_k43_coordinator_kill.py`, `test_get_integrity.py`, `test_report_stepdown.py`,
   the K35 fsync-degraded and W166 tests. AM, AN, AV and the receipt half of
   `task-h-repair` are inputs.
2. **Management and status off the consensus path** (A11-C): `server_raft_host_status`,
   the retired-replica routing of K44, `raft-mkfs`/`raft-resync` handling and the
   W130/W165 halt behavior behind one narrow interface, with AF's published
   snapshot as the mechanism if its measurement justifies it. Oracles: the K44
   sweep test, `test_w130_dir_fault.py`, `test_k33_*`, `leader_placement.py`.
3. **Storage path unification** (A11-D): ordinary and packed fragment I/O,
   tails, directory barriers and placement (K37, K41, AP, AQ, AR inputs) over
   one validated primitive set, differences kept explicit.
4. **Client buffer and wait ownership** (A11-E): bufpool, chunk map, demand-read
   and PUT-window waits (AG, AH, AS, AT inputs) with one lifetime owner per
   object and one completion mechanism per wait condition.

Slice 1 first: it is the surface with the most repairs and the most retained
failure oracles, so its before/after is the most measurable.

## Qualification cost per slice

A slice that changes no wire or on-disk format is qualified by the loopback gate
(`make test-all`, sanitizers) plus the live gates its oracles name; the full
support matrix (D45 topologies: orcd-test in the 3/4/5/13-storage-host shapes,
TCP and RDMA) runs **once**, on the integrated 1.1 candidate, with the alpha 1.0
qualification driver and matrix unchanged. A slice that does change a format
adds a fresh-format run of the affected gates on one real-host shape before it
merges. This keeps 1.1 at one campaign, not one per slice.

## Execution and validation

1. **Freeze the baseline.** Record the released alpha 1.0 source/build, support
   matrix, effective settings, known limitations, test results and reproducible
   representative workloads. Record earlier failures separately from later PASS.
2. **Review one boundary.** Supply before/after ownership and state diagrams,
   API contracts, affected callers, lock/persistence ordering and failure cases.
   Select metrics and acceptance bounds before implementation or timing.
3. **Change one coherent slice.** Preserve external semantics, add focused
   regression/interleaving tests, and remove the superseded mechanism when safe.
   Avoid simultaneous unrelated protocol, scheduler and storage rewrites.
4. **Qualify the slice and integration.** Run relevant native and sanitizer tests,
   deterministic faults and live gates, then the full released support matrix.
   A model or extracted-C test does not substitute for transport/cluster evidence.
5. **Measure and record.** Compare matching workloads, load and hardware; retain
   raw logs/captures externally with manifests, hashes and commands. Rebuild and
   validate the graph, update architecture and status, and identify remaining
   responsibilities before selecting the next slice.

Required failure cases include lost replies before/after acceptance, duplicate
requests, leader change, session revoke, partial publication, timeout during
persistence, process crash/restart, truncated/corrupt records, quota/memory
pressure and shutdown with work in flight. Preserve existing recovery/protection
limits: process-crash qualification does not establish power-loss acceptance.
Do not manufacture PASS by changing timeouts, weakening workloads or dropping
failed cases. Old/new code comparisons use separate suitable fixtures; no
mixed-build running cluster is required.

## Alpha 1.1 exit checklist

- [ ] A11-A through A11-F complete, with every listed lifecycle and authority
  boundary documented and linked to executable coverage.
- [ ] Before/after review shows removed duplicate ownership/transitions, not just
  fewer lines or moved files. Remaining cross-boundary dependencies are explained.
- [ ] Current-format code has no superseded production path or new A/B selector;
  fresh-format changes and operational instructions are explicit where applicable.
- [ ] Alpha 1.0 behavior/support matrix requalified on the final integrated
  candidate, including 3, 4 and >4 nodes and the supported transports/features.
  Known limitations and unresolved failures retain their actual status.
- [ ] Full test suite and applicable sanitizers pass; live exact-payload,
  publication/durability, recovery, degraded operation and clean-stop gates meet
  the released contracts. Missing environment coverage is NOTRUN, not PASS.
- [ ] Matched baseline comparisons cover throughput, p95/p99 latency, CPU,
  memory, RPC/proof counts, disk bytes/barriers and wakeups where affected.
  Bounds agreed before each change are met; no unexamined small-workload regression.
- [ ] Every AB–AX and P3 item carries its 1.0-cut disposition (closed, 1.1 input,
  or 1.0 blocker) and every 1.1 input is traceable to the slice that consumed it;
  `agent/task-h-repair` is split and retired as described above.
- [ ] Each new boundary has its contract and oracle links in the code graph
  (`sh scripts/rebuild-code-graph.sh`, `python3 tests/test_code_graph.py`), and
  A11-A's diagrams cite `graphify path` output plus source lines, so the model
  is reproducible from the tree.
- [ ] Architecture, runbooks, status and graph describe the final implementation;
  retained evidence supports an explicit release decision. Tag/merge/push and
  deployment are separate actions, not outcomes of this planning document.

Broader metadata distribution, 2+2 protection, new removal/rebalancing features
and new storage/transport backends are outside this consolidation scope. They
remain in their existing roadmap/decision items. Alpha 1.1 prepares clearer
extension points for them without implementing speculative compatibility layers.
