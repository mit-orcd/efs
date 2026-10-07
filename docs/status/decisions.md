# Decisions — taken and pending (register D1–D31)

**Decided means the design was accepted; it does not mean implementation or
acceptance is complete. Preserve accepted choices and check their current work
items before implementation. ASK rows remain unresolved design choices.**

[Work queue + status](README.md) · [In flight](in-flight.md) ·
[Architecture](../how-it-works/architecture.md)

---

#### Decisions — taken and pending (register D1–D31)

Each row is a choice the spec did not make. A row marked **decided**
was accepted by the user and is now part of the design; implement it
per the item it points at. A row marked **done** is history. Nothing
in an ASK row authorizes implementation; explicit user instructions govern work. D14 was never
assigned. D15/D16 (closed Oct 2) and the Sep 28/29 ordering tables are
in project-history.md "START-HERE closed items". **D28, D29 and D30 remain ASKS.** D29/D30 detail is in the
[open ask rows](#open-ask-rows--d29-p13-and-d30-p25); the former A–O
ordering table is historical, not current implementation status.

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

## D28 — unresolved client-death salvage choice

| item | question / choices | state |
| --- | --- | --- |
| **D28 · who owns the bytes across client death — ASK (restated Oct 2 04:55Z)** | What D27 leaves: a client that dies (SIGKILL, node loss) with *pending* or *failed* bytes loses them, and the original case (0a) is a process that **closed without `fsync`, whose `flush` failed or stalled, and whose client then died** — the application has already gone. Two things the user established: (1) an intent persisted only at `fsync` protects nothing in that case, because no `fsync` happened; a server-side intent can protect bytes only if it is durable **before the acknowledgment being protected** — for `write()`-returned bytes that acknowledgment is the `write()` itself, i.e. the rejected "every write publishes" cost class, or at the latest the `flush`; (2) under the visibility contract (§3) a durable intent **never** justifies a successful `fsync` — publication must complete; an intent can only make a later recovery possible. So the choice is: **(i) accept the loss on client death and forced teardown** as the written unrecoverable-writeback policy, with D27's controlled-teardown report as the whole contract and ENOSPC/EIO as the live signals; or **(ii) a server-side intent as best-effort salvage, scoped precisely** — one design, not the only one: at detection time (the client is alive, the servers reachable, only the publication rejected) the client persists an intent naming the inode, ranges, PUT objects and opid; a group-side recovery pass after client death can then complete or *name* the loss. Another: periodic server journaling of pending ranges, which can protect some pending writes too. **What (ii) protects, exactly: stalled writes whose intent was successfully persisted** — there is a death window between detection and persistence, and pending bytes are covered only if a journaling variant catches them; neither variant makes every acknowledged write survive death — only durable acceptance *before* the acknowledgment does that, which is the rejected cost class for `write()`. `fsync` still returns EIO until publication lands under either | not to be built either way until decided. (ii) is a protocol and metadata change (intent key kind, a recovery pass like `host_txn_recover_pass`, a client identity that survives restart, and the question of why an intent write succeeds where the publish did not); bring its cost after D27's gate exists and after 0a (a) says whether the Oct 1 stall was a defect or contention |

---

The [public-path review](spec-implementation.md) records W71–W77; accepted
decisions and healthy POSIX results do not close those implementation gaps.

## Historical ordering

The Oct 2 A–O sequencing table is [archived](../archive/decision-plan-20261002.md).
Use the current status queue; its old implementation states are not current.
Decided means approved design, not implementation or acceptance. D28–D30
remain unresolved asks; no new decision is created by this cleanup.

## Open ask rows — D29 (P1.3) and D30 (P2.5)

The primary ask texts are rows N and O in the
[historical ordering record](../archive/decision-plan-20261002.md); below are
the active performance-plan asks. That archived order is not the current queue.

### P1.3 · D29 — the REPORT receipt (performance plan row; ask)

**Item.** **D29** REPORT receipt

**Status.** **ask** — bring with P1.2

**What changes.** a "committed, apply pending" receipt (group, index, term, op identity) the client waits on / queries instead of re-sending; not a publication, frees no dirty bytes; with it the per-record `efs_meta_apply_chunk_holds` get in pack (one get per record, 100–133 µs under `l0=100–170`, added only to make byte-identical resends cheap) can be skipped on a first send — that skip is part of the ask, not a separate change

**Gate / done when.** lost receipt and leader change tests (archived row N); pack_ms per record on the 16× dd drops from ~130 µs to the proposal cost

**Forbidden.** implementing any of it before the decision


### P2.5 · D30 — the apply-lag remedy (performance plan row; ask)

**Item.** **D30** apply-lag remedy

**Status.** **ask**, blocked on P2.4

**What changes.** candidate per class: compaction shape (D9/D10 revisit), apply batching, sender pacing

**Gate / done when.** —

**Forbidden.** code before the decision


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
disconnect and missing receipt are not authority. D28's broader client-death
salvage/durability choice remains unresolved.
