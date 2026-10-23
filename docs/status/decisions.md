# Decisions — taken and pending (register D1–D37)

**October 15 reconciliation:** [Review of stale claims and remaining gates](open-item-reconciliation-20261015.md). Read these annotations before treating dated implementation descriptions as current tasks. No new global acceptance or deployment is implied.

**Decided means the design was accepted; it does not mean implementation or
acceptance is complete. Preserve accepted choices and check their current work
items before implementation. ASK rows remain unresolved design choices.**

[Work queue + status](README.md) · [In flight](in-flight.md) ·
[Architecture](../how-it-works/architecture.md)

---

#### Decisions — taken and pending (register D1–D37)

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

## D39 — authoritative configured cluster membership (PROPOSED Oct 22)

Proposal written at the user's request after the W153 review:
[d39-cluster-membership-proposal-20261022.md](d39-cluster-membership-proposal-20261022.md).
Today identity (`--node-id`), membership (`--join` + HELLO + persisted node
table) and metadata topology (`EFS_MD_RAFT_N`, default 4) have no shared
source of truth, which is the W153 defect. Options: A, strict per-daemon
count (W153a only); **B (recommended for alpha 1.0), one configured member
list passed to every daemon and to `raft-mkfs`, hashed into HELLO and the
committed MKFS, replacing `--join`, `--node-id` and `EFS_MD_RAFT_N`, with
later changes through `raft-change` plus a replicated `CLUSTER_SET`**; C,
control-plane group (post-alpha scope option). **Review: B's bootstrap
direction is sound, but online CLUSTER_SET and cross-group installation are
not yet specified.** See the proposal's §6 for the actual decisions: fixed
versus online membership in alpha, local identity selection, and initial
formation readiness. These recommendations are not accepted decisions.
Parent-session response in the proposal's §7: agreed, with the note that
peers already pass the W133 declaration gate (bind the digest there) and
that the MKFS salt plus the configuration digest can be the cluster
identity carried to both groups. The Oct 23 review (§8) requires explicit
formation/admission states: independent pre-MKFS salts, existing connections
and partial installation across both groups must not bypass agreement.
Placement eligibility is not a complete readiness proof. The parent session's §9
response accepts all of it and specifies unformed/forming/formed admission
states, installation in both groups followed by peer redeclaration, a digest
independent of the salt, and three-fact readiness. **§10 identifies remaining
formation liveness gaps:** formed peers can strand a voter missing the pair;
local-only installation can expose node 1 before group 2 is ready; management
formation needs its own permitted path. Resolve these in a state/role table
before implementation. The §10 review's three counterexamples (lagging voter stranded by
strict formed admission, node 1 declaring completion before group 2's SALT,
no management role before formation) are accepted; the §11 response replaces
§9 with a catch-up learner channel, a two-group completion predicate and a
peer/client/mgmt role, and gives the consolidated state and role table.
The §12 review then showed that a lagging configured voter cannot be
demoted to a learner and that the table needed every connection direction;
the §13 response decouples formation state from Raft membership (voters
always vote, acknowledge and may campaign) and replaces the table with a
declarer/admitter matrix keyed on pair conflict, with a deterministic
transition harness required before any closure claim. The §14 review (identity changes on existing connections, the declaration
reply, conjunctive service) is accepted in §15, which chooses a per-frame
identity tag with generation, binds the reply, folds the `complete` predicate
into the table (version 3) and fixes the work order: model tests, then
admission code through the real entry points, then cluster gates.
**§16 review:** generation checks scoped to one connection leave other
connections stale; receive-time checks need ordered validation at queued-work
effect boundaries. Specify peer-wide/incarnation rules and exact binding
acceptance before implementation. No protocol gate has run.
The §16 review (generation scope across connections, effect-boundary
fencing, exact acceptance rule) is accepted in §17, which sets the
authoritative identity and fencing rules R1–R6: per-member binding under one
lock as the authority, exact-tuple frame acceptance with redeclaration on a
newer generation, revalidation at every effect boundary with one explicit
linearized-work exception, and incarnation-based restart.
Awaiting the user;
W153a remains on hold.

**Oct 23 §12 review:** the table still conflates lagging voters with learners
and omits required connection directions. Preserve Raft quorum membership,
allow recovery elections during partial formation, and specify discovery and
replication in both directions. Endpoint declarations check consistency, not
peer authentication. D39 remains proposed and W153a held.

**§14 review of `c6d19da6f`:** the expanded matrix fixes voter semantics and
connection direction coverage, but declaration-only checks cannot enforce
identity learned later on an existing connection. Specify invalidation or
per-frame rebinding on both endpoints, validate declaration replies, and keep
service permissions conditional on complete formation. D39 remains proposed;
W153a held, protocol tests NOTRUN.

## D38 — metadata placement ladder across the alpha releases (accepted Oct 22)

User wording:

> alpha 1.0 only supports 2+1 across at most 3 nodes, even if we support > 3
> nodes for data placement
> alpha 2.0 additionally supports 2+2 across at most 4 nodes, even if we
> support > 4 nodes for data placement
> alpha 3.0 additionally supports 2+1 across as many nodes as defined, i think
> there will be a setting: cluster nodes = N, data placement = N, meta data
> placement = N − M (M could be zero)
> alpha 4.0 additionally supports 2+2 across as many nodes as defined, with the
> same setting

Interpretation: the ladder is about **metadata** placement and protection.
Data placement uses every cluster node from alpha 1.0 on.

**Amendment (user, Oct 22, T3 of the third integration pass):** "at most 3
nodes" in alpha 1.0 means each metadata *group* has three voters (2+1, one
loss). For N = 3, both groups use nodes 1–3. For N ≥ 4, the default
is SHARD on nodes 1–3 and SHARD2 on nodes 2–4, spanning four nodes.
K28's `efs_md_group_voters` layout stands; nodes 5 and up host no metadata
group in this default layout. These are three full Raft replicas with a
majority of two, not erasure-coded metadata fragments. The data path uses
2+1 erasure coding. One arbitrary node loss leaves a quorum in each group;
this does not promise two-node-loss availability or distribute metadata work
onto nodes 5 and up. Explicit Raft membership reconfiguration can place
voters elsewhere; the default masks are not a hard runtime placement limit.

| Release | Metadata groups live on | Metadata protection | Data placement |
| --- | --- | --- | --- |
| alpha 1.0 | default: both groups on nodes 1–3 for N = 3; SHARD on 1–3 and SHARD2 on 2–4 for N ≥ 4 (amended Oct 22, T3); N is `EFS_MD_RAFT_N`, which deployments set to the member count (W153) | one loss per group (three replicas, quorum two) | all N nodes, 2+1 stripes |
| alpha 2.0 | at most 4 nodes | two losses (2+2; the D36 four-node metadata protocol) | all N nodes |
| alpha 3.0 | N − M nodes, configured (M may be 0) | one loss | all N nodes |
| alpha 4.0 | N − M nodes, configured | two losses | all N nodes |

Consequences recorded now:
- **W151 (raise the four-node cap) is alpha 1.0 data-placement work only.**
  Metadata stays on three voters per group (both on 1–3 for N = 3; on
  1–3 and 2–4 for N ≥ 4); a fifth and later node serves data and clients
  and hosts no metadata group in the default layout. This
  matches the W151 brief.
- **Packed segments are data. Before K28 they were placed on a group's
  voters** (`packer.c`). Under this ladder, "data placement = N" means small-file
  packed data should also use all N nodes in alpha 1.0; keeping it on the
  three voters would confine small files to three nodes. **Decided (user,
  Oct 22): packed segments are data; alpha 1.0 places them on the data ring
  across all N nodes.** This is a W151 sub-item. Code consequence: a node that
  does not host the group cannot read the pack object locally
  (`server_raft_host_pack_object` refuses with `host_hosts`), and both the
  PUT_SEGMENT admission check (`segment_store.c`) and the reclaim check
  (`reclaim.c` `segment_acceptable`) depend on that read, so non-voter nodes
  need a linearizable remote object read from a group voter before they can
  accept or reclaim a segment fragment. **Implemented and merged in K28
  (`a2942cb0f`):** data-ring placement and the remote object read are in
  devel; the five-node live gate remains NOTRUN.
- The alpha 3.0/4.0 setting (N, data = N, metadata = N − M) is a deployment
  input in the sense of A8, not an A/B switch.
- D36's final contract is unchanged; alpha 2.0 is where its four-node metadata
  mechanism is owed.

## D37 — packing and container engine removed from devel (taken Oct 16)

**Taken 2026-10-16 at the user's request:** all tiny-file packing code and
the experimental container storage engine left devel (commit `5ded11179`), to
be resumed later. The last commit with both is `6c274f47f`, at tag
`archive/packing-containers-20261016` (branch `packing-containers`).

**Follow-up direction (user, 2026-10-18):** small-file packing is to return
as the default small-file storage.
- **Background packing after close** (the user's choice after the early P1
  measurement failed packing at close): files are written exactly as
  today, and an always-on background packer moves closed small files into
  packed segments, retiring their per-file fragments through W124.
- One write path; no runtime or build switch, and no setting that disables
  the packer.
- Immediately before it lands, the last devel state without the packer is
  preserved as a tag (`archive/unpacked-small-files-<date>`).
- The archived client packed writer and the container engine stay out.

Work and gates: [W125](../backlog/small-file-packing-default.md), after
[W124](../backlog/fast-delete-reclaim.md) parts 1–2.

## D36 — node-count protection contract (accepted Oct 9)

Three nodes are the minimum deployment; four provide maximum two-node-loss
protection for acknowledged data and authoritative metadata; additional nodes
increase volume and performance across all dimensions. Default data profiles
are 2+1 at three nodes and 2+2 from four onward. This supersedes the older
four-node/f=1 and f=3 ladder. The four-node metadata mechanism remains unresolved,
not permission to weaken the requirement. See [binding contract and gates](node-count-protection-20261009.md)
and [architecture §2](../how-it-works/architecture.md). W74/W66 implementation
and acceptance remain open.

**Release sequencing, October 22:** the user sets [Phase 1 / alpha 1.0](alpha-1.0-2plus1-checklist.md)
as completing 2+1 on 3, 4 and >4 nodes before continuing the remaining roadmap.
This intermediate release does not claim D36 two-loss protection on 4+ nodes;
the accepted final requirement above is unchanged. **Reaffirmed October 22
after the [plan review](alpha-1.0-plan-review-20261022.md):** >4-node clusters
stay in alpha 1.0, so raising the code's four-node cap (W151) is alpha work;
2+2 and broader metadata distribution follow alpha 1.0. The user's three
conditions for alpha 1.0: fix all known in-scope bugs; functional completeness
in scope; several rounds of performance testing and improvement that do not
bleed into post-alpha changes. NFS re-export and macOS clients are not alpha
scope (user, Oct 22): beta backlog W152; alpha supports the Linux FUSE client.

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
| D25 / W43 | Logical truncate with per-part epoch histories; deferred versioned reclamation, safe history retirement and bounded apply | Branch `d25-logical-truncate` (Oct 22, not yet merged): one 86 B inode-group TRUNCATE with opid receipts, lazy lane adoption, `TRUNC_SWEEP`/`SWEEP_DONE`/`RETIRE` with metadata clips and masked packing; the eager path, `LANE_FENCE`, tail stubs, the client tail rewrite and the switch are removed (step 5). Gates g0–g8 PASS on private node9901 and node9900 clusters (TCP and RDMA); multi-host placement, a sweep scheduler without both groups local, and >2 GiB sweep throughput remain. [Contract](../backlog/decision-contracts.md#d25--logical-truncate), [protocol](d25-logical-truncate-design-20261021.md), [step 5 evidence](d25-step5-gates-20261022.md), [routing](d25-admission-routing.md) |
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
