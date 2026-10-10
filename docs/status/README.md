# Project status — current work and index

[Architecture](../how-it-works/architecture.md) · [Current handoff](in-flight.md) ·
[Decisions](decisions.md) · [Work-item details](../backlog/work-items.md) ·
[Operations](../operations/operations.md) · [Archive](../archive/README.md)

## Governing node-count contract — D36

[User-ratified D36](node-count-protection-20261009.md): three nodes minimum,
four nodes maximum two-loss safety, additional nodes for capacity and performance
across all dimensions. This supersedes the earlier protection ladder. Current
fixed 2+1 and three-voter metadata groups do not satisfy the four-node target;
W74/W66 must close the gap. Never infer the accepted design from current code.

## Accepted design follow-up

[D32–D35](accepted-namespace-scratch-20261008.md) settle W84's current transaction
limit, W90 expiry policy, W91 `.efs/` commands and W81 contention-triggered spread.
[W84 bounded namespace acceptance](w84-closure-20261009.md) is closed;
other implementation and gates remain open. No fixed directory depth or
universal scratch TTL is selected. D28–D30 are also accepted. The zero decision count
refers to settled registered asks, not to every remaining implementation detail.

## Command options and expiry coverage

[W91/W92 options and W90 test status](efs-command-options-20261008.md) records
long listings, dates, sorting and server-side predicates. Expiry remains
unimplemented; unavailable/refusal tests are not lifecycle acceptance.

## Tiny-file performance follow-up

[W93–W96: NUC profiling evidence and next experiments](tiny-file-performance-plan-20261008.md)
register broader Raft group commit, atomic CREATE/open lease, tiny-file data
persistence/packing, and KV WAL preallocation. All four are open; W93/W94 are
the recommended first priorities within this set. W95 refines existing P4.1,
so the earlier grouped count below is retained as a dated baseline rather than
recomputed by blindly adding four. W90–W92 keep their existing scopes.

## Disk-backed query cache progress

[W92 local disk backend](query-disk-backend-20261009.md) implements persistent
partition catalogs, bounded builder scratch, immutable snapshots and validated
checkpoint recovery. Functional gates and clean Xorinox mutation queries pass after the authorized
wipe; the earlier wrong-shard artifact remains historical evidence. Per-generation
logical disk budgeting is implemented. Incremental derived stats, aggregate cache
quotas and large-scale acceptance remain open.

## Distributed metadata scaling discussion

[W98 — from fixed placement to distributed metadata capacity](all-node-metadata-queries-20261009.md)
records the move to locality-aware sharding and bounded replication: additional
nodes contribute ordinary POSIX metadata and distributed query capacity without
cluster-wide coordination for routine updates. W66 owns authoritative placement,
routing and migration; W92 remains the local disk backend. W98 owns the scaling
discussion and distributed query design. This item is outside the dated grouped
count below.

## Storage pressure policy

[W99 — proactive capacity protection](storage-pressure-read-only-20261009.md)
records the user's requirement for every disk writer and shared/separate data,
metadata and cache storage. Read-only admission must precede exhaustion and
reflect placement/quorum availability and preserve authoritative metadata. The
note explicitly corrects the assumed any-two-of-four guarantee. Thresholds and escalation details remain
open; this new item is outside the dated grouped count below.

## 1. The task right now

[AMD W73/W64 RXE completion](amd-rxe-gate-closure-20261009.md): integrated
source passes direct/buffered RXE core POSIX, peer, mount-sync, replay/recovery
and exact cold append/GC. Accepted binaries are installed direct/RXE with
runtime verbs proof. W72 and hardware durability remain separate open work.

[AMD functional follow-up](amd-functional-gates-20261008.md): append retry ownership,
RXE memory/shutdown, exact namespace crash boundaries and cold physical GC.
Short functional cycles default to direct-I/O/TCP. Remaining durable append,
full-ancestry and physical-host obligations remain open.

[Version0.2.0 AMD release acceptance](v020-amd-release.md): devel-roce-rdma merged into devel; full POSIX, peer and cold durability suites pass for direct/buffered × TCP/RXE RDMA. Hardware RoCE/native-IB and broader roadmap obligations are not closed by this release.

The [memory acceptance follow-up](memory-closure-20261007.md) closes W59/W67.
The [three-package closure follow-up](allocation-gc-fold-gates-20261007.md) closes D17.
Grouped open count before W93–W96 registration: **45**: **15** unfinished correctness/investigation
packages, **10** implemented/partial awaiting gates, **0** unresolved D28–D30
decisions and **20** future feature/scaling/performance packages. This Oct 8
reconciliation moves W71/W73 to awaiting gates, adds W90/W91, settles D28 policy
and carries D29/D30 into unfinished implementation; accepted designs are not
closed implementation packages. W84 and W23 still have implementation/design gaps.
The [five TCP package round](tcp-five-gates-20261007.md) closes W65 for supported TCP and W56 inside W56/W60. RDMA is deferred; nine physical hosts, W60 historical scale and W84 full-ancestry acceptance remain.

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
  in that checkpoint; metadata state machine and replicated admission/status transport are implemented
  (`b3a11877`, `0aaf7d32`) and AMD direct/TCP gated, including cold replay. Production data-plane
  PUT fencing and all-member revocation/deletion are not active.
- **W43/D25 — writer integration:** lane-local authority, typed publication,
  ordered ACK and session gates are staged. Public logical truncate is still
  disabled. Follow [the handoff](in-flight.md) for the remaining activation gates.
- **D27/0a — writeback recovery:** runtime ownership/error/stop foundations
  are implemented; strict whole-call timing, fault recovery and RSS gates remain
  open. Bounded DNS ownership is implemented; accepted-job lifetime and live
  fault/recovery acceptance still limit whole-write deadline claims.
- **W64:** durable receipts, coordinated EOF/reservation/completion and bounded
  DONE cleanup pass AMD direct-I/O/TCP replay, leader-death, append/fsync,
  peer-extension and cold-byte gates. Production session fencing and broader
  failover acceptance remain open; see the [readiness ledger](replay-readiness-20261008.md).
- **W73:** O_SYNC/O_DSYNC callbacks explicitly publish before replying; small,
  queued and cross-lane append cases pass before close and after crash/restart.
  Mount-sync and final buffered/RXE acceptance remain owed.
- **W65:** supported TCP shutdown acceptance is recorded in the five-package
  round; AMD RXE shutdown acceptance is being exercised separately.

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
| 0i | **W54** · a fold's GC deletes the live base → read EIO, data loss | correctness | NUC direct/buffered authoritative folds and cold GC/restart pass; real cold IOR/hardscan passes at 4 and 36 ranks, including nine independent NUC FUSE clients; nine physical hosts remain; AMD software-RXE fold/GC gate passes, hardware RDMA remains | [full text](../backlog/work-items.md#w54--a-folds-gc-deletes-the-live-base-queue-row-0i) |
| 0j | **W69** · the client's 50 ms lookup memo returns pre-mutation stats (posix 164/201) | correctness | correctness accepted: real blocked LOOKUP across chmod, immediate stats and full POSIX pass; GNU du samples recorded; unidentified Spark performance gate remains | [full text](../backlog/work-items.md#w69) |
| 0k | **W55** · span committed to raft, fragment PUTs never landed → read EIO, data loss | correctness | open — [follow-up](known-bugs-create-negative-verdict-20261008.md): partial/directory barrier and quorum cold-byte gates pass after resumption; Linux NFS 20-copy cold bytes pass, broad NFS still fails (179/212); macOS plain cp fails attribute copy; cp -X 20/20 cold Mac pass; 640 publication stress operations, 184 peer byte checks and 331 exact-fragment quorum checks pass; Mac broad POSIX fails (166/212), including setup EIO and namespace/locking behavior; original macOS NFS trigger remains unisolated; [validation and POSIX suite split](known-bugs-validation-20261008.md) | [full text](../backlog/work-items.md#w55--span-committed-to-raft-fragment-puts-never-landed--read-eio-data-loss-queue-row-0k) |
| 0l | **W56** · root-level rename leaves a ghost name in the renaming client's local lookup | correctness | fixed in `f8fef814` — exact old directory-name cache eviction across parent/hash tabs; closed: NUC full POSIX/two-client and 64 root-rename reuse repeats pass; xorinox running binary matches cb5e86de containing the fix (Oct 7) | [full text](../backlog/work-items.md#w56--root-level-rename-leaves-a-ghost-name-in-the-renaming-clients-local-lookup-queue-row-0l) |
| mem1 | **W67** · sparse dirty writes and bounded body admission | correctness / resource exhaustion | Closed: NUC live publication failure plus real recovery scratch exhaustion passes at 32+32 and default 256+64 MiB budgets under a 1 GiB/no-swap ceiling; same-mount/cold recovery and physical GC/restart pass | [evidence, fix shape and gates](fuse-memory.md#sparse-dirty-writes-bypass-reclaim-and-cache-admission-has-no-hard-bound) |
| mem2 | **W68** · reply-buffer worker lifetime | resource lifetime | Closed: NUC direct/buffered real worker retirement, allocation/TLS/growth fault RSS and key-failure gates pass | [evidence, fix shape and gates](fuse-memory.md#readreaddir-reply-buffers-leak-when-fuse-workers-exit) |
| 0o | **W58** · open(O_EXCL) create answered EEXIST for a name the same client's own create just landed | correctness | open — [ordinary negative CREATE receipt fix](known-bugs-create-negative-verdict-20261008.md) and replay-capacity/ACK refresh fixes pass Linux regressions; actual FUSE lost-reply/BUSY identity and cold wire verdict gates pass; 16-worker exclusive-create gate passes; historical trigger remains unisolated; [validation and POSIX suite split](known-bugs-validation-20261008.md) | [full text](../backlog/work-items.md#w58--openo_excl-create-answered-eexist-for-a-name-the-same-clients-own-create-just-landed-queue-row-0o) |
| 0p | **W59** · write(2) via FUSE fails ENOSPC with 156 GiB free — client cache-admission mapped to ENOSPC; the 8 MiB metadata budget never drains | correctness | Closed: NUC live publication failure plus real recovery scratch exhaustion passes at 32+32 and default 256+64 MiB budgets under a 1 GiB/no-swap ceiling; same-mount/cold recovery and physical GC/restart pass | [full text](../backlog/work-items.md#w59--write2-via-fuse-fails-enospc-with-156-gib-free--client-cache-admission-mapped-to-enospc-the-8-mib-metadata-budget-never-drains-queue-row-0p) |
| 0q | **W60** · sequential prefetch starves tiny demand reads | correctness | fixed in `73ce8aaa`; NUC 32 MiB A/B: baseline 2000/2000 failures, fixed 0/2000 and no read-NOMEM; closed Oct 9: user accepts AMD nested-tree 2000 tiny + four 128 MiB files, core212/212, peer64/64 and cold physical GC; Xorinox optional | [evidence and remaining gates](fuse-memory.md) |
| 0r | **W61** · local GC discards failed lane-sweep verdicts | correctness | repaired in `3a1b4a52`; private-store results recorded in GC checkpoint; xorinox rollout/reclamation observed Oct 7 (`cb5e86de`); final drain, NUC service rollout and lost-ledger reconciliation remain open | [full text](../backlog/work-items.md#w61) |
| 0s | **W62** · sweep/truncate batch boundaries lose delta GC records | correctness | repaired in `3a1b4a52`; private-store results recorded in GC checkpoint; xorinox rollout/reclamation observed Oct 7 (`cb5e86de`); final drain, NUC service rollout and lost-ledger reconciliation remain open | [full text](../backlog/work-items.md#w62) |
| 0t | **W64** · durable append reservation replay across leader changes | correctness / recovery | partial — durable replay, coordinated cross-group EOF/completion and bounded cleanup pass AMD direct/TCP replay, POSIX/peer, sync-write and cold-byte gates; production sessions and broader failover acceptance remain open ([ledger](replay-readiness-20261008.md)) | [work item](../backlog/work-items.md#w64) |
| 0u | **W65** · daemon exceeds graceful shutdown wait | liveness investigation | closed for supported TCP: six active-fsynced-write leader stops (three direct/three buffered) recover every acknowledged file cold; RDMA deferred | [work item](../backlog/work-items.md#w65) |
| posix-link | **AMD `link_of_symlink` — Python request/test mismatch** | test correctness | root cause proven on native and EFS storage: default os.link requests follow; explicit no-follow works; test correction and full reruns owed | [evidence and fix](amd-link-symlink.md) |
| virtual-io | **W91/W92 · `.efs/` commands — server snapshots and safe reads** | correctness / performance / access control | in progress — opt-in command scripts, versioned JSON queries, server snapshots/index and private gates implemented; loaded publication, publisher fault, noexec and opt-out gates pass; server startup disablement/catch-up gates pass; nine profiling rounds include CPU/storage/scaling evidence, with software limits remaining; full scale/remote per-export policy/platform acceptance remains open | [latest profiling](efs-command-performance-20261008.md) · [policy](efs-command-policy-20261008.md) |
| gc-obs | **W63** · GC backlog, reclaimed bytes and retry progress are missing | observability enhancement | repaired in `3a1b4a52`; private-store results recorded in GC checkpoint; xorinox rollout/reclamation observed Oct 7 (`cb5e86de`); final drain, NUC service rollout and lost-ledger reconciliation remain open | [full text](../backlog/work-items.md#w63) |
| 0e | **W38** · ior-hard fold tombstone without the span's bytes | correctness | code present; dated Oct 5 checkpoint — replay preserves live spans; folds require byte observations; deterministic regression + ASan/UBSan pass; traced real IOR-hard + independent cold hardscan pass (108000 records, bad=0, short=0); nine physical hosts remain; AMD software-RXE fold/GC gate passes, hardware RDMA remains | [full text](../backlog/work-items.md#w38--ior-hard-fold-tombstone-without-the-spans-bytes-queue-row-0e) |
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
| container-profiling | **P4.1** · container tracing/CPU profiling | storage enhancement | 8 rounds complete on `devel-storage-engine`; 12 additional VM cases and clean Linux suite pass; duplicate mapping/hash work removed; buffered-write median +2.8% in two repeats, other rates mixed; metadata stalls unresolved; files default | [rounds 1–6](container-profiling-20261008.md) · [rounds 7–8](container-profiling-rounds7-8-20261008.md) |
| metadata-group-perf | **P4.1 / W66** · metadata-placement performance split | investigation / benchmark enhancement | open follow-up; Oct 8 live probes reproduce group-2 reads at 16.5–18.7 ms median versus 1.4–1.6 ms for group 0; direct leader contact remains slow; quorum/pump durability candidate, causal isolation pending | [follow-up](metadata-group-perf-followup-20261008.md) |
| nuc-storage-engine | **P4.1** · bare-metal NUC files/containers A/B | storage validation | 8 cases + full native suite pass; 64 daemon launches verified; installed services preserved; group-2 versus group-0 inode placement splits rates and blocks engine ranking | [checkpoint](nuc-storage-engine-ab-20261008.md) |
| storage-engine-modules | **P4.1** · built-in engine modules | architecture enhancement proposal | documented, not implemented; shared coordination plus opaque physical-engine contexts; estimated 18–34 hours; dynamic loading deferred | [proposal](storage-engine-modules-proposal-20261008.md) |
| container-engine | **P4.1** · experimental daemon files/containers selector and VM A/B | storage feature | implemented experimentally on `devel-storage-engine`; 16 VM A/B cases and full Linux suite pass; files remains default; read-index waits limit read-speed attribution | [checkpoint](container-engine-20261008.md) |
| bench-churn | **W90** · independent steady-state storage churn benchmark | benchmark enhancement | implemented on `devel-bench`; functional gates pass; initial matched 50/80/95-percent evidence; 64 KiB scaling, shared ranges and five profiling rounds recorded; bounded punch batching retained; merged into isolated `devel-storage-engine`; matched simulator comparison validated; 8 longer cases favor punch at 95%/QD4 (same-device roots); no production changes | [work item](../backlog/work-items.md#w90) · [ranges](bench-scaling-ranges-20261008.md) · [profiles](bench-perf-five-rounds-20261008.md) · [comparison](bench-layout-comparison-20261008.md) |
| P4.1–P4.4 | fragment on-disk layout (**wipe**), W40 FUSE write copy, RDMA zero-copy receive, 9-client scaling | performance | deferred until P3's numbers | [full text](../backlog/work-items.md#p4--deferred-until-p3s-numbers-long-one-is-a-wipe) |
| meta-scale | **W66** · metadata leadership distribution and topology-independent routing | scalability | open — Oct 5 implementation gap recorded; correctness work first, then baseline measurement and staged multi-Raft implementation | [evidence, phases and gates](metadata-scaling.md) |
| tiny-raft | **W93** · broader durable Raft group commit | performance | implemented — covered AMD batching/fault/cold and balanced long mixed-load gates pass | [work item](../backlog/work-items.md#w93) |
| tiny-create | **W94** · atomic CREATE plus open lease | performance / lifecycle | partial — retained FUSE CREATE/adoption, sequenced lease edges and cleanup AMD gated; REPORT session fences AMD gated; data-plane/per-publication integration and measured consensus/throughput gate remain open | [work item](../backlog/work-items.md#w94) |
| tiny-data | **W95** · tiny-file data persistence and packing | performance experiment | partial — tiny/durable/cold/crash/churn gates pass; indexed packing and P4.1 decision remain | [work item](../backlog/work-items.md#w95) |
| tiny-kv | **W96** · bounded KV WAL preallocation | performance experiment | partial — recovery/reservation/fault and final guard cold gates pass; isolated NUC acceptance owed | [work item](../backlog/work-items.md#w96) |
| decisions | **D28** client-death loss policy · **D29** durable publication receipts · **D30** bounded admission/pacing | accepted design | accepted Oct 8: salvage deferred; D29/W88 implementation first, then D30/W23/W89; implementation and acceptance remain open | [contracts and gates](decisions.md#d28d30--accepted-oct-8-2026) |
| op1 | **W70** · shrink-quota falsely reports background migration | operator observability bug | truthful running-quota reporting implemented; NUC accepted/refused shrink and unchanged-byte gates pass; no migration or persistence claim | [work item](../backlog/work-items.md#w70) |

### Source findings awaiting triage

These review findings have canonical identities and gates below; their
positions do not assign an execution order or override the active handoff.

| review ID | finding | classification | review state | evidence / gate |
| --- | --- | --- | --- | --- |
| spec1 | **W71** · fragment PUT lacks a production persistence barrier | durability correctness | implemented barrier — file and pathname fsync before production PUT ACK; fault/live gates pass, hardware power-loss gate owed | [evidence and gate](../backlog/work-items.md#w71) |
| spec2 | **W72** · production mount session lifecycle / I23 integration | fencing integration | partial — full mount UUID/epoch shared by append/HOLD/FLOCK; retained FUSE CREATE/adoption and sequenced OPEN/CLOSE plus proven-revocation recovery pass AMD direct/TCP; namespace shard admission and fenced replay/effect checks are AMD gated; REPORT mapping/activation/completion session fences pass AMD direct/TCP; staged typed per-publication recovery/newer-range ownership passes AMD; production flush/PUT-ticket integration remain open | [evidence and gate](../backlog/work-items.md#w72) · [Oct9 mounted-client gates](dependency-package-20261009.md#terminal-retained-identity-recovery--implementedgated) |
| spec3 | **W73** · synchronous application-write publication | integration | implemented — explicit O_SYNC/O_DSYNC publication passes AMD direct/TCP small/queued/append, peer-before-close and cold crash gates; mount-wide sync also passes AMD direct/TCP peer-before-close and cold bytes on the current REPORT candidate; final mode matrix owed ([Oct9 evidence](../../results/measure/20261009-report-log-coverage/README.md)) | [evidence and gate](../backlog/work-items.md#w73) |
| spec4 | **W74** · protection profiles, debt and automatic repair | recovery feature gap | fixed 2+1; no automatic repair/profile cutover; accepted design exceeds current capability | [evidence and gate](../backlog/work-items.md#w74) |
| spec5 | **W75** · fragment integrity trust / missing checksum fallback | integrity correctness | missing/corrupt evidence fails closed; captured metadata digest and PUT validation gates pass on NUC; identity-bound format remains open | [evidence and gate](../backlog/work-items.md#w75) |
| spec6 | **W76** · opid table exhaustion sends unprotected namespace retries | conditional correctness | Closed: bounded identity admission refuses saturation safely; Oct9 reserves receipt capacity with16 unacknowledged positions (512 tracker slots); all four lost-reply mutations replay across leader death/restart with one effect | [evidence and gate](../backlog/work-items.md#w76) |
| spec7 | **W77** · atomic multi-chunk public publication/observation | integration gap | per-group REPORT/per-chunk publication; request-wide decision/read-validation gate unestablished | [evidence and gate](../backlog/work-items.md#w77) |
| op2 | **W78** · server wrapper trusts PID-file process identity | conditional operator correctness | identity-checked pidfd daemon/recorder retirement implemented; NUC ownership fixtures pass; daemon shutdown remains W65 | [evidence and gate](../backlog/work-items.md#w78) |
| op3 | **W79** · mkfs help/name argument disagrees with single-export handler | operator observability | legacy label explicitly reported as ignored; extra arguments rejected; NUC repeated mkfs preserves salt/namespace | [evidence and gate](../backlog/work-items.md#w79) |
| op4 | **W80** · efs-query reports retired zero-filled statistics as totals | operator observability | unsupported status and CLI nonzero exit replace invented zero totals; NUC RPC/CLI gates pass; actual totals remain unimplemented | [evidence and gate](../backlog/work-items.md#w80) |
| scale1 | **W81** · automatic directory spreading lacks a pressure trigger | scalability feature gap | size trigger/migrator exist; pressure policy remains unspecified and unwired | [evidence and gate](../backlog/work-items.md#w81) |
| proto1 | **W82** · stale completed ReadIndex authority survives leader isolation | read correctness | Closed: stale LOOKUP/GETATTR/session/publication/transaction views and configuration partition gates pass; coordinator callback refuses untrusted absence | [evidence and gate](../backlog/work-items.md#w82) |
| proto2 | **W83** · transaction decision records lack safe retirement | metadata lifecycle gap | resolve/drop remove participant records; no decision acknowledgement/GC path found | [evidence and gate](../backlog/work-items.md#w83) |
| proto3 | **W84** · bounded namespace transactions | namespace completeness | Closed for accepted D32 144-participant envelope: distinct EOVERFLOW, prompt atomic refusal diagnostics, mixed boundaries, full64-lane removal/replacement, exact144 PREPARE/COMMIT cold recovery and AMD normal/fault POSIX/GC gates pass; larger-operation protocol deferred | [closure and limits](w84-closure-20261009.md) |
| storage1 | **W85** · path-hint eviction loses ambiguous PUT retry history | conditional accounting/storage correctness | Closed: ambiguous PUT placement retained in 389a8baf; direct/buffered real lost-ACK collision and two-root physical/quota/restart gates pass | [evidence and gate](../backlog/work-items.md#w85) |
| gc-own | **W86** · durable PUT ticket integration | feature / safe reclamation | metadata state machine plus replicated admission/authoritative status transport implemented (`0aaf7d32`), AMD cold replay/body collision/revocation gates pass; production data-plane fences and all-member collection remain staged | [evidence and gate](../backlog/work-items.md#w86) · [D31](decisions.md#d31--recorded-abandoned-upload-policy) |
| append-load | **W87** · buffered concurrent append loses records under load | correctness investigation | failure recorded in private GC fixture; isolated repeats pass; full traced acceptance owed | [evidence and gate](../backlog/work-items.md#w87) |
| replay-span | **W88** · folded span retry resurrects after bounded history eviction | publication/retry correctness | partial — actual cross-group typed cache/host replay preserves peer bytes and cold mappings on AMD direct/TCP; production flush/ticket integration and full replay gates remain open | [evidence and gate](../backlog/work-items.md#w88) · [Oct9 checkpoint](dependency-package-20261009.md#retained-typed-publication-writeback--staged-amd-gated) |
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
ledger, and the previous status row is retained in the archive. W56 is closed; W60 retains its original large-tree verification obligation.

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


### Xorinox's two GC errors — investigation comment (Oct 7)

[Read-only investigation](gc-errors-20261007.md): both are cumulative xefs3
`reap_errors` from metadata/Raft proposals, first observed at 16:35 and 16:39
UTC; physical-delete errors are zero. GC continued and both sampled leader
queues are now empty. The deployed code omits the command/return code, so the
exact causes are not recoverable. W63 follow-up: separate expected orphan BUSY
deferrals from retry/hard failures, retain operation/inode/return-code diagnostics,
and label portal errors as cumulative with node/class and recent activity.
Preserve all lease/transaction checks and retry records. The known age-clock
fix is implemented locally but still needs xorinox rollout; see the checkpoint.

### Implemented-fix gate rounds (Oct 7)

[Thirty-round ledger](gate-30-rounds-20261007.md): NUC gate results, additional fault/restart/configuration evidence and remaining obligations. W68 closes; other packages retain their explicit platform, scale or durability gates.

### Next five implemented-fix gates (Oct 7)

[Follow-up ledger](next-five-gates-20261007.md): W27, W76, W82 and W85 close
after targeted NUC acceptance; W69 correctness passes, with its unidentified
Spark performance measurement still owed. Grouped open counts are now
15 unfinished, 11 awaiting gates, three decisions and 18 future features:
**47 open packages**. Earlier tables remain dated baselines.
