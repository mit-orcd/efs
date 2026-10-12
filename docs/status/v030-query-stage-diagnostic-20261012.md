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

## Local scheduling remedy proposal, not deployed

The new local candidate explicitly returns only the final capture attempt's transaction-changed flag. A BUSY from that flag may schedule another capture after 200 ms only when the whole capture path took less than 200 ms. This is a new completion-to-retry scheduling policy, not the existing 100 ms poll interval. Every capture still creates a new pin, repeats both authority proofs/import/revision checks, rejects mixed transactions, releases failed pins, obeys the four-cut bound and original build deadline. It does not use fresh inode data to fabricate a retained subtree.

Authority/import/general BUSY, a later authority refusal after an earlier cut collision, hard IO, captures lasting at least 200 ms, pending/build/checkpoint refusals retain their original scheduling/coalescing/backoff. Policy or metadata-revision read failures clear the fast-cut hint. Success retains 2 s coalescing. No case, authority or build budget is extended.

The actual capture functions and scheduler are executed together under controlled boundaries. With a modeled coherent quiet interval at 500–700 ms, the old 2 s retry misses it and fails the oracle; the candidate captures it at 590 ms after nine pins/eighteen group proofs, with balanced pin ownership. Persistent churn never publishes: five four-cut cohorts, twenty pins and forty group proofs over 1,100 modeled ms. The upper sustained admission rate is five cohorts/twenty cuts/forty group proofs per second per publisher (up to tenfold the previous retry load); real proof durations and polling reduce it. Faster retry may worsen overload and does not guarantee a quiet cut exists.

Normal and ASan/UBSan controlled checks pass. The baseline-scheduling negative control and unsafe-all-BUSY shortcut both fail their intended assertions. Existing successful coalescing, hard-error exponential backoff and mixed-cut refusal checks still pass. [Local raw evidence](../../results/measure/20261012-v030-cut-retry-local/normal.log). These are controlled local tests, not full Linux or mounted performance acceptance. Independent review is pending; no new AMD live run is authorized for this source.

## Frozen qualification handoff and separate oracle checkpoint

The immutable candidate is `b239dc7de104ff8437e49d9333a529790601464c`, including the reviewed Intel namespace fixture repair and the reviewed cut-only scheduler. Its complete source bundle has 1,517 per-file SHA-256 entries, archive SHA-256 `24f6ca3ec7d509a24c74de4b6a746d033bef872b7145eec9beb2e387a5caa548`, input-manifest SHA-256 `1500601db39a388c5d033c1ab8c9c537e0d94d5fc2c1a2ef9ed8946da90696c7`, directory-manifest SHA-256 `911c85e28319837c62e30a5ab92fba394be649713ec3062b34442f58668197bb`. The archive is generated from the Git commit and verified against every input hash, never from moving worktree content.

Mac review `0e24f213` and manager review found no additional correctness defect within the bounded scheduler change; full Linux/live qualification is separate. NUC and ORCD received exact bundle/runtime/ABI requirements for independent qualification, preserving original stores, certificates and services. All daemons, clients and management binaries in a fixture must have the same build identity; old readers cannot safely consume new tag53/tag54 records. Use isolated stores or stopped verified copies and preserve old untouched service data for restoration.

AMD full native/fault/sanitizer prerequisites run on frozen b239 inputs. Authorized mounted plan is both-mode ordinary overwrite warm/peer/cold/physical gates followed by one quiet direct seven-client/jobs16 run with original case budgets and cold/physical gates. Buffered packing runs only after direct passes. Tracing is unset; no noisy diagnostic result is quiet acceptance. A direct failure stops unchanged repeats and preserves existing case evidence. NUC W52/W112 and controlled W97 and ORCD coalescing/shared-GC remain independently source-specific.

A separate test-only next checkpoint moves the scheduler oracle after unchanged mixed-cut safety checks, matching independently tested Mac artifact `ce328004`. This makes the fence-removal negative control fail at the intended actual mixed-cut refusal, rather than first tripping the scheduling oracle. Normal/ASan checks pass and the fence negative fails the intended assertion. [Local evidence](../../results/measure/20261012-v030-mixed-cut-oracle-isolation/fence-negative.log). Executing/frozen b239 inputs remain unchanged; this test-order checkpoint does not alter production behavior or retroactively replace frozen qualification.

## Reviewed candidate quiet result: b239dc7d still blocked

Full normal and fault-enabled Linux suites, eighteen focused ASan/UBSan scripts and the fully sanitized real-LSM catalog pass on frozen b239. All 1,517 input hashes remain unchanged; normal binary/library hashes retained. [Full raw qualification](../../results/measure/20261012-v030-merged-native/b239dc7d/acceptance-summary.json), 38 retrieved artifacts hash verified.

Both direct and buffered ordinary prerequisites again pass all four primary/peer/cold overwrite byte checks and physical fragment/queue cleanup; round/restore zero. [Ordinary evidence](../../results/measure/20261012-v030-ordinary-overwrite/b239dc7d/mounted/progress.log), 13 artifacts verified.

The single authorized QUIET direct seven-client/jobs16 packing run with original case budgets still fails. All seven report 209 PASS, 4 FAIL, 1 mmap SKIP, 7 NOTRUN in 45.1–45.3 seconds. Deep60 and stats time out, many-files is terminated, and find remains blocked after stats. The trace environment was unset. [Quiet raw result](../../results/measure/20261012-v030-quiet-packing/b239dc7d/direct/stage-summary.json), 28 artifacts verified; empty stage counters are expected with tracing disabled, not proof of absent cut churn.

Retained generation 2 → 3 publication gaps are 46.534 s on n2 and 46.656 s on n3. The generation-3 logged total build/attempt durations are 10.063 s and 9.347 s, not exclusive fsync or proof durations. The shorter retry therefore does not close original-budget quiet packing acceptance. No causal throughput/load improvement is claimed from unmatched previous instrumented runs. No noisy baseline load comparison or unchanged repeat was run.

Round exit 1, restore exit 0; normal mounts verified and all detached jobs finished. Buffered packing is NOTRUN because direct failed. Cold packing and physical/shared GC are NOTRUN after busy extra-client teardown. Original stores remain intact. NUC/ORCD independent same-source qualification may continue within their narrower gates; it must not inherit AMD pressure acceptance.

Next evidence needed is the exact foreground case phase and in-flight operation: the failing 60-level absolute-path test includes recursive cleanup, while the passing 100-level case uses directory handles and does not include recursive cleanup. Quiet existing logs do not identify which phase consumed the deadline. A minimal opt-in phase discriminator and bounded capture/cohort/proof work counters require review before another instrumented live run. No safety fence or timeout relaxation is justified by this failure.

## Local nonblocking phase/work diagnostic proposal

The next local candidate adds ten bounded deep60 lifecycle boundaries (spawn, worker entry, setup, creation, leaf write/read, recursive cleanup, absence check, completion). The actual original case operation AST remains byte-equivalent after stripping observer calls; no workload or deadline is changed. Missing/full observers cannot block workers or change their result: events use nonblocking UNIX datagrams, with at most 128 retained case records. An asynchronous native-/tmp observer writes outside EFS, records receiver limits/errors, and explicitly treats lost datagrams as unknown.

A separately enabled capture-work record reports whole-path capture elapsed, final capture reason/status, final transaction-changed and fast-eligible flags, actual pins/successful pins/paired authority-call counts, pending capped revisions, completion-to-next-admission spacing, exact catalog-build API duration, exact checkpoint API duration and total attempt time. Paired calls do not count transport retries or certify authority coverage. The snapshot/capture ownership and all existing decisions are unchanged. New fields are diagnostic stack state only; no wire or durable format changes.

Each daemon emits at most 512 fixed-size nonblocking datagrams per process; the private fixture collector retains at most 2,048 work records. Emission occurs after host/query locks and retired-catalog release, uses balanced short-lived descriptors, and is default quiet. Build duration excludes capture/checkpoint but includes the actual catalog API's internal callbacks; total time also includes revalidation/publication/free work. Initial restore/catchup and all other residual intervals remain residual, not attributed to fsync. Missing packets, cap exhaustion and millisecond rounding must not be assigned a cause.

Local actual case/worker tests pass for default quiet, exact phase ordering, original shape, leaf failure status, missing/full sinks, capped collection and cleanup. Actual C sender normal/ASan/UBSan checks pass for exact numeric payload, disabled silence, record cap, missing/full sinks and balanced descriptors. Actual capture functions normal/ASan/UBSan pass with counters matching controlled pin/proof boundaries. [Local evidence](../../results/measure/20261012-v030-phase-diagnostic-local/phase-observer.log). Peer review and full frozen Linux prerequisites are pending; no new diagnostic run has started.

Proposed discriminating run: one new frozen direct seven-client/jobs16 fixture with original 30-second cases, `EFS_POSIX_PHASE_TRACE=1` and `EFS_QUERY_WORK_TRACE=1`, legacy synchronous `EFS_QUERY_STAGE_TRACE` unset. It must distinguish creation versus cleanup and whether fast-eligible capture attempts are actually admitted, at what spacing/load, with expensive build/checkpoint phases separated. This is diagnostic evidence only, not quiet acceptance; stop unchanged repeats and preserve normal mounts.

## Peer-reviewed diagnostic timing and cleanup follow-up

Both Mac and ORCD independently pass the actual phase/sender checks; Mac also independently passes capture native/ASan checks. The next follow-up names `capture_reason` explicitly, keeps the whole-path `capture_ms` used by scheduling, and adds exclusive `source_ms` plus initial `restore_ms`. These are needed to distinguish pre-source revision/lag work disabling fast eligibility from expensive source proof work, and to avoid attributing startup restore/catchup to ordinary build/checkpoint. Restore phase may include its existing catchup log; residual time remains unassigned.

Observer teardown and handle closure now run in a finally around daemon stop. A regression executes that actual shutdown AST with a controlled stop exception: original exception preserved, observer thread/socket closed. Record caps cover the planned original pressure window: 512 attempts/node allows over 100 s at the maximum five cohorts/s; four node caps fit the 2,048-record receiver limit. Caps/drop/rounding still make missing packets unknown. Pair counts are logical pair API invocations, not exact group-proof or transport RPC counts.

All original safety decisions and scheduling remain unchanged. No new live run has started. Subsequent merged tests/evidence are separate from immutable b239 runs; latest source needs a new full Linux freeze.

## Defect-only shared candidate: optimization quarantined

The next shared candidate restores the pre-b239 scheduling implementation exactly: `query_refresh.h` is byte-identical to `95d3f1b1`; the publisher uses original `efs_query_refresh_due`, coalescing and failure backoff, with no cut-retry state or accelerated admission. Four-cut capture, authority/import/transaction fences and deadlines remain unchanged. Only the unproven retry optimization is removed; W112, namespace fixture correctness, all other true fixes, bounded diagnostics and Mac safety/audit regressions remain.

The quiet-window optimization oracle and earlier b239 evidence are preserved as historical experiments, not stable acceptance. Current actual capture regressions independently retain mixed-cut refusal, ordinary atomic-update noninvalidation, balanced pin/proof counters, final authority refusal after an earlier collision, import/IO/deadline bounds and the intended fence-removal negative control.

Diagnostic `fast_eligible` labels the historical experiment's eligibility only and is never read by scheduling; `policy_interval_ms` records the actual original coalescing/backoff interval. No performance comparison or tuning is authorized. A demonstrated fixture cleanup defect is also repaired: simultaneous daemon-stop and observer-summary failures preserve the primary stop exception while closing every handle; observer/handle failure without a primary remains an explicit failure. Actual shutdown-AST negative controls pass.

All five owners must test one immutable shared source after safely draining/restoring earlier fixtures. Old b239 NUC/ORCD results remain valid only for that source and cannot certify the new shared candidate. Full normal/fault/sanitizer gates and one original-budget phase/work defect diagnostic are pending under the new identity; no push or promotion.

## Shared 31abdd5e fixture portability failure

The five-owner shared source is `31abdd5e03be224fb95ca484ddbf3fa9dfd2899b` (1,557 inputs, 52 directories). Mac Linux Python 3.13 rejects the phase fixture's canonical AST hash before its socket checks: newer Python adds an empty `type_params` field and changes whether empty fields appear in `ast.dump`. This is a test-schema portability defect, not a filesystem result. Its failed gate remains failed; frozen executing inputs are unchanged. Central AMD full normal/fault and sanitizer gates have started on that exact source.

The next-candidate test-only repair asserts that generic parameters are empty, excludes that new empty field, and requests explicit empty fields when the interpreter supports that option. The original canonical workload hash and case operations remain unchanged. The complete observer/cleanup regression passes locally on Python 3.9 and independently in a separate AMD staging directory on Python 3.14. Mac peer review passes Python 3.9/3.13; a depth 60-to-59 mutation still fails the unchanged hash assertion. The next coordinated freeze remains pending. No old candidate result certifies the new source, and no new mounted packing diagnostic has started.

## Common c2bfb9e9 AMD prerequisites

Execution remains frozen at `c2bfb9e9a281aae33dfaa01689e61866ccf60af8`; subsequent evidence commits do not change that target. The reviewed Python AST portability repair and Mac retained-namespace audit regression are test-only changes. AMD full `make all test` normal/fault gates, twenty focused ASan/UBSan gates, and fully sanitized real-LSM catalog/namespace audit pass. Post-test verification finds no mismatch among 1,557 inputs; saved normal daemon/client/management/library hashes match. [Compact raw full-gate evidence](../../results/measure/20261012-v030-merged-native/c2bfb9e9/acceptance-summary.json).

Original private direct and buffered ordinary overwrite checks pass all four scenarios with primary/peer/cold exact bytes and drained physical fragments/queues. Both-mode fixture exit and normal-mount restoration are zero. [Raw overwrite evidence](../../results/measure/20261012-v030-ordinary-overwrite/c2bfb9e9/artifact-sha256.json). The one original direct seven-client/jobs16/30-second phase/work diagnostic has started; its final result is pending. Legacy synchronous stage tracing is disabled. These prerequisites do not certify packing-pressure acceptance or repair the retained Mac orphan.

## c2bfb9e9 original-budget phase/work result: FAIL

The single direct seven-client/jobs16 diagnostic exits 1; restoration exits 0. All seven clients complete deep60 and every suite has zero NOTRUN cases. Clients 0/1/2/4 record 219 PASS, one FAIL and one SKIP; clients 3/5/6 record 218 PASS, two FAIL and one SKIP (51.0–56.4 seconds). The failures are original 30-second `.efs` stats (six clients) and find (four clients) deadlines. Acknowledged cold-byte checks pass independently before the fixture raises the preserved seven-client failure list. No quiet acceptance run is scheduled.

The case collector receives/retains all 70 deep-case boundaries, below its 128-record cap, with no collector errors. The work collector records 249 events, below sender/sink caps; datagram loss remains unknown. Logical hosted node labels 1/2 record 13/20 terminal transaction-cut BUSYs and 2/0 authority BUSYs. Maximum exact source times are 408/657 ms; maximum actual build API times are 10,580/13,787 ms; maximum checkpoint API times are 3/2 ms. Logical labels 0/3 report unhosted pin NOTFOUND, not proven corruption. Pair counts measure proof API calls, not wire traffic. Multiple process epochs are retained; aggregate maxima alone do not establish which phase caused a query deadline. No retry/scheduler optimization or deadline increase follows this observation.

[Raw diagnostic and phase/work summary](../../results/measure/20261012-v030-phase-work/c2bfb9e9/direct/phase-work-summary.json) and its 30-artifact SHA256 manifest are verified locally. All 1,557 frozen inputs remain unchanged after mounted testing, detached jobs have ended, and both normal mounts are restored. The next step is an actual-function liveness/invariant or fixture reproduction with a negative control, before any product fix or further pressure attempt.
