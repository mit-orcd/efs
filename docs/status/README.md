[Intel package 3 execution](intel-package3-20261013.md): deployed `1a737143`,
files/TCP/direct, client/server commands, packing and expiry verified OFF;
container fixture stopped. Durable query mask0/version1 survives restart.
Complete normal native and six affected ASan/UBSan harnesses PASS on both hosts;
mounted qualification in progress, W97/D30/W106 OPEN.

[Three required work packages and feature matrix](three-work-packages-20261013.md):
ORCD `.efs` enabled for final RDMA/direct scale acceptance; AMD `.efs` enabled
for fixes and functional qualification; Intel `.efs` client/server disabled for
ordinary-file correctness/performance. Packing, containers and expiry disabled
in all three. Packages remain OPEN; this assignment supersedes generic
AMD-or-Intel placement for enabled `.efs` work.

[Reviewed all-branch devel integration](devel-reviewed-integration-20261013.md):
172 missing commits integrated from Mac/release, container and expiry branches;
parallel CLOSE, KV, GC and Raft overlaps resolved explicitly. Private AMD Linux
build and full normal native suite PASS at `986b4ec0`; 849 product/test input
hashes match. Mounted/fault/full-sanitizer/RDMA/scale gates remain separate.
No deployed-source verdict is superseded. Churn numbering is W114; client telemetry retains W113.

Current ORCD `.efs` source `419aa90f367a` passes complete native/fault/sanitizer
prerequisites, exact-source/profile deployment and acknowledged payload markers
on all seven clients. Orderly redeployment used no force; server readiness took
284.704 seconds of the unchanged 300-second budget. Mounted acceptance FAIL:
all six focused command suites encounter ENOSPC reading query files. The generation-size ceiling also reproduces with 3,000 synthetic files and a private 4 MiB test budget; compact derived-directory links are a proposed small-target optimization. Later
concurrent/restart/full-400-GiB/loaded-latency gates are NOTRUN on this source.
The earlier strict full 400 GiB PASS at `62770d5c3bfd` remains source-specific.

Catalogue retirement, retained-CLOSE scheduling and exact-EOF write acceptance
have small native regressions and corrections in this deployment; their loaded
mounted acceptance is still open. The original 5.569/22.044-second stalls,
application fork/direct-read platform gate and broader scale gates remain open.
All outstanding `.efs` work remains required: the [cluster assignment matrix](orcd-small-cluster-followups-20261013.md#required-outstanding-efs-work-and-cluster-assignment)
assigns enabled `.efs` logic/regression development to AMD Linux, with full-scale
seven-client RDMA/direct-I/O, cold restart, payload, latency and resource
acceptance on ORCD. No new execution or passing verdict is implied.
See [small-cluster development findings](orcd-small-cluster-followups-20261013.md)
and the [400 GiB evidence](orcd-efs-400g-20261012.md). Expiry, packing, containers
and unplanned-event repair/recovery remain outside the current profile.

# Project status — current work and index

**Historical experimental work, superseded by package 3; fixture stopped:** [Intel container sprint](container-intel-sprint.md), `devel-container`, containers/TCP/direct; `.efs`, expiry-date and packing OFF. Latest exact tested/deployed product **9145b7a3**: container native/sanitizers/runtime, ownership12/12 and all15 normal gates complete. Concurrent **1510/1512 PASS**; deep-directory30s timeouts remain on Intel1client3 and Intel2client2, other five clients216/216 PASS. Immediate physical113.767s/final8.983s and all quiet432/peer134/lifecycle/tiny/restart/cold gates PASS (shared mmap ENODEV skip). Loaded acceptance FAIL; one-run improvement does not close W100. Historical --deepsteps cleanup attribution is withdrawn: observer capability wrapping selected pathname-based rmtree. Capability-preserving descriptor observer committed as `1c86ac81286e3d21030a1e2a011678f5c80d9a21`: local regression checks PASS; Intel build, deployment and corrected diagnostic rerun NOTRUN; normal1510/1512 is unaffected; no release promotion.
**Mac checkpoint committed; native qualification remains open:** implementation `32f71f27`, full regression PASS; latest native **213/216**, three small-file timeouts, best fresh 214/216. Current-source persistence **27/27 prepare + 27/27 verify** after clean same-store restart PASS. Proposed 64-entry durable-ancestry cache is **undeployed**, awaiting approval and real-storage safety gates. [Results and small-file durability investigation](mac-v032-posix-20261012.md).

**Proposed feature:** [W113 — aggregate client workload telemetry](../backlog/work-items.md#w113--aggregate-application-workload-telemetry-across-fuse-clients): ORCD seven-client read/write ops/s and human-friendly bytes/s, separate from server/internal traffic. Discussion recorded; implementation and acceptance NOTRUN.

**Current development work:** [W95 packing TCP/direct-I/O](packing-tcp-direct-20261012.md) on `devel`, solo/exclusive AMD with `.efs` disabled. Exact `8151debe`: full normal/fault native, nineteen focused normal/sanitizer and actual-core sanitizers PASS; three quiet rounds each1512/1512 core +67/67 peer, all orderly/cold/reclamation/drain PASS; focused acknowledged-fsync SIGKILL/cold PASS. Deep-directory/unusual-name timeout acceptance closed for this source/workload. W95 production-format/migration and matched packing-on/off performance remain open. Next: [three alternating off/on pairs, lifecycle coverage audit, and P4.1 decision](packing-tcp-direct-20261012.md#next-work-toward-w95-closure--oct-13). Fresh-store-only opt-in packing is a recommendation, not an approved rollout. Original quotas/deadlines unchanged; no push or release promotion.

**Historical ORCD qualification (superseded by the current verdict above):**

[ORCD real 400 GiB command gate](orcd-efs-400g-20261012.md) uses `devel-efs`,
RDMA/direct/files, commands enabled and packing/containers disabled. Full writes,
all 1,638,400 cold payloads and exported paths, and human/default/JSON stats on
all seven clients PASS at `254a4a295c66`. Overall follow-up acceptance is FAIL:
full-catalog simultaneous seven-client queries still time out at `6bc43418`,
although all 1,512 core cases and all focused suites pass. Its production
worker-stack correction is verified and the cluster is healthy. Refreshes
take about 15–17 seconds and alias discovery is about 145ms. Late pending
transaction refusals still delay publication; early rejection before cache
copying and bounded fresh-capture retry are qualified in native/sanitized
oracles. Full qualification found a retained-orphan harness extraction
regression, corrected and verified before deployment; live acceptance remains
pending.
Deadlines and quotas stay unchanged. Passing basic commands/pages/large deltas and all failing logs are retained.
Orderly shutdown/remount and cold markers PASS after correcting startup readiness.
Seven screen benchmarks remain cleanly stopped pending successful follow-up gates,
then will resume with their existing files. Contended-write freshness and host
resource limitations remain documented.

[devel-efs scope and outstanding items](devel-efs-20261012.md) records the new
release-v0.3.2-based branch, the original four-item scope (expiry deferred) and the requested ORCD
RDMA/direct-I/O profile with commands enabled and containers/packing disabled.
Deployment and ordinary-file ORCD command gates PASS at `0dd2a7c7`: six
focused suites, three quiet mixed rounds (4,536 POSIX + 63 query cases), peer67,
cold queries/owner names, policy persistence and 1 GiB exact-payload/export
verification. [ORCD command qualification](devel-efs-query-20261012.md) retains
the failing baseline, fix and limitations. Broader scale/platform gates remain open.

**Current release integration:** `release-v0.3.2-pre-alpha` merges reviewed ORCD evidence9fc5e024 into v0.3.1. Product/test inputs unchanged. AMD TCP/direct and TCP/buffered PASS; independently reviewed ORCD nativeRDMA/direct two core rounds1512/1512 and peer67/67 per round, native/fault/sanitizers/cold/physicalGC PASS. Tested SHAs remain15e2a531 AMD and40f4509d ORCD; the merge itself was not remotely executed. Packing/containers/.efs/repair remain disabled. No push/tag/deployment. [v0.3.2 review and coverage](v032-integration-review-20261012.md).

**Latest alpha scope:** ordinary files/TCP/direct only, `EFS_PACKED_WRITES=0`, `EFS_COMMANDS=0`, `EFS_COMMANDS_SERVER=0`. Small-file packing is excluded alongside containers, `.efs`, RDMA, buffered and dedicated repair/recovery. Earlier packing qualification requirements below are historical. [Binding packing exclusion](v030-packing-exclusion-20261012.md).

**Standing testing scope:** containers are experimental and excluded from required tests and release gates. Use files for required qualification; container-only investigation requires explicit new user authorization. [Binding policy](container-testing-policy-20261012.md).
**Current scope (user decision): repair and recovery for unplanned events are deferred; focus on normal operations, clean shutdown and orderly restart.** Existing durability safeguards and fault-test evidence remain in place. The historical standalone two-repair-GAP/TCP-only item is closed on the isolated fix branch; automatic and packed-segment failure repair remain deferred. [Scope and qualification](orcd-default-repair-20261012.md#current-work-scope--user-decision).

[ORCD v0.3.1 RDMA/direct-I/O requalification](orcd-v031-rdma-direct-20261012.md) tests the frozen `release-v0.3.1-pre-alpha` snapshot with packing, containers and `.efs` excluded.

[ORCD ordinary-file RDMA/direct-I/O deployment and newer-commit review](orcd-profile-review-20261012.md): `.efs`, containers and small-file packing disabled and excluded; normal operations and orderly restart qualify separately from the release branch.

[ORCD mandatory standalone repair qualification](orcd-default-repair-20261012.md) supersedes the earlier default repair GAP for ordinary missing-base roles; release activation and broader W74 remain separate.

[Isolated ORCD standalone fault/repair fixes](orcd-standalone-fault-fix-20261012.md) track the subsequent explicit user request, separate from release qualification.

## v0.3.0-pre-alpha release integration

Current solo final-source freeze: **4e239d029029b8105a6e07f0e69a26fdc449de32**. AMD deep-directory blocker fixed by bounded receive batch8→32; three fresh candidate full-core runs and final release run PASS. Final normal/fault native, earlyACK, five affected sanitizers, core1512/1512, peer67/67, seven stops and ordinary overwrite/cold/reclamation PASS. Packing/containers/.efs remain disabled. No push/tag/promotion. [Fix and exact-source report](v030-deep-directory-fix-20261012.md). Older failing source reports below remain historical; final-source ledger governs.

Latest execution identity: **e59e815fab2916340baad585dc08712ed182b7ea**; files/TCP/direct, both commands flags0. AMD heavy and Intel directory deadlines remain FAIL; native/sanitizers and focused ordinary I/O/NUC checks PASS. Isolated packing-phase diagnostic7ef prerequisites PASS; live-retry1 PASS14/14 and7stops; broad phase diagnostic retry1 FAIL278.320s, cold bytes PASS; root continues solo after Intel/NUC stop (heartbeat paused). Preserved118 compilation and initial7ef output-directory failures; it is not a qualification candidate. [Current ledger](v030-tcp-direct-qualification-20261012.md) governs over historical checkpoints below.

**Execution RESUMED** by the subsequent human “start” instruction; progress reports every15minutes. Common candidatedef46109909c793e58ae2c453c5de537cdb2c627 is frozen on qualification-v030-tcp-direct, not yet release accepted. AMD/Intel/NUC all received this SHA. Old binaries/data and backwards compatibility need no preservation effort; identified EFS stores may be wiped if easier. [Accepted release scope](v030-release-scope-20261012.md): **TCP/direct I/O/files engine**. Containers, buffered/RDMA and dedicated repair/recovery qualification are deferred and no longer release blockers; their gaps remain open. Four planning gate groups remain: read-progress fix safety/sanitizers, seven-client packing/query acceptance, direct-I/O files POSIX/peer/mutation/GC, and final frozen merged-source native/fault/affected live qualification. Owners: **AMD groups1/4, Intel group2, NUC group3**. All start from full baseline8b7391b3 and test one centrally frozen candidate SHA; see the scope page for parallel execution and final-source rerun rules. This is not an exact invocation count.

Integrated inactive repair prerequisites pass full normal/fault native at exact94bd2e52 (implementation2b216427); no activation or restored-redundancy claim. Excluded privatea9f158ce passes build/affected Raft tests but still needs broader safety/sanitizer/mounted acceptance. Quiet scheduler17d218d7 remainsFAIL (stats/find all7, deep client5), excluded. Clean64-file cold/reclamation diagnosticPASS; global reclamation failures with retained live POSIX trees are confounded. Original services/mounts preserved. See [execution ledger](amd-all-suites-20261011.md). No tag, push or shared branch promotion.

[Current common-source execution](v030-tcp-direct-qualification-20261012.md): AMD normal/fault native and Raft sanitizersPASS; NUC files core216/216,peer67/67 and isolated physical packingPASS with reviewed raw evidence; Common52 seven-client quietFAIL on AMD/Intel; NUC seven-client1Gfixture alsoFAIL and is not equivalent to4G comparators. Newdef contains the2bf leader-hint correction plus a test adapter missing from failed2bf fullnative; corrected exact-source reruns pending. No release acceptance.

Previous qualification source: `410afed433b6da093bdc2acf37bec253ddbd586c`, corrected packaging-v2 (1,563 files, 52 directories). The new AMD all-suite run explicitly freezes `28b2089b`; its verdicts are recorded separately. Later integration/documentation commits are not new execution identities. AMD full normal/fault native, 20 focused sanitizer and fully sanitized real-LSM catalog/audit gates PASS. Fixed mounted CREATE three-stage/cold ownership and targeted query diagnosis remain NOTRUN centrally.

Historical broader-scope blockers (superseded for release scheduling by the scope decision above):

- AMD c2 seven-client/jobs 16 diagnostic: stats/find 30-second deadline FAIL on all seven clients; deep60 and independent cold bytes PASS. Publication gaps correlate with timeouts, but request/root causal proof is missing.
- Mac c2 mounted POSIX: 199/216 PASS and 17 timeouts; peer/policy/failover/cold NOTRUN. The fault-only 410 CREATE change does not repair normal-path failures.
- Final-source qualification is incomplete: Mac 410 native STOPPED; ORCD 410 fault PASS/normal STOPPED/sanitizers and mounted NOTRUN; NUC 410 execution NOTRUN. Older candidate passes do not transfer.
- Retained Mac inode4091 creating writer/ownership and a safe repair remain unproven. The CREATE duplicate fault hook is fixed and locally regressed; strict live/cold acceptance remains pending.

[Release ledger](v030-pre-alpha-release.md) · [central stop/merge checkpoint](../archive/status-20261012/v030-stop-checkpoint-20261011.md) · [query/fault diagnostic history](v030-query-stage-diagnostic-20261012.md) · [reconciliation JSON](v030-owner-commit-reconciliation.json) · [candidate operator notes](../using/v030-candidate.md) · [documentation consistency review](status-consistency-review-20261011.md).

[Superseded release-index narrative](../archive/v030-index-checkpoints-20261011.md) preserves the older source-specific checkpoints removed from this running summary.

Owner stop records: [Mac](mac-common-candidate-c2bfb9e9.md#manager-requested-stop-checkpoint), [NUC](nuc-common-c2-qualification-20261012.md#human-stop-checkpoint), [ORCD](../archive/status-20261012/orcd-stop-checkpoint-20261011.md). [Intel final stop checkpoint](../archive/status-20261012/intel-stop-checkpoint-20261011.md) is now integrated: both410/v2 normal Make suites PASS, partial standalone4/88 and6/88 PASS; fault/sanitizer/mounted NOTRUN. Its [acceptance ledger](intel-deployment-acceptance-20261011.md) preserves earlier c2 failures. The resumed complete-suite request and Intel/NUC split were subsequently stopped. AMD fixtures are idle and original mounts restored. Frozen28 owner stop checkpoints are Intel `6acd3bdd` and NUC `68901f73`; the subsequent human reply confirms execution stopped.

[Architecture](../how-it-works/architecture.md) · [agent handoff](in-flight.md) · [decisions](decisions.md) · [work-item details](../backlog/work-items.md) · [operations](../operations/operations.md) · [archive](../archive/README.md).

## Historical NUC integration session — October 11

[Current two-hour integration and repair ledger](devel-nuc-integration-20261011.md)
records the captured all-branch merge, W72 fresh-mount admission fixes, a bounded
W52 prepare-pipeline experiment and their retained failures. Corrected native
and both full live modes pass (POSIX 216/216, peer 67/67 per mode). Additional
loaded APPEND/GC rounds pass in both modes. Four written-ring drive controls
repeat the NVMe decline with physical request evidence; no cache diagnosis or
hardware ceiling is claimed. Long 20k append gates remain incomplete. Enabled
command/options gates pass in both modes, final native passes, and the original
installed runtime and retained sample bytes are verified after restoration.

## Historical NUC closure session — October 10–11

[Current NUC acceptance](devel-nuc-closure-20261010.md) closes W70, W78, W79
and W80 for their named operator-correctness scopes, plus W96's isolated
allocation and software recovery acceptance. Full native, actual stale
PID startup/stop, both mkfs aliases' consumed-reply recovery and explicit
retired-query outcomes pass. W96's sixteen drive comparisons, exact retained
replay, ASan/UBSan and Valgrind pass. Both full live modes pass POSIX214/214,
peer66/66, cold/fault/GC and actual follower WAL-fsync refusal recovery.
W111's clean allocation, separate XFS profiles and written-ring controls all
finish: NVMe latency rises even on written extents; W111 remains open. The
three-hour session restores four regular daemons and both mounts, verifies
installed/live hashes and16 existing payload samples. New-binary installation
remains approval-blocked; the original installed build is retained.

## Governing node-count contract — D36

[User-ratified D36](node-count-protection-20261009.md): three nodes minimum,
four nodes maximum two-loss safety, additional nodes for capacity and performance
across all dimensions. This supersedes the earlier protection ladder. Current
fixed 2+1 and three-voter metadata groups do not satisfy the four-node target;
W74/W66 must close the gap. Never infer the accepted design from current code.

## Design proposals

[Scratch expiry and virtual `.efs/` commands](scratch-expiry-interface.md)
records the preferred `/scratch/.efs/help`, `ls`, `find` and `stats` interface,
expiry presentation and proposed lifecycle design. Tracked as **W90** (expiry)
and **W91** (`.efs/` commands); policies were subsequently accepted as D33/D34; implementation and gates remain open.

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

## Release correctness review

[Checked address-port parsing](address-parser-20261011.md) fixes signed/overflowing
and partially numeric ports across six CLI/bootstrap parsers. Exhaustive valid-port
and malformed-input sanitizer regressions pass; valid host splitting is preserved.

[Benchmark walker mode cleanup](benchmark-walk-mode-20261011.md) removes the
unselectable private execute-check mode. Before/after owner-RPC batch/fallback
regressions pass, with public lookup and FUSE permissions unchanged.

[Retained-body flush simplification](flush-retained-body-mode-20261011.md) removes
the unselectable stolen-body mode after the pipeline correctness fix. Cache/fence/
pressure gates pass; ownership through exact REPORT ACK is preserved.

[Full flush-pipeline failure](flush-pipe-failure-20261011.md) reproduces and fixes
a clean incoming snapshot losing dirty retry eligibility after the full pipe fails.
Actual cache/pipe fault-injection and sanitizer gates pass; mounted acceptance
remains pending.

[Private legacy find cleanup](private-find-cleanup-20261011.md) removes the
uncalled parser/renderer chain while preserving active query/stats authorization.
Focused callback/routing/authority gates pass; merged Linux acceptance is pending.

[Recorder shutdown ownership](recorder-stop-20261011.md) fixes signaling after reap
and ECHILD across the profiling stop paths. Stubbed lifecycle/failure tests pass;
real Linux recorder and full merged acceptance remain pending.

[Deferred rollup budget](rollup-budget-20261011.md) charges retained queue capacity
and admits replacement growth against the existing staging target, falling back to
eager rollups under pressure. Focused accounting/boundary regressions pass; this
does not change the cache's pinned-work over-target policy.

[Rollup allocation-failure fix](rollup-allocation-failure-20261011.md) records the
reproduced dangling-owner failure, ownership-safe queue growth and focused sanitizer
gates. Full merged/Linux acceptance remains pending; focused queue-accounting and
recorder repairs are indexed above. [Review release disposition](../reviews/release-disposition-20261011.md)
protects active packed/query consumers from broad cleanup.

## Tiny-file performance follow-up

**W94 CREATE/adoption closed Oct 11**, by the user's explicit scope decision:
[accepted lifecycle, fault and performance evidence](amd-feedback-integration-20261011.md#w94--createadoption-closed-by-user-scoped-decision).
Broader flush/PUT-ticket integration remains W72/W86/W88; no activation is implied.

[AMD W94/W97 lifecycle and replication-feedback acceptance](amd-feedback-integration-20261011.md)
records passed actual Linux async and two-mode full lifecycle/POSIX/fault/cold
gates with feedback on; pressure/recovery and repeated comparisons are running.
Slow-lane control/read-probe starvation is reproduced and repaired (`4ff779d0`);
actual CREATE-owner allocation, deterministic adoption/competing-OPEN and
slow-follower barrier regressions pass. Corrected full native/fault and direct
live lifecycle gates pass; buffered and subsequent fault/pressure acceptance
are pending. Normal stores and the completed
AMD admission closure are retained.

**Scope split — Oct 11:** [W111](w111-nuc-persistence-investigation-20261011.md)
now owns the NUC durable-write slowdown investigation. **AMD unpaced
overload/admission is closed** ([accepted deployment](amd-overload-admission-20261011.md));
its closure is independent of W111.

| Work item / scope | State | Evidence |
|---|---|---|
| W97 AMD data-GC overload/admission | Closed: direct/buffered unpaced, cold recovery, physical drain and deployed service audit pass | [Closure](amd-overload-admission-20261011.md) |
| W97 replication-inbox feedback | Open: sustained receiver-feedback/queue/catch-up acceptance | [W97](../backlog/work-items.md#w97) |
| W112 ordinary full-overwrite STALE replay | Closed reproduced canonical overwrite replay defect: full normal/fault, eleven sanitizers and both-mode exact immediate/cold/cleanup gates PASS at 02b9918b | [Status](w112-full-overwrite-replay-20261012.md) |
| W111 NUC persistence latency | Open: isolate filesystem, block queue and device/cache effects | [W111](../backlog/work-items.md#w111) · [Status](w111-nuc-persistence-investigation-20261011.md) |


[W111 matched three-node SSD tests](w97-three-drive-20261010.md): NVMe
31.83→10.80 files/s with Raft sync 2.465→6.993 ms; SATA 562.73→299.40 with
0.150→0.319 ms. Both slow, at very different absolute latency; no reported NVMe
thermal event during recorded slow phase. Cache/ceiling remains unproved.
128 private cold payload checks pass and normal four-node service restores.


[W111 latest merged NUC observation](w97-latest-nvme-20261010.md) deploys
`6b8f05acb484`: first-minute 152/159 files/s falls to 53/48 before/after a
10-minute creation pause, with higher NVMe flush latency. Rest restores only
a brief burst. Cache/thermal cause remains unproved; W111 stays open. Full
repeat gates pass with the earlier append timeout retained, and normal defaults
plus both cold mounts are restored.


[W93 final NUC acceptance](w93-atomic-apply-20261010.md) **closes W93 Oct 10**:
all 16 captured tips integrate at `37ad6a8790c8`; atomic apply recovery and
APPEND replay/admission repairs pass full/fault/cold gates. Repeated 15-minute
ABBA runs improve candidate throughput by 30.54% across means, with lower
operation percentiles, reported mixed-read outliers and improved idle percentiles
in the larger matched read check. Both NUC mounts are restored on the repaired
build with default settings. Tested tuning remains opt-in; W53/W94/W97 remain separate.


[Oct 10 W93 checkpoint cadence](../archive/status-20261011/w93-checkpoint-cadence-20261010.md) records
all-branch integration, failed-marker immediate retry, bounded checkpoint
coalescing, original/repeated NUC functional gates and private retained-marker
crash replay. The 10 ms default remains; performance and remaining recovery
acceptance are recorded separately from implementation.


[W93–W96: NUC profiling evidence and next experiments](tiny-file-performance-plan-20261008.md)
register broader Raft group commit, atomic CREATE/open lease, tiny-file data
persistence/packing, and KV WAL preallocation. W93 is closed by its final acceptance, and W96 closes after the Oct11 isolated
NUC gate above. W94/W95 retain separate implementation/acceptance obligations. W95 refines existing P4.1,
so the earlier grouped count below is retained as a dated baseline rather than
recomputed by blindly adding four. W90–W92 keep their existing scopes.

## Disk-backed query cache progress

[W92 local disk backend](query-disk-backend-20261009.md) implements persistent
partition catalogs, bounded builder scratch, immutable snapshots and validated
checkpoint recovery. Functional gates and clean Xorinox mutation queries pass after the authorized
wipe; the earlier wrong-shard artifact remains historical evidence. Per-generation
logical disk budgeting is implemented. Incremental derived stats, aggregate cache
quotas and large-scale acceptance remain open.
[W92 tiny-file profiling](query-cache-tiny-profile-20261009.md) records deployed
bounded read windows and immutable-file persistence reuse: controlled unchanged
refresh is 2.07× faster; the mixed live creation-rate drop remains unisolated.
[W92 three additional tiny-file rounds](../archive/status-20261011/query-cache-three-rounds-20261009.md)
records all-node profiles, exact-match lookup, bounded combined-record writes
and one checked raw/index persistence barrier; controlled syscall and cold
correctness evidence stays separate from live throughput observations.

[W92 merged-source suites and three further rounds](../archive/status-20261011/query-cache-merged-three-rounds-20261009.md)
records native, core/concurrent/extras and cold persistence gates, nine all-node
CPU captures and three logical cache fixes. Frozen-namespace preads fall 78.0%,
pwrites 23.6% and median process CPU 29.1%, preserving fsync counts; fixed memory
increases within the tested budget. Live throughput and narrower NFS evidence
are qualified separately. Runtime `a8a15a14` is restored with ordinary `.efs`
mount opt-in disabled; eight retained trees pass cold query/payload checks.

[W92 previous integration and three further profiles](../archive/status-20261011/query-cache-next-three-rounds-20261010.md)
records the captured branch boundary, parent-rollup coalescing and constant-time
cache descriptor hits, with bounded memory and persistence regressions. Controlled
measurements show 39.1% fewer preads, 34.9% fewer pwrites and 15.0% lower median
process CPU, preserving fsync counts. At that checkpoint runtime `f5164bb3` passes native,
POSIX core/concurrent/extras and cold persistence gates; eleven retained trees
verify 78000 files and 724 payload samples, with normal `.efs` opt-out restored.

[W92 closure work](query-cache-closure-20261010.md) remains active. Code
`cf4e365a` is deployed on normal Xorinox files/direct-I/O/TCP services.
Bounded disk indexes, durable incremental catch-up, row-only folding, aggregate
retained-cache admission and the [replicated command policy](efs-query-persistent-policy-20261010.md)
pass full native and private live/cold/fault gates. Ten-million-entry pilots
pass complete streaming, explicit guest kernel-cold recovery and concurrent
old/new export oracles with a fixed 3.69 MiB builder;
authoritative-source memory is tracked separately. The final oracle now checks
all ten rollup fields and sampled inode attributes; full 10m recovery passes. Named native commands,
fresh Linux NFSv4.0, retained payload recovery and loaded publication also pass.

Bounded external-merge batching (`fd457076`) passes focused ordering, ENOSPC,
cancellation, full native and complete 10m recovery gates. The queued 2^32
kernel-cold oracle uses it; subsequent raw batching is included in cf4e365a.
Fixed-budget raw-record/slot batching passes focused fault, full 100k recovery,
full native and named two-mount/Mac/Linux NFS gates. The completed 2^32 source
population is reused by `w92-2p32-raw-batched-v6`; previous builds were
intentionally superseded with evidence preserved. The replacement, corrected
cold watcher and real capacity writer remain active.

Remaining gates: the actual 2^32-entry index build/export/cold/mutation run,
the 400 GiB real-payload write/cold/hash/export run. Named cf4e365a Mac/Linux
NFS and native FUSE full oracles pass, as do 96 fresh Mac queries and all
423 retained payload hashes after cold restart; the captured
FORGET/handle-recovery ESTALE is fixed. These isolated scale fixtures have remaining cold-verification and service
restoration gates in the closure ledger; their current runtime state is unverified. Full named 2^32 kernel-cold
recovery is queued behind the replacement run’s successful terminal verdict
and the retained original population proof; the
400 GiB cold handoff has four recovered refused attempts (exit 1,
`writer state 1: cold handoff refused`), with a finished marker dated Oct11
04:05:07 UTC. Resolve the writer-state precondition and obtain the complete
write verdict before retrying. [Preserved evidence and manifest](../../results/measure/20261012-leftover-evidence-review/README.md)
replace the earlier pending status; W92 stays open and `.efs` is excluded from
v0.3.2-pre-alpha.
The current [W92 specification](virtual-find-stats.md) and backlog reference
the named raw-batched deployment and retain the older checkpoints as history.
The initial capacity writer stopped on CREATE PROTO after 124983 acknowledged
files; the unrouted-BUSY retry fix passes regressions/full native, and the fixed
client rehashed every acknowledged prefix before continuing. W92 is not closed. The backlog and virtual-interface overview are reconciled
with this named deployment and remaining scope. Acceptance audit found missing
index/stats phase diagnostics; cdc13b88 implements them and passes complete Linux
native, live two-mount and Mac/Linux NFS gates. Its full ten-million recovery
passes with 3864520 builder bytes; the actual-entry cold verifier uses this code.
Normal clients and gateway are restored to `.efs` opt-out after platform
acceptance; diagnostic mounts are retired. Private scale services and temporary
VM resources await the remaining gates and cleanup. The completed private
policy fixture is retired with its data and evidence preserved.
[Earlier index text](archive/w92-closure-index-history-20261010.md) is preserved.

[W92 incremental-cache implementation](query-cache-incremental-20261010.md)
tracks bounded change/ancestor sets, immutable partition reuse, exact directory
recomputation and full-build fallback. Three all-node profiles pass content
oracles (124 / 165 / 159 files/s); coalescing reduces sampled cache cost.
Final runtime `d83f3f06` passes full native, 212 core, 64 two-client,
4 extras with one unsupported shared-mmap skip, and 26/26 cold persistence.
Fourteen retained trees verify 96000 files / 922 payload samples.
Median unchanged refresh falls from 720 to 42 ms on a fixed namespace.

[Combined branch integration](find-stats-all-branches-20261009.md) records
`devel`, W66 and persistence-pressure merges into `devel-find-stats`, preserving
the cache fixes. Committed branch coverage and private Linux gates are tracked;
the newer suite/profile checkpoint above records merged-code deployment and live gates.

## Distributed metadata scaling discussion

[W98 — from fixed placement to distributed metadata capacity](all-node-metadata-queries-20261009.md)
records the move to locality-aware sharding and bounded replication: additional
nodes contribute ordinary POSIX metadata and distributed query capacity without
cluster-wide coordination for routine updates. W66 owns authoritative placement,
routing and migration; W92 remains the local disk backend. W98 owns the scaling
discussion and distributed query design. This item is outside the dated grouped
count below.

[W93/W97 NUC read-pressure follow-up](w93-read-pressure-20261009.md)
records latency controls, the rejected credit-order experiment and independent
functional recovery fixes. That early-credit experiment is not promoted; W97 remains open. W93
subsequently closes through the final recovery and sustained acceptance above.

[W94/W97 AMD unpaced admission follow-up](amd-overload-admission-20261011.md)
records reproduced unpaced ENOSPC, an implemented opt-in transient data-pressure
candidate, full normal/fault native passes, an ENOSPC control with cold recovery,
and no-refusal 180-second direct/buffered cold/physical passes; ten-minute
unpaced fairness/recovery gates pass (no refusals, 58.09/53.98-s drain and
post-pressure cold writes); full admission-enabled 216/67 functional gates pass,
and exact-artifact retained-service deployment/two-client audit pass. The bounded
AMD overload/admission gate is **closed**. NUC persistence attribution is
[W111](w111-nuc-persistence-investigation-20261011.md); it is not an AMD
acceptance requirement. W72/W86/W88 integration, W97 receiver feedback and W99
capacity-domain policy remain independent open scopes.

[W94/W97 AMD sustained mixed-load follow-up](amd-sustained-20261011.md) retains
an unpaced private-quota failure, successful quiescent GC progress and an
intentionally interrupted 300-second bounded baseline. Progress-conditioned GC scheduling
passes native/direct functional gates but fails the direct 180-second drain deadline; extra quiescent recovery empties all four stores. The deployed fragment-budget build (`85fb3a4b`) passes native/direct functional and buffered sustained gates; direct sustained cold bytes pass but its drain deadline fails. All-member inode-fence fanout (`e9ea8ca1`) passes full native/direct functional and both 180-second paced cold/physical gates; both ten-minute cold-byte gates pass but drain deadlines fail; the 20-ms follow-up passes native/direct functional gates and both ten-minute cold/physical gates (79.23/77.94-s drains); canonical service restoration passes; the later [unpaced acceptance](amd-overload-admission-20261011.md) closes the AMD overload/admission gate. W94 CREATE/adoption is now closed; W72/W86/W88 integration and W97 receiver feedback remain separate. The same note
records the completed W95 retained experiment review and its format limits.

## Storage pressure policy

[W99 — proactive capacity protection](storage-pressure-read-only-20261009.md)
records the user's requirement for every disk writer and shared/separate data,
metadata and cache storage. Read-only admission must precede exhaustion and
reflect placement/quorum availability and preserve authoritative metadata. The
note explicitly corrects the assumed any-two-of-four guarantee. Thresholds and escalation details remain
open; this new item is outside the dated grouped count below.

## Historical package reconciliation and work queue

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
  Supported software direct/buffered × TCP/RXE and mount-sync acceptance is recorded in the Oct 9 AMD closure. Hardware power loss remains W71.
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
| posix-link | **AMD `link_of_symlink` — Python request/test mismatch** | test correctness | Closed AMD test mismatch: corrected no-follow historical matrix passes; new explicit follow/dangling/core and peer cases pass native and AMD direct/buffered TCP; pre-fix buffered MKDIR failure retained; later repaired by W66 | [evidence and fix](amd-link-symlink.md) |
| virtual-io | **W91/W92 · `.efs/` commands — server snapshots and safe reads** | correctness / performance / access control | in progress — disk index, bounded incremental catch-up, aggregate quota and replicated policy landed; named native/live/cold and Linux NFSv4.0 gates pass; actual 2^32/400 GiB gates and named virtual-export fix rollout remain open; Mac/Linux NFS candidate oracles pass | [closure](query-cache-closure-20261010.md) · [persistent policy](efs-query-persistent-policy-20261010.md) |
| gc-obs | **W63** · GC backlog, reclaimed bytes and retry progress are missing | observability enhancement | repaired in `3a1b4a52`; private-store results recorded in GC checkpoint; xorinox rollout/reclamation observed Oct 7 (`cb5e86de`); final drain, NUC service rollout and lost-ledger reconciliation remain open | [full text](../backlog/work-items.md#w63) |
| 0e | **W38** · ior-hard fold tombstone without the span's bytes | correctness | code present; dated Oct 5 checkpoint — replay preserves live spans; folds require byte observations; deterministic regression + ASan/UBSan pass; traced real IOR-hard + independent cold hardscan pass (108000 records, bad=0, short=0); nine physical hosts remain; AMD software-RXE fold/GC gate passes, hardware RDMA remains | [full text](../backlog/work-items.md#w38--ior-hard-fold-tombstone-without-the-spans-bytes-queue-row-0e) |
| 2a | **W42** · `df` / `efs-mgmt status` report the 3-node capacity model on any node count | correctness | capacity helper is used by both clients; NUC four-node status/df record 500 GiB for 4 × 187.5 GiB quotas; NUC limited-member write/cold-read/reclaim gate passes; [ORCD unlimited-capacity correction](orcd-capacity-tests-20261011.md) passes live capacity/POSIX and private native-RDMA GC gates; protection debt/repair and named 19810 capacity acceptance remain open | [full text](../backlog/work-items.md#w42--df--efs-mgmt-status-report-the-3-node-capacity-model-on-any-node-count-queue-row-2a) |
| 0b | **W27** · REPORT identity from the staging table | correctness | closed: owned-PUT predicate shared by both builders; nonzero unowned observations rejected; 2048-file cold copy/trace and GC/restart pass | [full text](../backlog/work-items.md#w27--report-identity-from-the-staging-table-queue-row-0b) |
| 0g | **W43** · large truncate is refused; logical truncate = **D25** (decided) | correctness | explicit failure is implemented; `3a1b4a52` makes oversized legacy truncate fail atomically; D25 metadata/read/resize foundations exist, but FUSE publication, live-file sweep and public activation remain open | [full text](../backlog/work-items.md#w43--truncateo_trunc-of-a-file-with--32-chunks-in-a-lane-is-a-silent-no-op-queue-row-0g) · [D25](decisions.md) |
| 0a | STALE replay that never converges; remedy = **D27** (decided) | correctness | runtime foundations present; strict timing, fault/recovery/contention and RSS gates remain in handoff | [full text](../backlog/work-items.md#0a--stale-replay-that-never-converges-queue-row-0a) · [D27](decisions.md) |
| 0h | **W44** · the leader's GC frag pass scans the whole prefix every 1.2 s; remedy = **D26** | performance | D26 in tree + gated (dev cluster, Oct 4): watermark gates the scan, cursor bounds the pass; idle leaders logged no `gc-pass` line for 10 min; a 5120-record `rm` drained at ~514 records/pass; Oct 7 10-GiB physical reclamation passes; retained-table active GC latency regression remains (1393-ms pass); idle-hour/committed-tail results in the acceptance ledger | [full text](../backlog/work-items.md#w44--the-group-leaders-gc-frag-pass-scans-the-whole-prefix-every-12-s-queue-row-0h) · [D26](decisions.md) |
| 12 | **W48** · four of the 16 dd streams ended early | investigation | investigate — evidence only | [full text](../backlog/work-items.md#w48--four-of-the-16-dd-streams-ended-early--investigate-plan-row-12) |
| 15 | **W52** · a REPORT after thousands of O_APPEND writes answers after > 30 s | correctness | approved single-proposal shape A still open; bounded eight-reservation transaction and append timer fix await full acceptance, exact 20k append gate owed | [full text](../backlog/work-items.md#w52--a-report-after-thousands-of-o_append-writes-answers-after--30-s-plan-row-15) |
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
| bench-churn | **W114** · independent steady-state storage churn benchmark | benchmark enhancement | implemented on `devel-bench`; functional gates pass; initial matched 50/80/95-percent evidence; 64 KiB scaling, shared ranges and five profiling rounds recorded; bounded punch batching retained; merged into isolated `devel-storage-engine`; matched simulator comparison validated; 8 longer cases favor punch at 95%/QD4 (same-device roots); no production changes | [work item](../backlog/work-items.md#w114) · [ranges](bench-scaling-ranges-20261008.md) · [profiles](bench-perf-five-rounds-20261008.md) · [comparison](bench-layout-comparison-20261008.md) |
| P4.1–P4.4 | fragment on-disk layout (**wipe**), W40 FUSE write copy, RDMA zero-copy receive, 9-client scaling | performance | deferred until P3's numbers | [full text](../backlog/work-items.md#p4--deferred-until-p3s-numbers-long-one-is-a-wipe) |
| meta-scale | **W66** · metadata leadership distribution and topology-independent routing | scalability | open — Oct 10 AMD unhosted namespace-coordinator read routing repaired and direct/buffered 128-race cold gates pass ([checkpoint](amd-non-rdma-20261010.md#w66--unresolved-namespace-coordinator-routing)); four-voter host/engine, persistence/failover and staged leadership distribution pass; prolonged preferred-peer loss has no read errors/term churn; default two-group topology unchanged; D36 commit authority/recovery choice, routing/migration and scale remain | [evidence, phases and gates](metadata-scaling.md) |
| tiny-raft | **W93** · broader durable Raft group commit | performance | closed Oct 10 — AMD + repeated NUC batching, read-tail, atomic recovery, fault and cold gates pass; tuning opt-in | [acceptance](w93-atomic-apply-20261010.md) |
| tiny-create | **W94** · atomic CREATE plus open lease | performance / lifecycle | **Closed Oct 11: CREATE/adoption scope approved by user.** Measured lease/throughput savings, actual allocation/competing-OPEN barriers, both-mode full native/live/fault/cold and sustained admission acceptance pass. Broader production flush/PUT-ticket integration remains W72/W86/W88; no D25 activation | [closure](amd-feedback-integration-20261011.md#w94--createadoption-closed-by-user-scoped-decision) · [work item](../backlog/work-items.md#w94) |
| scratch-expiry | **W90** · scratch-file expiry metadata and safe lifecycle | feature | closed D33 initial scope and inspection/status follow-up; normal 24/24 and fault 25/25 expiry gates PASS; 30-minute two-mount soak, 1,644 expirations/six restarts PASS on NUC TCP/direct, `.efs` disabled, no containers/packing; each core 216/216 and peer 67/67; readable duration arguments and 30,000-file backlog/restart/off-on PASS; Oct 13 bounded-history/cadence fixes: 212–218s per 10,000 and 60–72 MiB sampled node RSS; follow-up p95 reduced 20–40% with physical drain 153–165s; maximum outliers remain | [work item](../backlog/work-items.md#w90) · [design](scratch-expiry-interface.md) · [implementation](expiry-implementation-20261012.md) · [POSIX expiry runner](posix-expiry-suite.md) · [inspection and soak](expiry-inspection-soak-20261012.md) · [large backlog and arguments](expiry-backlog-20261012.md) · [RSS and cleanup](expiry-rss-cleanup-20261013.md) · [foreground fairness](expiry-foreground-latency-20261013.md) |
| virtual-commands | **W91** · directory-local virtual `.efs/` command interface | usability feature | design accepted D34; `.efs/` commands and bounded query paths; implementation/compatibility gates remain | [work item](../backlog/work-items.md#w91) · [design](scratch-expiry-interface.md) · [implementation](expiry-implementation-20261012.md) |
| tiny-data | **W95** · tiny-file data persistence and packing | performance experiment | partial — independent indexed packing, publication/GC fault and 24-sample AMD churn gates pass; retained raw-field/index-cost review completed ([review](amd-sustained-20261011.md#w95-retained-amd-experiment-review-completed)); P4.1 production-format and migration decision remain; [TCP/direct with .efs disabled](packing-tcp-direct-20261012.md): exact8151debe normal/fault native, nineteen focused/actual-core sanitizer gates, three quiet1512/1512+67/67 rounds and focused fsync-crash/cold all PASS; directory-timeout acceptance closed on AMD; [next](packing-tcp-direct-20261012.md#next-work-toward-w95-closure--oct-13): matched A/B, lifecycle evidence audit/gaps, P4.1 format/rollout decision; fresh-store-only opt-in policy recommended, unapproved | [work item](../backlog/work-items.md#w95) |
| tiny-kv | **W96** · bounded KV WAL preallocation | performance experiment | Closed Oct11: isolated SATA/NVMe repeated matrix, exact replay/reset/reservation, full native/POSIX/peer/fault/cold and WAL sanitizer gates pass; conditional benefit and SATA tails retained | [closure](devel-nuc-closure-20261010.md) |
| decisions | **D28** client-death loss policy · **D29** durable publication receipts · **D30** bounded admission/pacing | accepted design | accepted Oct 8: salvage deferred; D29/W88 implementation first, then D30/W23/W89; implementation and acceptance remain open | [contracts and gates](decisions.md#d28d30--accepted-oct-8-2026) |
| op1 | **W70** · shrink-quota falsely reports background migration | operator observability bug | Closed Oct11: current NUC direct/buffered × one/two-root accepted/refused reductions, truthful running quota and exact retained bytes pass; no migration/persistence claim | [closure](devel-nuc-closure-20261010.md) |

### Source findings awaiting triage

These review findings have canonical identities and gates below; their
positions do not assign an execution order or override the active handoff.

| review ID | finding | classification | review state | evidence / gate |
| --- | --- | --- | --- | --- |
| spec1 | **W71** · fragment PUT lacks a production persistence barrier | durability correctness | implemented barrier — file and pathname fsync before production PUT ACK; fault/live gates pass, hardware power-loss gate owed | [evidence and gate](../backlog/work-items.md#w71) |
| spec2 | **W72** · production mount session lifecycle / I23 integration | fencing integration | partial — full mount UUID/epoch shared by append/HOLD/FLOCK; retained FUSE CREATE/adoption and sequenced OPEN/CLOSE plus proven-revocation recovery pass AMD direct/TCP; namespace shard admission and fenced replay/effect checks are AMD gated; Oct 11 merged REGISTER verdict mismatch repaired; before/after proof retained; REGISTER/ESTABLISH verdict regressions and final captured `90a47313` full normal/fault native plus direct/buffered TCP cold namespace/APPEND/sync gates pass ([follow-up](amd-integration-20261011.md#w72-register-verdict-mismatch--reproduced-and-repaired)); REPORT mapping/activation/completion session fences pass AMD direct/TCP; staged typed per-publication recovery/newer-range ownership passes AMD; production flush/PUT-ticket integration remain open | [evidence and gate](../backlog/work-items.md#w72) · [Oct9 mounted-client gates](dependency-package-20261009.md#terminal-retained-identity-recovery--implementedgated) |
| spec3 | **W73** · synchronous application-write publication | integration | Closed supported software integration: application sync and mount-sync direct/buffered × TCP/RXE gates accepted; hardware power-loss acceptance remains W71 ([Oct9 closure](amd-rxe-gate-closure-20261009.md)) | [evidence and gate](../backlog/work-items.md#w73) |
| spec4 | **W74** · protection profiles, debt and automatic repair | recovery feature gap | fixed 2+1; inactive journal/fence/readonly authority tested; physical repair/profile cutover remain gaps | [evidence and gate](../backlog/work-items.md#w74) |
| spec5 | **W75** · fragment integrity trust / missing checksum fallback | integrity correctness | missing/corrupt evidence fails closed; captured metadata digest and PUT validation gates pass on NUC; identity-bound format remains open | [evidence and gate](../backlog/work-items.md#w75) |
| spec6 | **W76** · opid table exhaustion sends unprotected namespace retries | conditional correctness | Closed: bounded identity admission refuses saturation safely; Oct9 reserves receipt capacity with16 unacknowledged positions (512 tracker slots); all four lost-reply mutations replay across leader death/restart with one effect | [evidence and gate](../backlog/work-items.md#w76) |
| spec7 | **W77** · atomic multi-chunk public publication/observation | integration gap | per-group REPORT/per-chunk publication; request-wide decision/read-validation gate unestablished | [evidence and gate](../backlog/work-items.md#w77) |
| op2 | **W78** · server wrapper trusts PID-file process identity | conditional operator correctness | Closed Oct11: actual NUC startup/stop retains unrelated processes; absent/dead/invalid/wrong-storage/stale-start and recorder ownership gates pass; shutdown remains W65 | [closure](devel-nuc-closure-20261010.md) |
| op3 | **W79** · mkfs help/name argument disagrees with single-export handler | operator observability | Closed Oct11: aliases/help/argument refusals, ignored label, consumed successful replies and explicit retry preserve namespace/salt in both NUC I/O modes | [closure](devel-nuc-closure-20261010.md) |
| op4 | **W80** · efs-query reports retired zero-filled statistics as totals | operator observability | Closed Oct11 for retired endpoint: nonempty namespace normal/raw, help, legacy and malformed replies pass; mounted stats/scale remain W92 | [closure](devel-nuc-closure-20261010.md) |
| scale1 | **W81** · automatic directory spreading lacks a pressure trigger | scalability feature gap | size trigger/migrator exist; pressure policy remains unspecified and unwired | [evidence and gate](../backlog/work-items.md#w81) |
| proto1 | **W82** · stale completed ReadIndex authority survives leader isolation | read correctness | Closed: stale LOOKUP/GETATTR/session/publication/transaction views and configuration partition gates pass; coordinator callback refuses untrusted absence | [evidence and gate](../backlog/work-items.md#w82) |
| proto2 | **W83** · transaction decision records lack safe retirement | metadata lifecycle gap | resolve/drop remove participant records; no decision acknowledgement/GC path found | [evidence and gate](../backlog/work-items.md#w83) |
| proto3 | **W84** · bounded namespace transactions | namespace completeness | Closed for accepted D32 144-participant envelope: distinct EOVERFLOW, prompt atomic refusal diagnostics, mixed boundaries, full64-lane removal/replacement, exact144 PREPARE/COMMIT cold recovery and AMD normal/fault POSIX/GC gates pass; larger-operation protocol deferred | [closure and limits](w84-closure-20261009.md) |
| storage1 | **W85** · path-hint eviction loses ambiguous PUT retry history | conditional accounting/storage correctness | Closed: ambiguous PUT placement retained in 389a8baf; direct/buffered real lost-ACK collision and two-root physical/quota/restart gates pass | [evidence and gate](../backlog/work-items.md#w85) |
| gc-own | **W86** · durable PUT ticket integration | feature / safe reclamation | metadata ledger, budgeted retained admission and ticket-bound durable publication receipts implemented (`22c8c1f9`, `70daa5a4`); AMD same/cross-group lost replies, leader death, cold replay, collision and revocation pass; default PUT/flush, storage fences and all-member abandoned collection remain staged | [evidence and gate](../backlog/work-items.md#w86) · [D31](decisions.md#d31--recorded-abandoned-upload-policy) |
| append-load | **W87** · buffered concurrent append loses records under load | correctness investigation | failure recorded in private GC fixture; isolated repeats pass; full traced acceptance owed | [evidence and gate](../backlog/work-items.md#w87) |
| replay-span | **W88** · folded span retry resurrects after bounded history eviction | publication/retry correctness | partial — actual cross-group typed cache/host replay preserves peer bytes and cold mappings on AMD direct/TCP; production flush/ticket integration and full replay gates remain open | [evidence and gate](../backlog/work-items.md#w88) · [Oct9 checkpoint](dependency-package-20261009.md#retained-typed-publication-writeback--staged-amd-gated) |
| measure-fields | **W89** · W23 TSV field packing shifts derived metrics | verification correctness | Closed measurement defect: explicit schema and raw-field identity regressions pass; fresh AMD real-cap/quorum/recovery/cold-value gate passes (276 observations); D30/W23 bounds remain open | [closure and limits](amd-non-rdma-20261010.md#w89) |

The [specification/public-path review](spec-implementation.md) separates accepted
contracts from current capabilities (W71–W77). These findings add no deployment
acceptance or new priority order.

## Maintaining this index

[Project evidence rule](../../AGENTS.md#evidence-and-reproducibility): commit summaries and reproducible commands; keep raw captures outside Git. Existing raw evidence is migrated only after its conclusions and reproduction steps are preserved.

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

## October 7 documentation-review checkpoint

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


## Intel follow-ups — W100–W106 (indexed Oct 11, 2026)

Historical Intel results below are source-specific, not current execution or acceptance. The [acceptance ledger](intel-deployment-acceptance-20261011.md) retains later c2 failures. Superseded `0ae7e480` retains the certified APPEND cleanup repair and corrects exact salted namespace and physical compactor-drain fixtures. All corrected namespace/compactor live reviews pass on both hosts; the confirmed worker allocation errno repair passes all six focused direct/buffered fault fixtures on each host before fresh complete qualification with six standalone cold-safety gates and TCP/direct and TCP/buffered acceptance on both Intel hosts; preceding real namespace/compactor failures remain retained. Its four targeted live APPEND reviews pass. The preceding retained-CREATE candidate passed all four 214/214 POSIX cores but failed Intel2 APPEND cleanup; its unfinished remainder remains required. The superseded 1,000 ms trial passed 110 main checks and 30 canonical rounds on each host but failed the supplemental CREATE injection. Earlier candidates and partial checkpoints are archived; no partial or failed trial establishes deployment acceptance.

This index adds traceable IDs to the Intel acceptance follow-ups; it does not change the existing queue order or accepted design decisions. Existing sustained tiny-file performance remains W97, protection/repair W74/W66, durability W71/W73 and capacity W99.

| ID | Task | Recorded state |
| --- | --- | --- |
| [W100](../backlog/work-items.md#w100) | Intel concurrent POSIX timeouts and mixed-workload EIO | Correctness/performance investigation; open. The preceding trial passed 110 main and 30 canonical gates. Cleanup-repaired `3d69edb4` passed four 214/214 cores but later namespace/compactor fixtures failed; `5abf8e7b` repairs their setup/oracles. Fresh complete fault and production acceptance remain pending. |
| [W101](../backlog/work-items.md#w101) | Complete and verify the Intel two-million-file workload | Operational acceptance; in progress. |
| [W102](../backlog/work-items.md#w102) | Complete Intel2 private peer and GC acceptance | Validation follow-up; in progress, aggregate acceptance remains open. |
| [W103](../backlog/work-items.md#w103) | Investigate intermittent Intel persistence and stats timeouts | Liveness/performance investigation; open. |
| [W104](../backlog/work-items.md#w104) | Decide and gate writable shared mmap support | Feature scope/design follow-up; deferred, currently unsupported by specification. |
| [W105](../backlog/work-items.md#w105) | Stable NFS file handles across EFS client restarts | Gateway acceptance/correctness investigation; open. |
| [W106](../backlog/work-items.md#w106) | RDMA acceptance: AMD software / orcd-test hardware IB/RDMA | Open validation; test placement decided. |

Pending choices retain their existing homes: W99 cache-only pressure escalation and numeric thresholds; W54 additional live-reference GC guard; W66/W74 metadata mechanism under accepted D36. W104 separately records the deferred shared-mmap scope choice. No new D number or policy acceptance is implied.

**RDMA test placement — user instruction, Oct 11, 2026:** All RDMA work is tested on AMD using software RDMA, or on `orcd-test` using hardware IB/RDMA, according to the claim being validated. Keep software and hardware verdicts separate. Intel continues to provide TCP acceptance. This applies to all RDMA gates, including older W-item hardware obligations.


## ORCD communication follow-ups — W109/W110

| Item | Assigned cluster | State / next action |
| --- | --- | --- |
| [W109](../backlog/work-items.md#w109) | orcd-test | Open: correlate metadata apply/query contention with original EBUSY/timeouts; preserve deadlines and full-load gates. |
| [W110](../backlog/work-items.md#w110) | orcd-test | Open: distinguish EFS retries/loss from TCP retransmission and native-IB errors; establish per-op correlation and recovery gates. |

[Capture summary and closure plans](orcd-communication-20261011.md): two EFS TCP
retransmissions, unchanged IB error counters, 22–50 ms metadata apply waits and
substantial query-build CPU. No transport root cause or gate closure is claimed.
W95 packing and W106 broader hardware acceptance remain separate work.

[Deeper investigation and repair hypotheses](orcd-communication-hypotheses-20261011.md):
prioritize the 1,024-change query fallback; distinguish compaction correlation,
GC partial-progress BUSY and TCP duplicate probes from established failures.
W109/W110 remain open on orcd-test.


## Mac verification of W109's changed-inode fallback

[Mac query delta evidence](mac-query-delta-20261011.md): `devel-mac` confirms
the exact 1,024-ID cliff, removes it with a bounded candidate journal, and
optimizes inode diff scans. Mac buffered/TCP deployment and unit gates passed.
Installed-namespace bootstrap and broader live gates retain their own verdicts;
no ORCD acceptance or W109 closure is implied.

[Latest ORCD branch integration and rollout](orcd-merged-rollout-20261011.md):
all current development branches merged and `c52a4643` deployed to eleven hosts.
Full Linux units and retained file samples pass; loaded POSIX/query gates remain
failed, and the fresh tiny-file benchmark is running.

- [NUC W52/W97 deployed continuation](nuc-w52-w97-resume-20261011.md): preserved-store rollout, repeated phase/storage latency measurements, staged bounded completion transactions and pending placement controls.

## Current alpha release qualification

`v0.3.2-pre-alpha` is tagged at b4f26e9d; main points to that tagged commit.
The release branch also contains subsequent documentation/evidence housekeeping.
No push has been performed. The documentation-only ORCD integration preserves
identical qualified product/test inputs: AMD ordinary files/TCP/direct and
TCP/buffered pass the selected full native/fault/sanitizer and seven-client
core/peer/cold/reclamation gates; supplemental ORCD native RDMA/direct evidence
is reviewed separately. See [v0.3.2 review](v032-integration-review-20261012.md),
[v0.3.1 direct qualification](v031-qualification-20261012.md) and
[buffered qualification](v031-buffered-qualification-20261012.md).

Packing, containers, `.efs` commands/query cache and dedicated repair/recovery
remain excluded from this release's qualification. Their open work items and
retained failures are not closed by ordinary-file acceptance. W92 remains open
with refused capacity handoffs recorded in [its closure ledger](query-cache-closure-20261010.md).
Packing diagnostics remain in [the diagnostic ledger](v030-query-stage-diagnostic-20261012.md);
W74 repair gaps remain in [the design ledger](nuc-repair-gap-design-20261012.md).

The [archived release chronology](../archive/status-20261012/release-index-chronology-20261012.md)
preserves preceding failed candidates, historical RUNNING/STOP instructions and
scope changes. It does not authorize further cluster or agent work.
See [evidence cleanup inventory](../../results/measure/20261012-evidence-cleanup/README.md)
for consolidated duplicate paths and explicitly pruned historical captures.
