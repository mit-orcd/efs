# v0.3.0-pre-alpha integration and acceptance

**Latest exact-source result:** `4e239d029029b8105a6e07f0e69a26fdc449de32` passes AMD ordinary-files qualification after the deep-directory fix; .efs remain disabled. [Report](v030-deep-directory-fix-20261012.md). No push or tag. Earlier failed checkpoints below are historical.

**Latest alpha scope:** ordinary files/TCP/direct only, `EFS_COMMANDS=0`, `EFS_COMMANDS_SERVER=0`. `.efs`, RDMA, buffered and dedicated repair/recovery are excluded.

Latest integrated-source verdict: **1e3a46384e897a47751871b15e82ead86c9d0abd** is not accepted. Native/fault/sanitizers, peer/teardown and ordinary cold/reclamation PASS; seven-client deep-directory30s failures remain. [Final qualification](v030-final-qualification-1e3a4638-20261012.md).

Status: qualification STOPPED/checkpointed; incomplete and unaccepted. Planned alpha scope is TCP/direct I/O/files with `.efs` disabled; dedicated repair/recovery deferred. Nothing tagged or pushed. User authorization, Oct 11: create an isolated integration
branch, coordinate all agents, add and test all improvements, prioritize
code-review findings. Release owner: agent 2 / AMD.
[Operator-facing candidate notes](../using/v030-candidate.md) summarize included
changes, opt-in flags, upgrade constraints and unaccepted scopes.

Branch: `release-v0.3.0-pre-alpha`, created from `devel` `48a2719d`.
Other agents retain their branches and cluster ownership. Shared `devel` and
`main` stay unchanged during integration. Main currently points to `53be1da7`;
latest existing tag is `v0.2.2-pre-alpha`. Only a frozen, accepted release
revision may be promoted and tagged. Push remains outside business hours in
America/New_York; no schedule or push is created by this checkpoint.

## Current authoritative checkpoint

Current pending frozen candidate is `03bd89fb7de56c2c6af44ab3bd26c54caf4b8274`. Qualification remains stopped. The latest human decision excludes `.efs` features from the planned alpha; clients and daemons must disable the interface and query-cache work explicitly. [Scope and gate contract](v030-release-scope-20261012.md) and [source-specific results](v030-tcp-direct-qualification-20261012.md) govern. AMD native gates pass, but broad live tests with `.efs` enabled failed; they do not qualify the new disabled-feature scope. Deep paths and ordinary I/O refusals remain unresolved.

The source410/frozen28 and multi-mode rollout statements below are historical;
older passes do not qualify the next merged candidate. No resumption, push, tag
or shared devel/main promotion is authorized by documenting this decision.

## Agent checkpoint contract (on human-authorized resumption)

Commit logical work before submission. Supply exact checkpoint and tested
source/binary SHA, changes since devel, passed/failed/not-run gates and evidence,
deployed revision/configuration, wire/on-disk/upgrade implications, remaining
blockers and uncommitted work. Continue assigned work on the owner branch and
cluster only after explicit resumption. Do not merge/push shared devel/main, replace another owner's binaries,
wipe retained data or interrupt another owner's workload. Later checkpoints are
integrated explicitly; moving branch tips are not test identities.

## Historical implementation handoff ledger

| Owner / branch | Submitted checkpoint | Integration state | Acceptance limits |
| --- | --- | --- | --- |
| AMD / devel-amd | `19f7efb3` | merged in `eff8d05a` | W94 CREATE/adoption closed; normal/fault native and both-mode full lifecycle pass. W97 repeated pressure comparisons continue; feedback stays opt-in. |
| ORCD / devel-orcd | code `79551691`, evidence `8d07b45a` | base merged in `ea5f5dc4`; exact-rejection repair in `8a7fd9be` | Full native and private lifecycle evidence; mounted concurrent POSIX times out. Pinned-stats, cold/shared-GC pass on ORCD; concurrent retest still times out. Bounded receipt-retirement `627d48ba` merged in `d1197381`; mounted acceptance remains pending. |
| Intel / devel-intel | `5f03ec76` plus fixture corrections through `5abf8e7b` (C source `3d69edb4`) | base merged in `22056d9b`; corrected fixtures integrated | Both-host native and complete direct/buffered private fixtures pass; supplemental fault matrix, remaining canonical/cold gates and retained-data deployment still pending. Production remains `7bde5f43`. |
| Review / devel-code-review | reports `1b0c6b7c`, fixes through `3ac828cd` | base merged in `5e381545`; further repairs in `9260d8a1` and `9acea823` | Reproduced use-after-free fixed; focused merged regression passes. Capacity accounting, recorder reap and incoming-chunk re-dirty fixes included; private find/flush/walker cleanup included. Full merged/Linux/live acceptance required. |
| NUC / devel-nuc | revised `ba73f304` | merged in `5cbe0ce4` | APPEND_CATCHUP tag52; focused merged gates pass. New-tag native rebuild, live/fault/cold/20k and measured W97 acceptance pending. Production remains `f52cd2dd-nuc`; preserved writer must finish before restart. |
| Mac / devel-mac | `b2c01fa0` | reconciled in `93bb30de`; complete-server compile corrections in `29b33f80` | Request-local authority masks and combined registration retained alongside ordered NUC/Intel reservation guards, tag52 completion and Mac prune/close cohorts. Single-shard durable tag53 is now integrated from `a6885603`, with CREATE adoption and coverage-aware routing (`f842cd37`, `0f869394`); full merged acceptance pending. Owner review and merged gates pending. Concurrent CREATE timing still open. |

Submitted checkpoints are implementation inputs, not release certification.
Some are already contained by another branch; Git ancestry and source diffs
must determine overlap. The detached ancestry audit confirms bench, known-bugs,
find-stats, RDMA, gate-closure, compaction-pressure and primary checkout commits
are already contained. Old find-stats has four modified historical log files
and no uncommitted code; retain them untouched. No improvement is silently
dropped merely because its branch name was retired.

## Priority: code-review correctness

[Review disposition](../reviews/release-disposition-20261011.md) protects active
query consumers and public/native/wire compatibility. Integrate bounded
correctness fixes first: rollup allocation ownership, retained queue accounting
and recorder stop/reap. Each needs focused failure/boundary tests plus relevant
merged native and mounted gates. Broad refactors and API deletion must have
explicit consumer/contract validation; a base-specific unused-symbol scan is
not sufficient. Retain the RAM query oracle.

## Integration conflicts and protocol allocation

Command/request tags45–51 are reserved. NUC's undeployed tag45 collision
was corrected to APPEND_CATCHUP52 in `ba73f304`; tag45 native results are historical
and cannot certify52. Mac single-shard APPEND now uses53 in the merged candidate; do not reuse that
number elsewhere. Acceptance and rollout remain separately gated. Run the actual canonical-header
namespace gate on every merged revision. All voting servers must understand new
commands before admission; preserve native client/server layout compatibility
and document cold replay/rollback restrictions before rollout.

Resolved ORCD harness conflict retains both command-only and synthetic-pressure
options. Generated architecture is regenerated from source documents. NUC
persistence-refusal merge preserves every APPEND_CATCHUP
IO/NOMEM retry case; neither may advance past undurable state. The append
PREPARE regression retains physical-cohort checks while obsolete REPORT
completion assertions move to the actual new catch-up host regression.
Raw evidence is preserved byte-for-byte, including historical failures and
log whitespace; source/current-doc whitespace gates are checked separately.

## Release acceptance matrix and promotion

1. Finish checkpoint integration and reconcile owner-reported conflicts; audit
   committed detached worktrees and pending working changes. Record each
   improvement as included, pending or explicitly experimental, with a gate.
2. Freeze exact source/build inputs; run full Linux normal and fault native,
   relevant sanitizers/memory/namespace/durability regressions and doc gates.
3. Gate the same candidate in buffered/direct TCP on assigned clusters; software
   RDMA on AMD and hardware RDMA on ORCD only. Owner coordination precedes any
   shared-cluster deployment. Keep four-daemons/one-host and independent-host
   evidence distinct. VM storage timing is not hardware performance proof.
4. Complete full POSIX, peer/concurrent/extras, cold acknowledged bytes,
   retry/fencing/publication, fault recovery, physical GC and retained-data
   verification. Track normal
   service restoration and source/binary/configuration identity independently.
5. Summarize accepted improvements, opt-in flags, known unsupported features,
   blockers and wire/store/client/server upgrade/rollback requirements in release
   notes and new-user/operator/developer docs. Current fixed2+1 data/three-voter
   metadata implementation must not be described as D36's completed four-node
   two-loss protection guarantee.
6. Only after merged acceptance, promote that exact commit to devel/main, create
   annotated `v0.3.0-pre-alpha`, then push main/tag in the authorized out-of-hours
   window. No failed gate, branch-native pass or earlier deployment
   proves that the final merged release passes.

## Initial release-branch gates (historical)

Focused merged command namespace, ordered APPEND PREPARE/cohort semantics,
APPEND_CATCHUP20k shape/refusal and rollup allocation-failure regressions pass.
Five architecture checks pass after regeneration. This is not full Linux,
mounted, performance or release acceptance. The release ledger will be updated
with exact final test/build identities and raw evidence before promotion.

The integration checkpoint evidence (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-v030-release-integration/focused-checkpoint.json`)
records ten passed local focused gates. The detached ancestry audit (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-v030-release-integration/detached-ancestry.json`)
records exact worktree commits and tracked-change counts.

## AMD merged qualification — October 11

The initial `9acea823` fault build caught a duplicated namespace wrapper and
two missing APPEND cleanup contexts. `29b33f80` repairs complete-server
compilation; focused authority/ordered PREPARE tests pass. Its full suite then
caught missing LOOKUP test dependencies for speculative pair reads. `063bc8c4`
updates that regression, including stale hint and BUSY fallback assertions.
The next full run reached report-pressure and found a stale session-proof
helper extraction. The regression now covers the actual request-local bitmap
helper, foreign-group fresh reads and mutable epoch/state; combined
REGISTER_ESTABLISH business/storage verdict coverage is also included. These
are retained failed gate verdicts, not release acceptance.

AMD builds use isolated `/data1/efs/release-v030-20261011`; normal stores and
installed binaries remain intact. Preceding W97 comparisons and service
restoration exited0 before release builds began.

The merged native `bdee0d1c` run reached the late namespace-owner regression
but failed obsolete REGISTER-proof shortcut expectations. `f1d25b3e` retains
exact production proof-emitter error/fencing coverage and expands the actual
fresh-admission helper tests across command refusals, floor errors and fencing.
ORCD `627d48ba` is included in frozen candidate `d1197381`; full normal/fault
AMD native tests are running. Input hashes cover929 build/test/documentation files, with no
synchronization mismatches. NUC reports both tag52 native builds pass on
`ba73f304`; new-tag live/cold/20k acceptance remains pending.

## Full merged native verdict

Frozen `d11973815676657e53039615634a9ac4b14c927c` passes `make -j8 all test`
in both normal and EFS_FAULTS=1 builds on AMD. Both native exit markers and
the controlling build exit are0. All929 hashed build/test/documentation inputs
match the frozen source. Native logs, source verification and binary identities (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-v030-merged-native/d1197381/acceptance-summary.json`)
retain the exact artifact identity. Normal binaries identify as
`d1197381-release-normal`; fault artifacts are separate. Final sanitizer and later parser checkpoint acceptance are
still pending. This native pass does not close ORCD's quiet concurrent
directory/query timeouts or certify a tagged release.

## Seven-client workload on AMD

Two mounts/four daemons on one AMD host do not certify ORCD's seven-client
multihost workload. ORCD `627d48ba` fails even with fresh stores: directory/query
timeouts and canceled workers remain, despite passing cold/shared-GC/stats.
Prior failing cases include deep nesting, many files, unusual names and virtual
stats/find. Client RPC waits, server ReadIndex/apply waits and query publication
refusal followed by a9.75s build are observations, not a proven single cause.

The reviewed parser fix `7a8ed4a5` is merged; it rejects signed, malformed and
overflowing ports across six callers. Candidate `f1c322e1` adds seven concurrent
mounted clients running unchanged221-case suites/deadlines before peer/cold/GC
checks. Full normal/fault native plus allocation/pipeline/parser sanitizers
precede direct and buffered seven-client runs. Completed native/sanitizer and
seven-client outcomes are recorded below.

## Native qualification of f1c322e1, bbb84e41 and f0bb0208

Frozen `f1c322e1`, `bbb84e41` and `f0bb0208` each pass full Linux normal and
fault-native suites and their ASan/UBSan gates
(f1c322e1 (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-v030-merged-native/f1c322e1/acceptance-summary.json`),
bbb84e41 (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-v030-merged-native/bbb84e41/acceptance-summary.json`),
f0bb0208 (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-v030-merged-native/f0bb0208/acceptance-summary.json`)).
The original higher-pressure seven-client/jobs16 gate reproduces the ORCD
directory/query failures on AMD. Nothing is tagged or pushed.

### Authority and Mac reconciliation: latest merged candidate

The release branch now includes submitted `b579bee2`: Mac
single-shard APPEND tag53, CREATE adoption and coverage-aware leader routing.
General cross-shard APPEND retains ordered session/inode guards and tag52
completion. All voters must understand tag53 before admitting those commands;
an older binary is not a safe rollback after durable new commands.

The earlier `66c237c8` full native attempt failed in an extracted retry fixture;
`1c6cc5b6` corrects its dependency mock. The final merged source requires new
normal/fault native, sanitizer and quiet seven-client/jobs16 direct/buffered
qualification on AMD. No timeout budget, authority predicate or durability
barrier has been relaxed. ORCD reports its own `b579bee2` full native pass,
which does not substitute for this release qualification.

### Merged native qualification: cfa103dc

Frozen `cfa103dc` passes full AMD Linux `make all test` in normal and
`EFS_FAULTS=1` builds plus eight ASan/UBSan gates, including the actual
read-only authority NET fallback caller. All 1,325 source/linked-evidence
inputs verify unchanged after qualification. Exact versions and normal/fault
binary hashes are retained with native gates and logs (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-v030-merged-native/cfa103dc/acceptance-summary.json`).

The earlier `9c65c893` attempt failed the real-storage test's obsolete NET
expectation. `cfa103dc` corrects that assertion and expands real buffered/direct
coverage: NET tries fresh legacy admission, a legacy NET propagates without
FileID checks or writes, and stale phase/epoch refuse. Both complete suites
then pass. Independent review disposition `ba33c3f1` reports no additional
concrete authority/batch finding in its bounded source/control-flow review.

The same quiet seven-client/jobs16 direct/buffered gate ran after all
qualification passes and failed both modes. This native result does not certify
the unfinished NUC/Intel owner gates; no release promotion or push.

AMD `cfa103dc` quiet jobs16: every client in both modes reports
209 PASS/4 FAIL/1 SKIP/7 NOTRUN (44.3–45.8s). Directory deep nesting times out,
many-files is killed, stats times out with a live worker and find is blocked
after stats. Extra-client stop refuses the busy mount; later cold bytes and
physical GC are NOTRUN. Fixture cleanup and normal mount restoration pass.
ORCD's independent directory improvement is therefore not reproduced on AMD.

A separate diagnostic captures all four daemons and seven FUSE processes for
35s with userspace cpu-clock, frame-pointer unwinding at49Hz, no extra runtime
observation logging: 500 samples, zero lost. Caller stacks now resolve Raft
apply, transaction resolution, KV batch and memmove; many captured threads
wait in futex/socket paths. This bounded CPU sample does not establish the
wall-time blocker or justify removing durability barriers.
Normal mounts are restored after this diagnostic as well.

ORCD submitted `bbe8b475` for bounded transaction-cut recapture (at most four
complete fresh attempts within the existing deadline) and avoiding duplicate
first-page barriers. Query owner investigates coherent-cut/builder starvation;
these efforts are coordinated. Original30s case budgets and all authority,
import and transaction fences remain required. The implementation is integrated in `be8169d4`; production-function
recapture/page regressions pass normally and under ASan/UBSan. Full merged
AMD qualification and same quiet pressure gates are next.

### d9d39acc qualification, buffered improvement and W112

The bounded query candidate passes full merged AMD normal/fault `all test` and
ten sanitizer gates; all1,330 inputs are unchanged and27 retrieved native
artifacts verify byte-for-byte. Exact full gates (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-v030-merged-native/d9d39acc/acceptance-summary.json`).
Independent [query safety review](../reviews/query-cut-retry-20261011.md) finds
no additional concrete defect within its bounded scope.

Buffered seven-client/jobs16 now passes every client220/0/1/0, exact cold
bytes, physical reclamation within180s and metadata queue drain.
Direct still fails: six209/4/1/7 and one210/3/1/7, with directory/query cases
and later cold/GC NOTRUN.
Normal mounts restore successfully.

Ordinary-write pressure controls pass all seven suites in both modes on the
same binaries/jobs16/30s limits, with explicit create/read-only warm-up. These
are case-only controls, not cold/GC or identical mixed-mutation warm-up proof.
The initial helper-import failure is not an I/O result. Mixed-mutation OFF
controls instead expose [W112](w112-full-overwrite-replay-20261012.md): ordinary
full overwrite loses first-chunk bytes despite successful fsync, including
cold reads. It blocks release independently of the pressure timeouts. A bounded
actual-code repair and focused regressions pass; full/live qualification is next.

### W112 qualified; pressure remains open

Frozen `02b9918b` passes complete AMD normal/fault native and eleven sanitizer
gates with all1,335 source/linked inputs unchanged. Both ordinary-overwrite
modes pass exact primary/peer/cold bytes for all four cases, physical cleanup
and normal mount restoration. Independent review reproduces baseline loss and
passes ownership boundary probes. [W112 closure](w112-full-overwrite-replay-20261012.md)
is limited to that reproduced defect. Direct seven-client/jobs16 pressure still
fails; this is not release acceptance. New ORCD follower-read deadline and
Mac append completion checkpoints require reconciliation and full qualification.

### Read deadline review and latest Mac reconciliation

`b1632feb` integrates bounded read-only forwarded RPC waits through pool/connect/
TCP and RDMA receive handling, and fixed reusable helpers for independent hosted
group proofs. Independent review `67687dce` reproduces continued routing retries
after an inherited100ms scope expires; a read-only loop guard and actual-wrapper/
loop regression now stop expired attempts. Mutation uncertainty handling remains
unchanged. [Review and retained baseline probe](../reviews/read-deadlines-20261011/README.md).

Mac `e798c2aa` adds eligible single-shard durable tag54 append completion and
coverage routing without recursive host mutex acquisition. Merge conflicts retain
NUC ordered reservation guards, tag52 catch-up charging and all NUC/Mac regression
cases; only eligible completion uses tag54. All voters must understand53/54 before
admitting these commands; old binaries are unsafe rollback targets. New frozen
merged full AMD qualification and both-mode pressure are pending.

Latest `02b9918b` pressure gate fails both modes (direct six209/4/1/7, one210/3/1/7;
buffered seven209/4/1/7), cold/GC NOTRUN after busy extra-client stop, restore0.
Frozen `d26e6874` full merged AMD qualification is active after correcting an
imported Mac fixture mismatch in `6819c127`. [Manager handoff](v030-amd-handoff-20261012.md)
records the missing Mac client adoption prerequisite and exact pending gates.

### Latest frozen merged qualification d26e6874

Complete AMD normal/fault native `all test` and sixteen focused ASan/UBSan gates
PASS; all1,469 frozen source/linked inputs remain unchanged. Native results and
normal/fault binary identities (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261012-v030-merged-native/d26e6874/acceptance-summary.json`)
include33 retrieved hash-verified artifacts. Both direct/buffered ordinary
overwrite gates pass all four primary/peer/cold exact SHA256 oracles and physical/
queue cleanup, restore0; raw both-mode evidence (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261012-v030-ordinary-overwrite/d26e6874`).
Original seven-client/jobs16 pressure fails both modes: direct seven209/4/1/7;
buffered six209/4/1/7, one210/3/1/7 (PASS/FAIL/SKIP/NOTRUN).
Busy extra-client stop aborts later cold/GC phases; round1, restore0.
No release acceptance.

Frozen Mac/ORCD/review/NUC owner ancestry and evidence are reconciled; root core
and actual live fixture match d26. Only NUC measure promotion recipes additionally
require ordinary gates before acceptance. The missing Mac client combined adoption
prerequisite is assigned separately; it is not silently certified by server ancestry.

### Stage diagnosis, client reconciliation and next source

Mac46609ad7 confirms release combined CREATE/OPEN behavior is already present
with independent retained-edge bookkeeping; eight focused exact-source gates
PASS. The earlier missing-client blocker is withdrawn, optional alternate pointer
layout held. [Reconciliation](mac-client-reconciliation-20261012.md).

NUC501205b6, ORCD1bee6f43, Intel801218f3 and Mac e7ebbd25 are merged as
documentation/evidence checkpoints; none confers merged-cluster acceptance.
[ORCD six-wave diagnosis](orcd-query-starvation-disposition-20261012.md) and
[Mac boundary inspection](mac-query-stage-disposition-20261012.md) leave cut
collision, pending transactions, authority and publication lock delay unresolved.

Opt-in `EFS_QUERY_STAGE_TRACE=1` stage records are being prepared for one separate
AMD diagnostic: pin/snapshot, authority, import and transaction-cut refusals with
revision counters; pending-record kind/shard; capture/build/checkpoint completion
and publication lock wait. No names/value bodies are logged. Normal mode remains
quiet; every attempt keeps its existing budgets/refusals/receipts. Actual capture
regressions pass normally, with tracing and under sanitizers. New frozen full
qualification is required before deploying this diagnostic source; no additional
correctness fix or quiet pressure acceptance is claimed.

Instrumentation also records failing paged request stage/root/catalog generation
and separates build completion from publication-lock acquisition. Pending scans
record only shard/kind, not transaction key or values. Both production-function
recapture and page-authority suites pass with tracing off/on and ASan/UBSan;
failed cuts/permissions still produce no successful bytes. No deadlines, retry
count, durability setting, fence or KV mutation was changed.

ORCD880016b1 documents its exact83 normal718-input suite and ten staged builds
PASS; fault/mounted83 remain NOTRUN. NUCf4b594fa confirms preserved f52 service
restoration and old ba73 direct20k cold/physical PASS; repaired101 native failed
documentation-bundle links, not equivalent to merged-source acceptance. Both
documentation/evidence descendants are now integrated.

Bounded review of diagnostic d485afbf finds no added correctness defect; keys
are length-checked, page traces precede session release and all return/fence/
ownership behavior remains. Initial query_lock writer acquisition for lag/pending
update is not timed; a stall there can occur before any capture event. Synchronous
fprintf, including lock-held events, perturbs diagnostic latency. Preserve this
source/results first; add that missing discriminator only if evidence requires
it. Instrumented timing is never quiet acceptance.

[Latest exact-stage evidence](v030-query-stage-diagnostic-20261012.md) establishes
146 cut revision collisions and46.915/48.816s publication gaps under the original
pressure budgets. d485 full normal/fault/sixteen sanitizers and ordinary exact
both-mode cold/cleanup PASS, instrumented pressure FAIL/restored0; no additional
fix or quiet ON/OFF acceptance. Mac/ORCD now own bounded design/admission analysis.

### Second diagnostic checkpoint, 95d3f1b1

[Exact evidence](v030-query-stage-diagnostic-20261012.md#second-frozen-amd-diagnostic-95d3f1b1) records full normal/fault/17 focused sanitizer/real-LSM sanitizer PASS and both-mode ordinary overwrite PASS on unchanged inputs. One instrumented direct seven-client run remains failing. Fourteen roots absent from retained generation 2 explain the logged query refusals; 145 cut mismatches and 47–48 second publication gaps are measured. Foreground deep-nesting causation remains unresolved. Cold/GC NOTRUN, mounts restored, no jobs, no release acceptance. Intel's newly diagnosed older-source namespace fixture incorrectly crossed the participant bound before its ancestry oracle; its test-only repair requires separate integration/qualification, never retroactive acceptance.

### Superseded quiet candidate, b239dc7d

Full normal/fault/18 focused sanitizer/real-LSM sanitizer gates and direct/buffered ordinary warm/peer/cold/physical gates PASS on unchanged frozen inputs. One quiet original-budget direct seven-client attempt remains FAIL (all seven 209/4/1/7). The reviewed shorter cut-only capture retry does not close acceptance; publication gaps remain approximately 46.5 s. Buffered and cold/shared-GC gates NOTRUN, mounts restored/no jobs. [Exact evidence](v030-query-stage-diagnostic-20261012.md#reviewed-candidate-quiet-result-b239dc7d-still-blocked). Next is phase/in-flight evidence, with no unchanged repeat, deadline increase or stale-snapshot fallback.

## Supersession of earlier running/next checkpoints

Sections above retain chronological evidence. Present-tense running, planned retry tuning and next-test statements in those dated checkpoints ended at the human stop. The b239 cut-retry optimization was removed before shared31/c2/410; it is not current release policy. The current checkpoint at the top and owner stop records govern status. No historical PASS transfers to another source or host.
