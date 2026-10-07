# In flight — current agent handoff

[Status index](README.md) · [Decisions](decisions.md) ·
[Architecture](../how-it-works/architecture.md)

Reviewed Oct 7, 2026. Historical rollout facts are in the
[30-round ledger](../../results/measure/20261007-roadmap-rounds/SUMMARY.md);
previous handoff text is [archived](../archive/handoff-20261007.md).
Do not use a historical cluster address, binary hash or mount as current state.

## Active correctness work

Follow-up round 1 implements W82 fresh read rounds and correlated acknowledgements.
NUC real-daemon partition GETATTR acceptance passes; wider W82 RPC/configuration
gates remain open. Follow the [round ledger](roadmap-followup-20261007.md).

**W61/W62/W63 — GC:** repairs are committed in `3a1b4a52`; the
[implementation checkpoint](gc-implementation-20261007.md) records isolated
NUC direct-store acceptance. Buffered broad concurrent append lost records
(**W87**) despite later isolated repeats passing. Xorinox was rolled to `cb5e86de` on Oct 7 with safe drains and full units;
24.2 GiB payload reclamation and increased physical free space were observed.
The queues had not fully drained, xefs3 recorded one reap error, and lost-ledger
reconciliation remains open. The Oct 7 follow-up deploys the NUC service and passes full direct-I/O gates;
this does not establish final incident GC drainage. Preserve
live references and keep the original [incident review](gc-reclamation-review.md).
This round did not inspect the remote fixture logs.

**W86/D31 — PUT tickets:** checkpoint-recorded policy and metadata
state machine committed in `b3a11877` exist, staged for integration; versioned wire/PUT admission, actual W72 sessions,
distributed authoritative revocation and all-member storage fences/deletion
remain. D28 salvage is separate. Do not activate collection from age or a
local/cached fence.

**W43/D25 — staged, public logical truncate disabled.** Metadata histories,
committed readers, internal resize coordination, lane authority/bootstrap,
FileID-bound planning, durable publication, ordered ACK, I23 admission and
mtime coherence are implemented and tested in recorded checkpoints. The
[lane-local routing plan](d25-admission-routing.md) owns their detailed contract.
Remaining sequence:

1. Connect every FUSE write entry point and both flush paths to epoch-owned
   admission and typed publication/retirement. Replace unlabelled dirty unions;
   serialize pending publication and drain range exhaustion before copying.
2. Carry captured authority epochs for spans and unconditional overwrites.
   Fence races must retain bytes, return STALE and rebuild from authoritative views.
3. Add bounded live-file materialization scheduling, durable progress and safe
   lane/inode history retirement. Unlink sweep is a separate path.
4. Enable logical resize only after unpublished overlapping writes across
   shrink/extend, cold reads, restart, leader changes, history-full and bounded
   apply gates pass. A legacy cache hit cannot grant current authority.

**D27/0a — implemented runtime, acceptance incomplete.** Sticky per-inode and
per-description errors, retained snapshots, completed-cycle stall detection,
WITHHOLD faults and controlled stop exist. Strict whole-call timing, remaining
legacy PUT coverage, recorded fault/recovery/contention and small-host RSS
acceptance remain owed. Accepted workers retain caller-owned completion state;
a timeout must not free it. DNS resolution remains outside connection bounds.
D28 salvage is undecided. Keep the D27 decision's fault-before-publication gates.

**W64 — append:** host-local replay races were fixed in `ec1500ec`; replay
across leader changes is not durable merely because concurrent repeats pass.
The round-20 first-record-loss trace remains evidence, not a proven sole cause.

**W65 — shutdown:** nuc n1 exceeded the ten-second SIGTERM wait twice after
clean client drains and was SIGKILLed by the rollout tooling. Capture userspace
stacks during a controlled stop; do not label graceful daemon shutdown accepted.

## Additional public-contract findings

The [source review](spec-implementation.md) indexes W71–W77. In particular,
W71's production fragment ACK lacks a persistence barrier; a successful
healthy fsync/restart test does not establish power-loss durability. W75
records missing integrity evidence and the parallel-read verification bypass.
These are source findings, not a diagnosis of the GC incident or a new
production acceptance result. The queue and detailed gates govern follow-up.

## Verification still owed

- W54 post-GC cold-read and W38 traced/cold hardscan gates.
- W56 xorinox rollout verification; W60 full-tree mixed reads on xorinox.
- W67 sparse-pressure/failure/RSS and W68 real worker-retirement/RSS gates.
- Mac buffered acceptance after maintenance; real RDMA hardware acceptance.
- Repeat W36 rename-versus-unlink first on a new production rollout. The
  recorded NUC `128f6b7d` and xorinox `b4a75492` 20/20 runs establish those
  builds only; they do not automatically validate changed working sources.

Read the [queue](README.md#1a-the-work-queue) for remaining investigations,
measurement work and decisions. This handoff adds no deployment authorization.


## Operator review findings

[W78–W80](README.md#source-findings-awaiting-triage) record conditional stale
PID-file signaling, misleading mkfs name syntax and zero-filled `efs-query`
statistics. These are source findings with gates, not a new execution order.
Use the corrected operations guide; query zeros are not GC completion evidence.
W81 separately records the missing automatic directory pressure trigger; its
size-based admission/migrator exists and the pressure policy still needs review.


## Protocol review findings

[W82–W85](README.md#source-findings-awaiting-triage) cover stale completed read
coverage, missing transaction-decision retirement, namespace guard bounds
and eviction of ambiguous PUT retry history. W82 now has fresh-round repairs
and a NUC real-daemon partition GETATTR gate; broader RPC/configuration gates
remain owed. W85 has an isolated helper reproduction; live RPC/FUSE and
storage/accounting gates remain owed. W85's attempt-owned/locked placement-cache repair is now committed in
`6d6056c3`; the GC checkpoint reports helper/concurrency and two-root tests.
This review did not rerun those tests or establish release/restart acceptance. These findings do not assign a new work order or change accepted designs.

## Contract/evidence review

W23's first completed measurement exists; its zero-L0 interpretation is
invalid due to W89's TSV field shift. Corrected samples show follower L0=22,
but the pressure bound remains unproven. Current production apply defers flushing instead of waiting at the
L0 threshold; its log line alone is not a pump-stall measurement. The follower
memory/lag bound still needs a valid preconditioned run. W87 needs the full
traced buffered append outcome and retained failing evidence. See the
[round-6 ledger](../archive/contract-review-20261007-round6.md).

**W88 — D1 legacy span replay:** isolated metadata reproduction shows a
folded identity can be evicted and its old retry reintroduced over a later
base. Actual host/FUSE byte and lost-ACK/restart gates remain open. Keep the
durable identity/retirement primitives separate from bounded trailer hints.
