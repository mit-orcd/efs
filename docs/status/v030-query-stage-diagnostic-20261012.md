# Packing query stage diagnostic — d485afbf

Status: diagnosis, no additional correctness fix; packing closure remains open.
AMD isolated four-daemon TCP/direct fixture, seven clients/jobs16, original30s
case and180s GC budgets. Source `d485afbf`, instrumentation only; normal production
stores/source unchanged. Full normal/fault native and sixteen ASan/UBSan gates
PASS with all1,495 frozen inputs unchanged. Both-mode ordinary overwrite exact
primary/peer/cold/physical gates PASS. Native33, ordinary13 and diagnostic28
retrieved artifacts match their SHA256 manifests.

[Native identity and full gates](../../results/measure/20261012-v030-merged-native/d485afbf/acceptance-summary.json),
[ordinary prerequisites](../../results/measure/20261012-v030-ordinary-overwrite/d485afbf/),
[stage summary and full-raw log hashes](../../results/measure/20261012-v030-query-stage-diagnostic/d485afbf/direct/stage-summary.json).
The full raw daemon logs remain at their absolute remote paths in that summary;
filtered timestamped stage/publish/refresh/disk lines and all seven raw POSIX
records are committed alongside it. No fixture stores were copied.

## Observed stages

| Publisher | Changed transaction cuts | Capture BUSY attempts | Successful checkpoints | Maximum checkpoint attempt ms | Root-authority/page-consume BUSY | Publication writer wait ms |
|---|---:|---:|---:|---:|---:|---:|
| n2 | 71 | 18 | 4 | 10071 | 378 / 378 | 0 |
| n3 | 75 | 18 | 3 | 8950 | 336 / 336 | 0 |

A changed cut means actual captured/current transaction revisions differ and
production code returns BUSY; all attempts keep their new pin/barriers and
existing four-cut bound. One n2 authority BUSY takes402ms. No pending-record
kind/shard refusal was recorded; n3 has one build BUSY250ms not attributed by
this instrumentation. n1/n4 are expected non-query-capable nodes (pin
NOT_PRIMARY), not evidence of a new failure.

n2's successful publications occur at00:47:14.722Z and00:48:01.637Z:46.915s gap.
n3's occur at00:47:14.629Z and00:48:03.445Z:48.816s gap. Both exceed original30s
cases. Catalog checkpoint events include total attempt wall time, not exclusive
fsync cost. Publication-lock wait is zero at millisecond resolution. The earlier
lag-update writer lock is unmeasured; no assertion that every lock wait is zero.

Root-authority is the first `query_auth_dir` plus ancestry refusal in
`query_consume`. It can mean new root missing from old catalog, changed
parent/generation/current binding, current transaction read refusal, permission
or deadline. The enclosing page-consume event repeats that refusal; do not sum
it as an independent failure. We have numeric root and catalog generation in
raw events, not a unique branch reason inside query_auth_dir. Thus observed cut
collision is concrete, but every request's reason is not yet established.

All seven clients209PASS/4FAIL/1unsupported-mmapSKIP/7NOTRUN in44.3–45.3s.
Failed cases: deep nesting timeout; many-files worker killed; stats timeout with
live worker; find blocked after stats. Busy extra-client stop prevents later
packed cold/physical phases; round1, restore0. Foreground directory creation is
not served by the catalog alone. No same-source d485 OFF quiet control or new
foreground stack capture was collected. Prior OFF controls and CPU captures
have different source/workload scope and cannot establish this run's causality.

## Safe next decision and ownership

Mac reviews coherent-cut ordering and individual root-authority branch diagnosis;
ORCD reviews relationship to foreground directory admission/proposal waits.
AMD remains sole implementation/editor and test executor. No fence/pending/import/
authority barrier may be removed on this evidence. In particular simply moving
pin after barriers requires proof about asynchronously applied participants and
cannot be treated as a minimal safe repair yet.

Before a repair, record exact invariant, actual-function regression and failing
old-source/negative control. Before another expensive run, state the changed
hypothesis or missing discriminator. Any repair gets a new immutable full-gated
source and separate quiet same-source ON/OFF pressure acceptance. Synchronous
tracing perturbs latency; this instrumented result is not quiet acceptance.
No new deployment, push, tag or main promotion.

## Coherent-cut negative control

Mac cd84edcd supplies the counterexample; the actual production capture-helper
regression now explicitly models sideA resolved in the pinned image while sideB
is absent. The post-pin barrier delivers B and advances the transaction revision:
the helper returns BUSY, frees the pin and marks a changed cut. Fresh stable
recapture contains both sides. An ordinary atomic update does not change the
transaction fence and retains the original coherent pin. Existing four-cut,
import/authority/IO/deadline and balanced pin gates remain. Normal and ASan/UBSan
executions pass. `--unsafe-no-transaction-fence` alters only extracted test code;
it fails the mixed-image assertion (exit1), proving removal of the production
comparison would violate this oracle. No production fence or ordering was changed.

This is a controlled production-function model of asynchronous participant
coverage, not a new real-network interleaving or full-LSM closure claim. Existing
real snapshot/transaction tests and latest full Linux gates remain separately
source-specific. Further discrimination should aggregate transaction revisions
by INTENT/GUARD/REDUCE/DECISION and PUT/DEL, correlate applied vectors, and split
root refusal branches (catalog-newroot, generation/parent, binding, live TXN read,
expiry). These changes are a diagnostic proposal only; no unchanged pressure
repeat or filtering DECISION/cleanup is justified yet.

## Second bounded discriminator source (qualification pending)

The next instrumentation adds eight saturating process-local counters per KV
store/snapshot: INTENT/GUARD/REDUCE/DECISION PUT/DEL. They increment only alongside
existing query-tracked transaction revision changes under the KV mutex; the
snapshot copies them at its original cut. Existing fence/revision semantics and
disabled tracking are unchanged. No counter is persisted, replicated or used as
coverage; fixed64-byte hints per store/snapshot, no per-operation log or new KV
mutation. Collision diagnostics compare pinned/live counters and pin/live g0/g2
applied vectors; sampling is explanatory and not an atomic new authority proof.

Root diagnostics split catalog absence with authoritative new-root success,
current transaction read refusal, catalog IO, parent/generation changes, cached
link refusal, live binding read/missing/mismatch, and expiry. They include requested
root, catalog generation and actual checked inode (including ancestors). Leaf
durations are measured; outer root/page events remain duplicate envelopes.
Initial lag-update query_lock writer wait is now measured separately. No names,
transaction keys or payload values are logged.

Actual root/binding helper tests pass tracing-off/on and under ASan/UBSan,
including unchanged permission and no-output refusal; cut/page tests pass as
well. A real-LSM query catalog regression checks each exact kind PUT/DEL count,
ordinary-write noninvalidation, snapshot immutability and disabled tracking.
Linux execution of that regression and complete candidate qualification are
pending. Mac/ORCD review labels and sampling limits before any expensive run.
This remains a diagnostic source, not a proven repair or quiet acceptance.

## Reviewed diagnostic freeze preparation

Mac review `aaf6a3e8` and ORCD review `79adbf22` are integrated. Typed transaction counters are process-local successful memtable mutation records, not durable acknowledgements, pending occupancy, unique transactions or participant coverage. Live revision and activity are sampled under separate KV mutex acquisitions. Root live-read labels identify call sites and must be interpreted with their return codes; catalog absence precedes type/access validation. Nested events and cumulative timings are not independent failures or exclusive backend time.

The follow-up source captures typed activity and applied vectors under the host mutex, then emits after releasing it. Initial lag writer-lock timing is captured at acquisition and emitted after unlocking, only for waits of at least one millisecond. This removes routine zero-wait records and logging under these locks without changing refusals, fences or retry budgets. Other synchronous trace sites still perturb timing; instrumented results are diagnostic evidence only. Foreground admission/proposal attribution remains uninstrumented. Actual capture-helper normal and ASan/UBSan checks pass; full Linux real-LSM and merged native gates remain required before the single original-budget diagnostic.

Frozen `57748bb5` full fault-enabled Linux run stopped at the new root-refusal extraction fixture: GCC `-Werror=misleading-indentation` rejected an unbraced one-line else followed by return. This is a fixture compile failure; normal/sanitizer/mounted gates did not execute and controllers exited without borrowing mounts. Explicit braces repair the fixture without changing production source. Preserve the failed run and qualify the new frozen identity.

## Second frozen AMD diagnostic: 95d3f1b1

The repaired fixture candidate passed full normal and fault-enabled `make all test`, seventeen focused ASan/UBSan scripts, and a separately rebuilt fully sanitized real-LSM catalog test. All 1,506 staged inputs remained unchanged; normal mounted binaries/library retained their recorded hashes. [Native evidence](../../results/measure/20261012-v030-merged-native/95d3f1b1/acceptance-summary.json). The earlier `57748bb5` fixture compile failure is retained separately, not relabeled as a pass.

Both direct and buffered ordinary overwrite prerequisites passed all four primary/peer/cold byte checks and physical fragment/queue drain; round and mount restoration exited zero. [Ordinary raw evidence](../../results/measure/20261012-v030-ordinary-overwrite/95d3f1b1/mounted/progress.log).

One instrumented direct seven-client/jobs16 run then reproduced the original-budget failure. Every client reports 209 PASS, 4 FAIL, 1 mmap SKIP, 7 NOTRUN, lasting 44.6–45.1 seconds. Deep nesting and stats timed out; many-files was terminated and find remained blocked behind the stats worker. These are not four independent 30-second timeouts. Busy extra-client teardown aborted the fixture; cold packing/GC were NOTRUN. Round exit 1, normal mount restoration exit 0; no detached jobs remained. [Stage summary](../../results/measure/20261012-v030-query-stage-diagnostic/95d3f1b1/direct/stage-summary.json). All 37 native, 13 ordinary and 28 diagnostic artifacts were retrieved and hash verified, excluding binaries/store data.

| Observation | n2 | n3 |
|---|---:|---:|
| Transaction-cut mismatch BUSY | 72 | 73 |
| Complete capture BUSY | 19 (includes one authority failure) | 17 |
| INTENT PUT/DEL changed samples | 68 / 61 | 72 / 71 |
| DECISION PUT changed samples | 61 | 72 |
| Root catalog-absence BUSY | 390 | 334 |
| Root live-read/parent/binding/expiry leaf failures | 0 | 0 |
| Successful checkpoint total-attempt maximum | 9,115 ms | 9,558 ms |
| Retained generation 2 → 3 publication gap | 47.961 s | 47.497 s |
| Initial lag lock waits at least 1 ms | 0 | 0 |
| Late publication lock maximum | 0 ms | 0 ms |

All root leaf refusals cover fourteen requested roots, each itself absent from retained generation 2 while the fresh transaction-aware inode read succeeded; leaf lookup duration is at most 3 ms. This directly explains these query refusals, not the foreground deep-nesting timeout or the entire pressure failure. No pending-record callback was logged. GUARD and REDUCE PUT/DEL activity also changed; aggregate counters cannot identify a specific transaction, namespace operation, participant coverage or durable acknowledgement. Counter samples and their deltas are diagnostic hints with the peer-reviewed sampling limits. A one-millisecond initial lock event occurred on unhosted n4, not the capable publishers; absent events mean below measurement resolution. Nested wrapper errors are not additional failures.

The source remains diagnostic-only. Quiet packing acceptance is blocked. A safe next hypothesis is to improve opportunities for a fresh coherent cut after cheap pre-build contention while preserving every post-pin authority/import/transaction check and the four-cut bound; separate costly build/checkpoint failure backoff remains necessary. Any scheduling remedy must first have a baseline-failing controlled oracle and bounded peer review. Foreground admission/proposal attribution still requires its own discriminator. No further unchanged live pressure run is warranted.
