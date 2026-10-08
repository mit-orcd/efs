# Project status — current work and index

[Architecture](../how-it-works/architecture.md) · [Current handoff](in-flight.md) ·
[Decisions](decisions.md) · [Work-item details](../backlog/work-items.md) ·
[Operations](../operations/operations.md) · [Archive](../archive/README.md)

## 1. The task right now

The [memory acceptance follow-up](memory-closure-20261007.md) closes W59/W67.
The [three-package closure follow-up](allocation-gc-fold-gates-20261007.md) closes D17.
Current grouped open count: **45**, including **9** implemented/partial
packages awaiting gates. W84 and W23 still have implementation/design gaps.

The [three-package acceptance round](three-package-gates-20261007.md) advances
W59/W67, W84 and W23/W89 with pressure, boundary and recovery fixes. That earlier pass left
all three open at 47 packages; the memory follow-up above supersedes its count.

The [next-five acceptance pass](next-five-gates-20261007.md) closes W27, W76,
W82 and W85. W69 correctness passes; its unidentified Spark performance
measurement remains open. Its 47-package count is historical.

The requested 30-round continuation is complete; see the
[continuation ledger](roadmap-30-continuation-20261007.md) for committed fixes,
NUC acceptance and retained failures.

Reviewed Oct 7, 2026, final round, from HEAD `b3a11877` through the
`75f6a42e` GC checkpoint / `7582df59` generated-artifact update and existing
working changes. Earlier round ledgers retain their own baselines. This is a documentation review; recorded deployments are evidence
from their dated runs, not a live cluster inventory.

- **W61/W62/W63 — GC reclamation:** repairs are committed in `3a1b4a52`;
  the [GC checkpoint](gc-implementation-20261007.md) records private-store
  direct acceptance and a buffered append failure (**W87**). The xorinox incident cluster was rolled to `cb5e86de` on Oct 7 with safe
  client drains and full units; a captured sample shows 24.2 GiB payload
  reclaimed and physical free space rising to 9.8–10.8 GB/node. Queues were
  still draining and xefs3 had one reap error. The Oct 7 follow-up rolls the NUC service through full direct-I/O gates;
  final incident drain and authority-safe lost-ledger reconciliation remain open.
- **W86/D31 — durable PUT tickets:** narrow abandoned-upload policy is recorded
  in that checkpoint; metadata state machine is committed in `b3a11877` and staged for integration. Production sessions,
  versioned PUT/publication and all-member revocation/deletion are not active.
- **W43/D25 — writer integration:** lane-local authority, typed publication,
  ordered ACK and session gates are staged. Public logical truncate is still
  disabled. Follow [the handoff](in-flight.md) for the remaining activation gates.
- **D27/0a — writeback recovery:** runtime ownership/error/stop foundations
  are implemented; strict whole-call timing, fault recovery and RSS gates remain
  open. DNS resolution and accepted-job lifetime still limit deadline claims.
- **W64/W65:** durable append failover replay and slow daemon shutdown are
  unresolved findings from the [30-round ledger](../../results/measure/20261007-roadmap-rounds/SUMMARY.md).

Latest continuation service acceptance: isolated `24fae5c1`, direct I/O, full
Linux units, POSIX 216/0/1 skip, peer 64/64, persistence 26/26 in each phase.
Logs and partial/failure evidence are recorded in the continuation ledger.

Earlier NUC follow-up acceptance: isolated `5ee9ab6b` + W82 round changes,
direct I/O, units, POSIX 216/0/1 skip, peer POSIX 64/64 and persistence
26/26 in both phases. See the [ledger](roadmap-followup-20261007.md).

Earlier recorded NUC production acceptance: `128f6b7d`, direct I/O, full rebuilt
Linux units, W36 20/20, single-client POSIX 216 pass/0 fail/one unsupported mmap
skip, peer POSIX 64/64, persistence 26/26 in each phase and ten concurrent
8/8 repeats. This does not close D25, D27, GC reconciliation, append failover
or shutdown. Mac buffered acceptance remains deferred. See the ledger for raw
logs and build identity; verify current processes before using any cluster.

### 1a. The work queue

W numbers identify bugs, features, enhancements and investigations; queue
positions express order and need not match the W number. D numbers identify
product/architecture decisions, with status in the decision register.
P rows identify the existing measurement plan. Correctness precedes
performance; do not infer a new priority order from a larger W number.
User-directed work overrides the historical queue order.

Read this index, then the selected item's detail and gate. Keep one canonical
row per item. Code present, local test passed, cluster validated and closed are
separate states. Close only when the item's named acceptance gates are recorded.

| # | item | class | status | home |
| --- | --- | --- | --- | --- |
| 0i | **W54** · a fold's GC deletes the live base → read EIO, data loss | correctness | NUC direct/buffered authoritative folds and cold GC/restart pass; real cold IOR/hardscan passes at 4 and 36 ranks, including nine independent NUC FUSE clients; nine physical hosts/RDMA remain | [full text](../backlog/work-items.md#w54--a-folds-gc-deletes-the-live-base-queue-row-0i) |
| 0j | **W69** · the client's 50 ms lookup memo returns pre-mutation stats (posix 164/201) | correctness | correctness accepted: real blocked LOOKUP across chmod, immediate stats and full POSIX pass; GNU du samples recorded; unidentified Spark performance gate remains | [full text](../backlog/work-items.md#w69) |
| 0k | **W55** · span committed to raft, fragment PUTs never landed → read EIO, data loss | correctness | open — found Oct 5 on the xorinox test cluster (write via the nfsd re-export from a macOS client); trigger not isolated | [full text](../backlog/work-items.md#w55--span-committed-to-raft-fragment-puts-never-landed--read-eio-data-loss-queue-row-0k) |
| 0l | **W56** · root-level rename leaves a ghost name in the renaming client's local lookup | correctness | fixed in `f8fef814` — exact old directory-name cache eviction across parent/hash tabs; NUC full POSIX jobs=4/jobs=1 and two-client suites PASS Oct 6; xorinox roll still owed | [full text](../backlog/work-items.md#w56--root-level-rename-leaves-a-ghost-name-in-the-renaming-clients-local-lookup-queue-row-0l) |
| mem1 | **W67** · sparse dirty writes and bounded body admission | correctness / resource exhaustion | Closed: NUC live publication failure plus real recovery scratch exhaustion passes at 32+32 and default 256+64 MiB budgets under a 1 GiB/no-swap ceiling; same-mount/cold recovery and physical GC/restart pass | [evidence, fix shape and gates](fuse-memory.md#sparse-dirty-writes-bypass-reclaim-and-cache-admission-has-no-hard-bound) |
| mem2 | **W68** · reply-buffer worker lifetime | resource lifetime | Closed: NUC direct/buffered real worker retirement, allocation/TLS/growth fault RSS and key-failure gates pass | [evidence, fix shape and gates](fuse-memory.md#readreaddir-reply-buffers-leak-when-fuse-workers-exit) |
| 0o | **W58** · open(O_EXCL) create answered EEXIST for a name the same client's own create just landed | correctness | analyzed Oct 6 (xorinox, build `3d3f17c2-dirty`): the file exists (created 05:17:31.319Z, size 0), the retry was answered BUSY (rc=-13, 05:17:31.733Z), no server logged EEXIST; suspect the retry path — an opid replay must return the recorded verdict (I16), not EEXIST; BUSY on unique-name creates is new with the dirty D25 intent probes | [full text](../backlog/work-items.md#w58--openo_excl-create-answered-eexist-for-a-name-the-same-clients-own-create-just-landed-queue-row-0o) |
| 0p | **W59** · write(2) via FUSE fails ENOSPC with 156 GiB free — client cache-admission mapped to ENOSPC; the 8 MiB metadata budget never drains | correctness | Closed: NUC live publication failure plus real recovery scratch exhaustion passes at 32+32 and default 256+64 MiB budgets under a 1 GiB/no-swap ceiling; same-mount/cold recovery and physical GC/restart pass | [full text](../backlog/work-items.md#w59--write2-via-fuse-fails-enospc-with-156-gib-free--client-cache-admission-mapped-to-enospc-the-8-mib-metadata-budget-never-drains-queue-row-0p) |
| 0q | **W60** · sequential prefetch starves tiny demand reads | correctness | fixed in `73ce8aaa`; NUC 32 MiB A/B: baseline 2000/2000 failures, fixed 0/2000 and no read-NOMEM; Xorinox full-tree gate owed | [evidence and remaining gates](fuse-memory.md) |
| 0r | **W61** · local GC discards failed lane-sweep verdicts | correctness | repaired in `3a1b4a52`; private-store results recorded in GC checkpoint; xorinox rollout/reclamation observed Oct 7 (`cb5e86de`); final drain, NUC service rollout and lost-ledger reconciliation remain open | [full text](../backlog/work-items.md#w61) |
| 0s | **W62** · sweep/truncate batch boundaries lose delta GC records | correctness | repaired in `3a1b4a52`; private-store results recorded in GC checkpoint; xorinox rollout/reclamation observed Oct 7 (`cb5e86de`); final drain, NUC service rollout and lost-ledger reconciliation remain open | [full text](../backlog/work-items.md#w62) |
| 0t | **W64** · durable append reservation replay across leader changes | correctness / recovery | open — host-local replay race fixed in `ec1500ec`; durable failover replay remains unresolved | [work item](../backlog/work-items.md#w64) |
| 0u | **W65** · daemon exceeds graceful shutdown wait | liveness investigation | idle-reader/accept blockers repaired; NUC TCP shutdown and service persistence gates pass; RDMA/fault-time mutation gates remain | [work item](../backlog/work-items.md#w65) |
| gc-obs | **W63** · GC backlog, reclaimed bytes and retry progress are missing | observability enhancement | repaired in `3a1b4a52`; private-store results recorded in GC checkpoint; xorinox rollout/reclamation observed Oct 7 (`cb5e86de`); final drain, NUC service rollout and lost-ledger reconciliation remain open | [full text](../backlog/work-items.md#w63) |
| 0e | **W38** · ior-hard fold tombstone without the span's bytes | correctness | code present; dated Oct 5 checkpoint — replay preserves live spans; folds require byte observations; deterministic regression + ASan/UBSan pass; traced real IOR-hard + independent cold hardscan pass (108000 records, bad=0, short=0); nine physical hosts/RDMA remain | [full text](../backlog/work-items.md#w38--ior-hard-fold-tombstone-without-the-spans-bytes-queue-row-0e) |
| 2a | **W42** · `df` / `efs-mgmt status` report the 3-node capacity model on any node count | correctness | capacity helper is used by both clients; NUC four-node status/df record 500 GiB for 4 × 187.5 GiB quotas; NUC limited-member write/cold-read/reclaim gate passes; protection debt/repair and named 19810 capacity acceptance remain open | [full text](../backlog/work-items.md#w42--df--efs-mgmt-status-report-the-3-node-capacity-model-on-any-node-count-queue-row-2a) |
| 0b | **W27** · REPORT identity from the staging table | correctness | closed: owned-PUT predicate shared by both builders; nonzero unowned observations rejected; 2048-file cold copy/trace and GC/restart pass | [full text](../backlog/work-items.md#w27--report-identity-from-the-staging-table-queue-row-0b) |
| 0g | **W43** · large truncate is refused; logical truncate = **D25** (decided) | correctness | explicit failure is implemented; `3a1b4a52` makes oversized legacy truncate fail atomically; D25 metadata/read/resize foundations exist, but FUSE publication, live-file sweep and public activation remain open | [full text](../backlog/work-items.md#w43--truncateo_trunc-of-a-file-with--32-chunks-in-a-lane-is-a-silent-no-op-queue-row-0g) · [D25](decisions.md) |
| 0a | STALE replay that never converges; remedy = **D27** (decided) | correctness | runtime foundations present; strict timing, fault/recovery/contention and RSS gates remain in handoff | [full text](../backlog/work-items.md#0a--stale-replay-that-never-converges-queue-row-0a) · [D27](decisions.md) |
| 0h | **W44** · the leader's GC frag pass scans the whole prefix every 1.2 s; remedy = **D26** | performance | D26 in tree + gated (dev cluster, Oct 4): watermark gates the scan, cursor bounds the pass; idle leaders logged no `gc-pass` line for 10 min; a 5120-record `rm` drained at ~514 records/pass; Oct 7 10-GiB physical reclamation passes; retained-table active GC latency regression remains (1393-ms pass); idle-hour/committed-tail results in the acceptance ledger | [full text](../backlog/work-items.md#w44--the-group-leaders-gc-frag-pass-scans-the-whole-prefix-every-12-s-queue-row-0h) · [D26](decisions.md) |
| 12 | **W48** · four of the 16 dd streams ended early | investigation | investigate — evidence only | [full text](../backlog/work-items.md#w48--four-of-the-16-dd-streams-ended-early--investigate-plan-row-12) |
| 15 | **W52** · a REPORT after thousands of O_APPEND writes answers after > 30 s | correctness | approved shape A, not implemented: current host_resolve_caught_up still proposes each reservation serially; batch implementation and append_gate owed | [full text](../backlog/work-items.md#w52--a-report-after-thousands-of-o_append-writes-answers-after--30-s-plan-row-15) |
| 16 | **W53** · W41's create/close-storm p99 regression — keep or revert | investigation | investigate — evidence only, then the user decides | [full text](../backlog/work-items.md#w53--w41s-createclose-storm-tail-under-concurrent-big-writers--investigate-plan-row-16) |
| E | **D17** · `st_blocks` = 0 for files this client did not write | performance | in tree + gated (dev cluster, Oct 4): lane-stamp present count, summed at getattr, client takes max with its local table; Closed: NUC direct/buffered actual ecrawl reports zero dense-file false sparse classifications, two true sparse files and zero errors; cold non-writer partial-tail/truncate/empty allocation gates pass | [D17](decisions.md) |
| P2.2 | **D26** · the GC pass | performance | in tree + gated (dev cluster, Oct 4): per-anchor pending-GC watermark maintained in the apply, derived once per recovery/import, `zero_if` clamp on a drained pass; `test_gc_watermark`; live: 514 records/pass drain, 10 idle min with no `gc-pass` line; Oct 7 10-GiB physical reclamation passes; retained-table active GC latency regression remains (1393-ms pass); idle-hour/committed-tail results in the acceptance ledger | [full text](../backlog/work-items.md#p22--d26--the-gc-pass-performance-plan-row) · [D26](decisions.md) |
| P2.3 | **W23** · stalled-compactor test | performance | strict W89 samples pass; NUC engine reaches production 1 GiB L0 cap; deferred idle drain and unapplied WAL-tail durability repaired; universal follower memory/lag bound remains open | [full text](../backlog/work-items.md#p23--w23--the-stalled-compactor-test-performance-plan-row) · [run](../../results/measure/20261005-040810-w23-stalled-compactor/SUMMARY.txt) |
| P3 | `efs-bench --bench data/meta`, then `efs-fuse --bench` | performance | tools implemented; Oct 7 corrected histogram/window coverage and metadata result validation are recorded in the latency/benchmark checkpoints; named six-NVMe fcstor storage curve and two-host client ladder remain owed | [server plan](../backlog/work-items.md#single-node-storage-bench-efsd---bench--asked-oct-2-2026-user-queue-position-after-w41--d23--d17--d26-in-plan-after-the-oct-1-2200z-review-its-number-decides-the-fragment-layout-w40-and-zero-copy-receive) · [client plan](../backlog/work-items.md#client-bench-efs-fuse---bench--asked-oct-2-2026-user-after-efsd---bench) · [original run](../../results/measure/20261005-045140-p3-benches/SUMMARY.txt) · [latency validation](../../results/measure/20261007-latency-validation/SUMMARY.md) · [benchmark validation](../../results/measure/20261007-bench-hot-path-review/SUMMARY.md) |
| P4.1–P4.4 | fragment on-disk layout (**wipe**), W40 FUSE write copy, RDMA zero-copy receive, 9-client scaling | performance | deferred until P3's numbers | [full text](../backlog/work-items.md#p4--deferred-until-p3s-numbers-long-one-is-a-wipe) |
| meta-scale | **W66** · metadata leadership distribution and topology-independent routing | scalability | open — Oct 5 implementation gap recorded; correctness work first, then baseline measurement and staged multi-Raft implementation | [evidence, phases and gates](metadata-scaling.md) |
| — | **D28** (who owns acknowledged bytes across client death) · **D29** (a REPORT receipt for "committed, apply pending") · **D30** (the remedy for compaction-induced apply lag) | ask | ask — not code until the user decides | [decisions.md](decisions.md) |
| op1 | **W70** · shrink-quota falsely reports background migration | operator observability bug | truthful running-quota reporting implemented; NUC accepted/refused shrink and unchanged-byte gates pass; no migration or persistence claim | [work item](../backlog/work-items.md#w70) |

### Source findings awaiting triage

These review findings have canonical identities and gates below; their
positions do not assign an execution order or override the active handoff.

| review ID | finding | classification | review state | evidence / gate |
| --- | --- | --- | --- | --- |
| spec1 | **W71** · fragment PUT lacks a production persistence barrier | durability correctness | open — fsync publication does not issue a separate target flush; no power-loss gate established | [evidence and gate](../backlog/work-items.md#w71) |
| spec2 | **W72** · production mount session lifecycle / I23 integration | fencing integration | open — metadata primitives exist; mounted HOLD/FLOCK and session lifecycle remain legacy | [evidence and gate](../backlog/work-items.md#w72) |
| spec3 | **W73** · synchronous application-write publication | integration | no explicit O_SYNC/O_DSYNC handling; real kernel/FUSE sequencing must be verified | [evidence and gate](../backlog/work-items.md#w73) |
| spec4 | **W74** · protection profiles, debt and automatic repair | recovery feature gap | fixed 2+1; no automatic repair/profile cutover; accepted design exceeds current capability | [evidence and gate](../backlog/work-items.md#w74) |
| spec5 | **W75** · fragment integrity trust / missing checksum fallback | integrity correctness | missing/corrupt evidence fails closed; captured metadata digest and PUT validation gates pass on NUC; identity-bound format remains open | [evidence and gate](../backlog/work-items.md#w75) |
| spec6 | **W76** · opid table exhaustion sends unprotected namespace retries | conditional correctness | Closed: 512 concurrent identity owners refuse saturation safely; all four lost-reply mutations replay across leader death/restart with one effect | [evidence and gate](../backlog/work-items.md#w76) |
| spec7 | **W77** · atomic multi-chunk public publication/observation | integration gap | per-group REPORT/per-chunk publication; request-wide decision/read-validation gate unestablished | [evidence and gate](../backlog/work-items.md#w77) |
| op2 | **W78** · server wrapper trusts PID-file process identity | conditional operator correctness | identity-checked pidfd daemon/recorder retirement implemented; NUC ownership fixtures pass; daemon shutdown remains W65 | [evidence and gate](../backlog/work-items.md#w78) |
| op3 | **W79** · mkfs help/name argument disagrees with single-export handler | operator observability | legacy label explicitly reported as ignored; extra arguments rejected; NUC repeated mkfs preserves salt/namespace | [evidence and gate](../backlog/work-items.md#w79) |
| op4 | **W80** · efs-query reports retired zero-filled statistics as totals | operator observability | unsupported status and CLI nonzero exit replace invented zero totals; NUC RPC/CLI gates pass; actual totals remain unimplemented | [evidence and gate](../backlog/work-items.md#w80) |
| scale1 | **W81** · automatic directory spreading lacks a pressure trigger | scalability feature gap | size trigger/migrator exist; pressure policy remains unspecified and unwired | [evidence and gate](../backlog/work-items.md#w81) |
| proto1 | **W82** · stale completed ReadIndex authority survives leader isolation | read correctness | Closed: stale LOOKUP/GETATTR/session/publication/transaction views and configuration partition gates pass; coordinator callback refuses untrusted absence | [evidence and gate](../backlog/work-items.md#w82) |
| proto2 | **W83** · transaction decision records lack safe retirement | metadata lifecycle gap | resolve/drop remove participant records; no decision acknowledgement/GC path found | [evidence and gate](../backlog/work-items.md#w83) |
| proto3 | **W84** · eight-record namespace bounds reject deep/spread work | namespace completeness | 64-participant envelope and cross-group HASHED unlink repair; NUC boundary/refusal/restart gates and exact helper limits pass; shared 8-second rename budget; 144-participant envelope and full 64-lane removal/replacement implemented; 64-hop ancestry limit and boundary leader-failure acceptance remain open | [evidence and gate](../backlog/work-items.md#w84) |
| storage1 | **W85** · path-hint eviction loses ambiguous PUT retry history | conditional accounting/storage correctness | Closed: ambiguous PUT placement retained in 389a8baf; direct/buffered real lost-ACK collision and two-root physical/quota/restart gates pass | [evidence and gate](../backlog/work-items.md#w85) |
| gc-own | **W86** · durable PUT ticket integration | feature / safe reclamation | metadata state machine committed `b3a11877`, staged for integration; production admission, sessions and all-member collection not active | [evidence and gate](../backlog/work-items.md#w86) · [D31](decisions.md#d31--recorded-abandoned-upload-policy) |
| append-load | **W87** · buffered concurrent append loses records under load | correctness investigation | failure recorded in private GC fixture; isolated repeats pass; full traced acceptance owed | [evidence and gate](../backlog/work-items.md#w87) |
| replay-span | **W88** · folded span retry resurrects after bounded history eviction | publication/retry correctness | isolated metadata reproduction; actual host/FUSE byte gate owed | [evidence and gate](../backlog/work-items.md#w88) |
| measure-fields | **W89** · W23 TSV field packing shifts derived metrics | verification correctness | TSV schema/reduction repaired; NUC regressions pass; new pressure run and valid bound remain owed | [evidence and gate](../backlog/work-items.md#w89) |

The [specification/public-path review](spec-implementation.md) separates accepted
contracts from current capabilities (W71–W77). These findings add no deployment
acceptance or new priority order.

## Maintaining this index

Every new active finding must receive an unused W number and a row here,
with its class, current state, evidence, next action and acceptance gate.
A decision belongs in the D register; "decided" means an approved design,
not an implementation or a passing gate. Existing identifiers are never reused.
Number allocation must check the active queue, decision register and work-item
index for collisions. Cross-link supporting status pages and long-form work items.

Archive superseded handoffs and dated implementation narratives intact; update
links to their historical home. Keep unresolved gates in this queue even when
most implementation is done. Preserve historical evidence without letting its
old "next" instructions or cluster claims govern current work.

W57's NUC implementation/gates are complete; its detail remains in the work-item
ledger, and the previous status row is retained in the archive. W56 and W60
remain here only for their named xorinox verification obligations.

## Latest queue reconciliation

The [final review ledger](../archive/final-documentation-review-20261007.md)
records the completed active-documentation review, final corrections and
verification scope. Earlier round evidence is indexed in the
[archive](../archive/README.md). Open implementation and acceptance gates remain
in this queue; historical build acceptance does not validate a new rollout.

## Historical checkpoints

The former 966-line running page is retained in
[Oct 5–7 status checkpoints](../archive/status-checkpoints-20261007.md).
Do not use its old "uncommitted", "not wired", "next item" or deployment
statements as current status. Completed writer/benchmark investigations and
superseded D25/D27 rollout narratives live there; their result logs remain linked.


## GC implementation checkpoint — Oct 7

The [GC checkpoint](gc-implementation-20261007.md) records production repairs,
NUC physical deletion/restart gates, and the approved discard-after-durable-
revocation policy. PUT ticket metadata is committed in `b3a11877` and staged for integration; production session, wire and
storage-fence integration remains open. Xorinox rollout and substantial reclamation are confirmed in the checkpoint; complete backlog drainage and historical lost-ledger reconciliation are not.

### Implemented-fix gate rounds (Oct 7)

[Thirty-round ledger](gate-30-rounds-20261007.md): NUC gate results, additional fault/restart/configuration evidence and remaining obligations. W68 closes; other packages retain their explicit platform, scale or durability gates.

### Next five implemented-fix gates (Oct 7)

[Follow-up ledger](next-five-gates-20261007.md): W27, W76, W82 and W85 close
after targeted NUC acceptance; W69 correctness passes, with its unidentified
Spark performance measurement still owed. Grouped open counts are now
15 unfinished, 11 awaiting gates, three decisions and 18 future features:
**47 open packages**. Earlier tables remain dated baselines.
