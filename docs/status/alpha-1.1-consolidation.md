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
and gates. They are not silently deferred to 1.1. Record their final disposition
at the 1.0 cut; 1.1 may build on their results without counting the same work twice.
Any newly demonstrated in-scope correctness bug is handled in its owning release
gate, not postponed because this plan prefers architectural cleanup.

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
- [ ] Architecture, runbooks, status and graph describe the final implementation;
  retained evidence supports an explicit release decision. Tag/merge/push and
  deployment are separate actions, not outcomes of this planning document.

Broader metadata distribution, 2+2 protection, new removal/rebalancing features
and new storage/transport backends are outside this consolidation scope. They
remain in their existing roadmap/decision items. Alpha 1.1 prepares clearer
extension points for them without implementing speculative compatibility layers.
