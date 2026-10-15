# Decisions — taken and pending (register D1–D36)

**October 15 reconciliation:** [Review of stale claims and remaining gates](open-item-reconciliation-20261015.md). Read these annotations before treating dated implementation descriptions as current tasks. No new global acceptance or deployment is implied.

**Accepted October 12, 2026 — container testing scope:** the container backend remains experimental; all container-specific testing, comparison and qualification is excluded from required work, including future releases, until explicitly reauthorized by the user. Files and files-backed packing tests remain required. [Binding policy](container-testing-policy-20261012.md).

**Decided means the design was accepted; it does not mean implementation or
acceptance is complete. Preserve accepted choices and check their current work
items before implementation. ASK rows remain unresolved design choices.**

[Work queue + status](README.md) · [In flight](in-flight.md) ·
[Architecture](../how-it-works/architecture.md)

---

#### Decisions — taken and pending (register D1–D36)

Each row is a choice the spec did not make. A row marked **decided**
was accepted by the user and is now part of the design; implement it
per the item it points at. A row marked **done** is history. Nothing
in an ASK row authorizes implementation; explicit user instructions govern work. D14 was never
assigned. D15/D16 (closed Oct 2) and the Sep 28/29 ordering tables are
in project-history.md "START-HERE closed items". **D28, D29 and D30 were
accepted Oct 8, 2026.** Their current contracts and remaining gates are below;
former ASK descriptions are historical.

## Release scope — v0.3.0-pre-alpha (accepted October12)

The user limits this release to TCP/direct I/O/files and defers containers and
repair/recovery qualification. This is a release scheduling decision, not a
change to the numbered architectural protection contracts. [Exact decision,
remaining gates and limits](v030-release-scope-20261012.md). Execution stays stopped.

## D36 — node-count protection contract (accepted Oct 9)

Three nodes are the minimum deployment; four provide maximum two-node-loss
protection for acknowledged data and authoritative metadata; additional nodes
increase volume and performance across all dimensions. Default data profiles
are 2+1 at three nodes and 2+2 from four onward. This supersedes the older
four-node/f=1 and f=3 ladder. The four-node metadata mechanism remains unresolved,
not permission to weaken the requirement. See [binding contract and gates](node-count-protection-20261009.md)
and [architecture §2](../how-it-works/architecture.md). W74/W66 implementation
and acceptance remain open.

## Earlier decisions — compact register

Exact accepted text, rationale and dated measurements are preserved in
[earlier decision records](../archive/decision-records-d1-d24.md). Consult the
relevant contract when changing that component; do not load the full historical
measurement narrative for unrelated work. Archiving detail does not revoke it.

| ID | Topic | Register state |
| --- | --- | --- |
| D1 | what do N-1 shared-file writes do under conflict? | accepted span-commutation contract; bounded folded-replay history gap is [W88](../backlog/work-items.md#w88) |
| D2 | may `open()` return before the chunk map is known; how big is the map window? | accepted contract; gate state belongs to its W item |
| D3 | may the 5/s REAP_DONE gate be raised to remeasure ior-hard 1/9/36? | accepted contract; gate state belongs to its W item |
| D4 | when does a group take a snapshot and truncate its log? | accepted contract; gate state belongs to its W item |
| D5 | may a follower answer heartbeats while its pump applies an InstallSnapshot diff? | accepted contract; gate state belongs to its W item |
| D6 | may `mdraft/` (Raft log, KV WAL, segments) live on a root the fragment writers do not use? | superseded by D11 |
| D7 | First-PUT sentinel to skip the storage-root probe | accepted contract; committed retry/cache repair and acceptance limits are [W85](../backlog/work-items.md#w85) |
| D8 | how does an 86 234-record REPORT reach the log? | answered historically: ~3 ms per batch; no larger-entry wire change indicated; remeasurement is separate |
| D9 | may the apply path (the pump) block on L0 back-pressure? | accepted: pump does not wait for compaction; current source defers flush, while W23 follower memory/lag gate remains open |
| D10 | Byte-triggered L0/L1 compaction and range splitting | accepted byte rule; file-cap treatment revised by D12/D13; W23 owns gates |
| D11 | is `mdraft/` sharing an XFS with the fragment writers the 100 ms `fsync` mode? | accepted contract; gate state belongs to its W item |
| D12 | Restore a file-count backstop | accepted bound; L1-rewriting backstop superseded by D13; W23 owns gates |
| D13 | when the file cap forces a compact, may that compact read the range's L1? | accepted contract; gate state belongs to its W item |
| D17 | Per-lane present-chunk counts for server-authoritative st_blocks | accepted contract; gate state belongs to its W item |
| D18 | Evict whole cold staging tabs to reclaim their allocation floor | accepted contract; gate state belongs to its W item |
| D14 | No assignment | unassigned in the recorded register; do not infer a design |
| D15/D16 | Leader stickiness / peer transport class | closed; motivating sender-channel bug was fixed |
| D23 | Clean body transfer to read cache | landed/gated historical record in the archive index |
| D24 | Close-time REPORT | accepted/in-tree historical record in the archive index |

D19–D22 were missing from the active index, but their identities and later
implementation records exist in [project history](../archive/project-history.md):

| ID | Topic | Recorded state and limit |
| --- | --- | --- |
| D19 | Early PUT eligibility for completed chunks; REPORT still at close/sync | implemented with W30 in the historical W28–W35 rollout; no publish-on-write approval |
| D20 | Fragment digest trailer instead of a separate checksum inode | historical rollout records the 4 KiB direct-I/O tail; format changes still need explicit design review |
| D21 | Compact common-case REPORT record | historical W35 implementation; not authorization for further wire changes |
| D22 | RDMA frame capacity of at least 84 KiB | historical W35 implementation; modern real-hardware gates remain separate |

The initial ask text and later rollout record are retained in that history;
this review does not create a new approval or infer acceptance on today's
cluster. D15/D16 and D23/D24 records are linked from the
[archive index](../archive/README.md#decisions-d).

**D25–D26 decision history.** The initial Oct 2 01:45Z recommendation
was accepted, then D25 was revised at 05:15Z to logical truncation with deferred
reclamation. The revised D25 row and normative architecture govern; the earlier
in-entry drain recommendation does not. D26 implementation state is in W44;
its development-cluster gate is not a blanket production capacity claim.

## D25–D27 — accepted, implementation separately gated

The [full accepted contracts and gates](../backlog/decision-contracts.md) remain
active; they are moved out of this running register, not archived or revoked.
Read the relevant contract before changing its component.

| ID | Accepted choice | Current state / gates |
| --- | --- | --- |
| D25 / W43 | Logical truncate with per-part epoch histories; deferred versioned reclamation, safe history retirement and bounded apply | Metadata/read/resize/publication primitives staged; FUSE integration, live-file sweep and public activation remain. [Contract](../backlog/decision-contracts.md#d25--logical-truncate), [handoff](in-flight.md), [routing](d25-admission-routing.md) |
| D26 / W44 | Pending-GC watermark plus bounded resumable scan | Historical development gate recorded; current reclamation correctness/diagnostics are W61–W63. [Contract](../backlog/decision-contracts.md#d26--bounded-gc-scan) |
| D27 / 0a | Bound recovery attempts, retain unresolved bytes, sticky inode errors and refuse ordinary teardown before unresolved drain | Runtime foundations present; strict timing, contention/fault/recovery/RSS gates remain. [Contract](../backlog/decision-contracts.md#d27--retained-failed-writeback), [handoff](in-flight.md) |

## D28–D30 — accepted Oct 8, 2026

The user accepted all three recommendations in this chat ("i take your
recomendations, document"). Acceptance settles these choices; implementation,
fault/recovery tests and activation gates remain separate.

| Decision | Accepted choice | Implementation / acceptance |
| --- | --- | --- |
| D28 | Unpublished buffered writes have no survival guarantee after client death; automatic salvage is deferred. | Preserve D27 retention and error reporting while alive, refused ordinary teardown, explicit forced-discard reporting and successful synchronization durability. W72/W86/D31 revocation and collection gates remain. |
| D29 | Durable, queryable committed/apply-pending publication receipts, integrated with existing operation identities and final results. | Implement with W88; retain validation until equivalence is proven. Lost replies, leader change, restart, partial results, retirement and cold-byte gates are required. |
| D30 | Bounded admission before Raft proposal with replica-aware pacing and bounded client queues. | Implement with W23/W89; retain committed work, reserve recovery/cleanup capacity and prove pressure bounds and catch-up. |

Full contracts and gates: [D28](../backlog/decision-contracts.md#d28--client-death-and-unpublished-writes),
[D29](../backlog/decision-contracts.md#d29--durable-publication-receipts),
[D30](../backlog/decision-contracts.md#d30--bounded-admission-and-replica-pacing).
Order: D29 alongside W88 replay correctness, then D30 pressure control. D28
establishes the failure contract; it does not activate session/ticket collection.

## Historical ordering

The Oct 2 A–O sequencing table is [archived](../archive/decision-plan-20261002.md).
Its former D28–D30 ASK state is superseded by the Oct 8 acceptance above;
historical measurements and failures remain evidence. Accepted means approved
design, not completed implementation or deployment acceptance.

<a id="d28--unresolved-client-death-salvage-choice"></a>
<a id="open-ask-rows--d29-p13-and-d30-p25"></a>
<a id="p13--d29--the-report-receipt-performance-plan-row-ask"></a>
<a id="p25--d30--the-apply-lag-remedy-performance-plan-row-ask"></a>

Former ask anchors are retained for existing links. Their current contracts
are D28–D30 above; the first-send validation skip requires its own proof under
D29, and no compaction-shape change is implied by D30.

**D25 writer admission routing — ACCEPTED Oct 6 2026.** The user selected
lane-local authoritative views before activation. Ordinary write admission must
read only its publication lane under ReadIndex; inode/lane coordination is
reserved for cold lane bootstrap and rare resize/fence operations. Do not
activate the staged inode-plus-lane RPC per application write. See the
[implementation checkpoint and remaining gates](d25-admission-routing.md).


<a id="d31--recorded-abandoned-upload-policy"></a>

## D31 — recorded abandoned-upload policy

The Oct 7 [GC implementation checkpoint](gc-implementation-20261007.md#approved-unpublished-body-policy)
records user approval: unpublished uploaded fragments may be discarded only
after their client's session is durably revoked, using durable PUT tickets.
This review assigns the missing register identity to that recorded policy; it
does not create a new approval or independently recover the approving exchange.

**Implementation:** [W86](../backlog/work-items.md#w86), pure metadata
state machine committed in `b3a11877`, staged for integration; production integration and distributed fault gates remain open.
Completed global revocation, durable lane/storage rejection floors, all-member
deletion acknowledgements and replay-floor retirement are required. Age,
disconnect and missing receipt are not authority. D28 accepts loss of unpublished buffered writes after client death and defers
automatic salvage; it does not relax these collection-authority requirements.

## D32–D35 — accepted namespace, scratch and command policies

**Accepted Oct 8, 2026:** the user accepted all four recommendations after the
W84 sizing and W91 command/script clarifications.

| ID / work item | Accepted choice | Contract / remaining gates |
| --- | --- | --- |
| D32 / W84 | Current 144-participant limit, no hardcoded directory depth; prompt atomic refusal with diagnostics; larger-operation protocol deferred. | [Contract](accepted-namespace-scratch-20261008.md#d32--w84-bounded-namespace-transactions) |
| D33 / W90 | Opt-in fixed deadlines, owner renewal within operator limits, singly linked regular files, clock-safe conditional namespace deletion. No universal TTL. | [Contract](accepted-namespace-scratch-20261008.md#d33--w90-opt-in-fixed-scratch-expiry) |
| D34 / W91 | Opt-in `.efs/` commands, initially readable Python 3 scripts using bounded versioned query paths and immutable results; real names win collisions. User revision: `.efs/find` and `.efs/stats` replace legacy `.find`/`.stats` under W92. | [Contract](accepted-namespace-scratch-20261008.md#d34--w91-directory-local-efs-commands) |
| D35 / W81 | Sustained directory-local contention plus activity triggers spread, with hysteresis, bounded migration and measured configurable thresholds. | [Contract](accepted-namespace-scratch-20261008.md#d35--w81-sustained-contention-directory-spreading) |

Accepted policy does not establish deployment acceptance. Numeric W81 settings,
query encoding and other implementation details must satisfy the linked gates;
configured expiry TTL is an operator choice, not a selected global default.


## RDMA test placement — accepted user instruction Oct 11, 2026

All RDMA work is tested either on the AMD cluster with software RDMA or on
`orcd-test` with hardware InfiniBand/RDMA. Hardware claims require the hardware
environment; software results remain separately scoped. Intel is the TCP
acceptance deployment, and its RDMA prerequisites do not block this work.
This selects test environments, not a new storage/protection contract or an
acceptance verdict. Track current-code transport validation under
[W106](../backlog/work-items.md#w106) and preserve each related work item's gates.
