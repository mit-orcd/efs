# Agent handoff — AMD checkpoint awaiting authority clarification

[Status index](README.md) · [Decisions](decisions.md) ·
[Architecture](../how-it-works/architecture.md)

**Current state:** narrowed qualification RESUMED under explicit human start, reporting every15minutes. Common candidate52bcfd3570b0a59076c225caad9157b0825954d8 on qualification-v030-tcp-direct; AMD groups1/4, Intel group2, NUC group3. All received exactSHA. TCP/direct/files only; containers and dedicated repair/recovery deferred. No old-binary/data or backward-compatibility preservation effort required; identified EFS test stores may be erased if easier. Keep raw evidence and source identity; unrelated services untouched. No push/tag/promotion. [Scope and assignments](v030-release-scope-20261012.md).

## Superseded assignments and evidence (before human stop)

The following dated rollout/next-task statements are historical. They are not current deployment proof or authorization to resume.

**Historical ORCD assignment — Oct 11:** user authorizes source transfer, builds,
tests and coordinated deployment on ORCD until resolved. Work on `devel-orcd`;
W95/P4.1 production tiny-payload packing is first, followed by ORCD W109/W97,
W108, W110 and remaining W99/W42/W106 gates. The fcstor007 `tiny-files` screen
is the requested benchmark. Preserve the original acceptance oracles; the
user explicitly permits erasing ORCD test data if needed for packing rollout. [Implementation and measured storage checkpoint](orcd-tiny-packing-20261011.md).
Packed admission/session validation, actual packed PUT/GET, initial Raft
publication and durable receipts, versioned GETCHUNKS/client reads, snapshot
capability barriers and atomic reference retirement now pass private Linux
and four-server ORCD gates. Bounded immutable cohorts pass deadline/sync/capacity
and retry gates. Actual FUSE reads packed mappings and fsyncs mixed ordinary
files; mounted promotion/grow/fsync, truncate, unlink and hard-link survival
pass, including exact bytes after a cold client/all-server restart. Retired
upload permits and receipts survive leader loss and cold restart.
The 20-minute randomized storage soak passes 299,520 cohorts. [Integration evidence](../../results/measure/20261011-orcd-packed-integration/).
Next connect mounted writer snapshot/cache/close/fsync ownership to the cohort
scheduler, implement distributed shared GC and abandoned-cohort cancellation,
and finish mixed fault/recovery and matched throughput acceptance. Private
port 19832 is separate from the live RDMA runtime. No mounted writer selects
packing yet; lower-layer gates do not close W95/P4.1.
The live runtime is now `c52a4643` (`devel-orcd`), merged from all current
branches and deployed to four servers/seven clients with native RDMA/direct I/O.
Full Linux units and preserved file samples pass. Loaded 30-second POSIX gates
remain failed (1,475 pass, 23 fail, 7 skip, 42 NOTRUN), and virtual listings
cannot close W108 while query RPCs fail. The fresh 512-byte benchmark is running
in `/tmp/efs-mount/tiny-files-merged-c52a464391a6`; the original screen run was
interrupted cleanly at 9,514,331 files. [Rollout and evidence](orcd-merged-rollout-20261011.md).
Do not infer mounted packing activation from lower-layer unit gates.
The bounded query pause was restored to policy mask 3, version 2. The
older package instructions below are historical context for this assignment.

Reviewed Oct 8, 2026. Historical rollout facts are in the
[30-round ledger](../../results/measure/20261007-roadmap-rounds/SUMMARY.md);
previous handoff text is [archived](../archive/handoff-20261007.md).
Do not use a historical cluster address, binary hash or mount as current state.

## Historical next-package authorization

The user authorized replay/writeback correctness followed by durability and
production activation readiness while offline. Follow the
[Oct 8 implementation ledger](replay-readiness-20261008.md), with direct-I/O/TCP
for short functional cycles and a four-mode batch gate. Benchmark sources and
harnesses are outside this work. D28/D29/D30 were accepted Oct 8 in the [decision register](decisions.md).
D28 defers salvage; implement D29 with W88, then D30 admission/pacing, keeping
their implementation and fault/recovery gates separate from design acceptance.

## Accepted namespace and feature policies

[D32–D35](accepted-namespace-scratch-20261008.md) are accepted: W84's existing
144-participant product limit, W90 opt-in fixed expiry, W91 directory-local
commands, W81 sustained-contention spreading. Preserve all linked gates;
acceptance is not implementation or activation. These supersede older open-policy
wording without cancelling replay/readiness priorities.

## Historical correctness-work checkpoint

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
a timeout must not free it. DNS resolution now has bounded ownership/deadlines in `011ae854`; the
[readiness ledger](replay-readiness-20261008.md) records its tests.
D28 accepts client-death loss of unpublished bytes and defers automatic salvage. Keep the D27 decision's fault-before-publication gates.

**W64 — append:** host-local replay races were fixed in `ec1500ec`; replay
across leader changes is not durable merely because concurrent repeats pass.
The round-20 first-record-loss trace remains evidence, not a proven sole cause.
Oct 8 hardening compares the complete UUID, session epoch and reservation length,
requires a fresh authoritative read before a cache replay, and refuses cursor overflow.
AMD direct-I/O/TCP passes full units, POSIX 216/0/1 skip, peer 64/64 and cold
durability 26/26 in both phases. These checks do not close durable replay.
A real 32-request identical-reserve gate then reproduced duplicate allocation
(offset 8 instead of 0). Per-inode nonblocking reservation ownership fixes that
race; the repaired gate checks exact offsets and cold extent. W87's original
NUC fixture contains missing W1S0002/W0S0004 records, beyond first-record loss.
AMD buffered broad POSIX/peer and mixed append/GC with 800 exact cold records
pass; this is acceptance evidence, not a proven single cause for W87.

**Oct 8 AMD package:** [evidence](amd-functional-gates-20261008.md).
Functional cycles use direct I/O/TCP; only transport/buffered-specific gates
change modes. [W84 is closed for accepted D32 bounded scope](w84-closure-20261009.md):
explicit prompt EOVERFLOW, atomic refusal and exact144 coordinator-crash gates
pass. Larger-operation protocol remains deferred. RDMA eager send-slot registration caused
a 1 GiB whole-fixture OOM. Lazy registration keeps the 32-slot pipeline and passes
the same 1 GiB recovery gate with zero OOM events; the allocation budgets did not
increase. No benchmark source or harness was changed.

**W65 — shutdown:** the historical NUC SIGTERM failures are superseded for TCP
by the [six active-write stop/recovery gates](tcp-five-gates-20261007.md).
AMD RXE active-write, idle/partial TCP reader and connection-churn stop gates
now pass with exact acknowledged bytes after cold restart. Hardware RoCE/native IB
remain separate platform obligations.

## Additional public-contract findings

The [source review](spec-implementation.md) indexes W71–W77. In particular,
W71 now has production file/pathname barriers in `e77e1f13`; fault and live
checks pass, but physical power-loss durability remains an acceptance obligation. W75
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

[W78–W80](devel-nuc-closure-20261010.md) close on Oct11 current-code NUC
operator acceptance: actual stale-PID startup/stop, both mkfs aliases and
consumed-reply retry, and explicit retired-query refusal without false totals.
W70's running-quota reporting and W96's isolated allocation/recovery gates also
close in that report. W97 remains open; neither raw drive controls nor these
operator gates establish sustained EFS throughput or a hardware ceiling.
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

W23's historical first measurement had an invalid zero-L0 interpretation due
to W89's TSV field shift. **Oct 10/11 update:** W89's measurement defect is
closed by strict raw/TSV identity checks and a fresh AMD real 1 GiB follower-cap,
quorum/recovery and cold-value gate (276 complete observations). The malformed
Oct 5 evidence and withdrawn interpretation remain retained. D30/W23's
production memory/lag and sustained-stall bounds remain open; measurement
closure does not activate that policy. See the
[current AMD checkpoint](amd-non-rdma-20261010.md#w89) and historical
[round-6 ledger](../archive/contract-review-20261007-round6.md).
W87 still needs its original broad-load failure investigated rather than
being inferred closed from clean repeats.

**W88 — D1 legacy span replay:** isolated metadata reproduction shows a
folded identity can be evicted and its old retry reintroduced over a later
base. Actual host/FUSE byte and lost-ACK/restart gates remain open. Keep the
durable identity/retirement primitives separate from bounded trailer hints.

Human-authorized three-way qualification split: AMD remaining direct/overload/RXE, Intel buffered first-stage matrix, NUC additional recovery/query and storage-engine matrix. All use release-v0.3.0-pre-alpha source `28b2089be6a5df835cc7bb46b9c2a9e2f10df190`; see [current execution ledger](amd-all-suites-20261011.md#authorized-three-cluster-split--october-11-2026). Other owner stops remain effective. No source fixes, promotion or pushes authorized by this testing split.

Subsequent human authority permits packing-timeout and repair-gap implementation fixes on separate frozen28-based worktrees, review/integration into release-v0.3.0-pre-alpha and affected reruns. Intel owns packing, NUC repair, AMD integration. Earlier testing-only prohibition is superseded for these fixes; no push/tag/promotion authorization.

**Latest stop checkpoint:** automatic approval review reports a newer all-chat document/commit/stop instruction and rejected further Intel continuation coordination. AMD has no owned screens/test processes and both original mounts are restored. Intel is stopping; NUC notified to stop launches, restore and checkpoint. Packing/repair fixes and affected fixed-source reruns remain incomplete. Do not restart automatically.


**Latest authority — October 12:** human explicitly replied “Resume fixes and affected testing.” This supersedes the preceding stop for coordinated fixes and affected reruns. Both owners acknowledged. Intel investigates completed-read-round starvation; NUC submitted corrected inactive journal checkpoint `8af41a1e`, pending AMD review. Actual repaired-fragment/cold/second-loss proof remains required.


**Subsequent authority conflict — October12:** Intel reports a newer direct stop answer; root requested whether it applies here and paused launches pending clarification. This checkpoint supersedes the preceding running task description, not original immutable verdicts.


**Parallel qualification assignment (documented, not launched):** AMD groups1/4
(read-progress safety and final native qualification), Intel group2 (direct TCP
packing/query), NUC group3 (direct TCP files POSIX/peer/mutation/GC). All agents
start from full baseline8b7391b3f3edcf9fc99440d9aad3c240325acb83 and must use the
same centrally published candidate SHA, including any intended read-progress
fix, before execution. [Freeze/dependency contract](v030-release-scope-20261012.md#common-source-and-parallel-execution-contract).


[Current common52 group results and next blocker](v030-tcp-direct-qualification-20261012.md): Intel quietfailure remains open; collect exact raw diagnostic evidence, review NUC passes; AMD normal/faultnative+RaftsanPASS.
