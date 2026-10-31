# Task H: devel wave 33 reconciliation and merge inventory — October 29

> Migrated from `agent/task-h-repair` at `cd822e2f7` on Oct 30, 2026, by Task BO. Nothing was shortened: the page already held conclusions, methods, hashes and raw-capture locations only. Scripts and tests it names in backticks exist only on that branch; see [the migration page](task-h-evidence-migration-20261030.md).

Branch `agent/task-h-repair`. Merge `aed85c52f` has parents `a3aacfee9` and
**devel `330c48aea`**. This page supersedes the b2 integration recommendation;
older pages remain historical evidence. No merge into devel and no push.
**Requested reconciliation COMPLETE:** two fresh intel2 full `test-all` runs
finished with **0 FAIL** on the same product/test source. Both retain
**STATUS INCOMPLETE / 10 NOTRUN** for unavailable host capabilities; this is
not an all-platform PASS. The earlier merged-head W170 failure remains FAIL
with attribution unproven. No new product investigation or acceptance scope
was added. The coordinator owns refactor cuts and the merge into devel.

**K33 fatal-read subfinding: OPEN, non-blocking for this merge**, per the user.
The repair of reproduced LOOKUP/write EIO mechanisms is separate from the
unproven callback of the historical `w-19`/`w-27` read EIO. The previously running original-binary control has now closed **FAIL**:
71 cases passed, case 62 failed, 184 queued cases NOTRUN after the failure.
Its writer reported `w-21` read-back EIO; the main thread subsequently failed
`s3.2-0` writes with BUSY. Exact original binaries remained unchanged. Raw:
`nuc:/home/maik/efs-artifacts/taskH-causes-20261029/k33-exact-error-trace-2/`;
1,565-file text manifest SHA-256
`4c359dba8d2b59672ba266628fec136c7e8d8f752213dc9c782641a18992273d`.
All stores remain on nuc; no payload verification or callback attribution
is claimed for this new failed fixture, and no further investigation was
launched in this reconciliation. It is not a failure of the merged-head
intel2 gates. The overall K33 membership release row has its other pre-existing verification obligations;
this page neither closes those nor adds a release decision.

## What won the reconciliation

| Overlap | Result in merged tree |
| --- | --- |
| `dd6d2c5c2`, pending receipts and unpublished stripe retirement | Devel's implementation/tests retained; no return to treating a not-yet-published object as retired. The branch's namespace materialization/scan-lock fixes remain distinct. |
| `3769f44bb`, stripe identity index | **Exact devel `data_commit.c`**, including full `(ino, chunk, object)` indexing. |
| `214c541aa`, terminal ticket authority | Devel's terminal ticket/closure and fresh absence proof retained; owned retry may reuse only a freshly ADMITTED exact body and never supplies terminal authority. Both test families retained. |
| `478a5400f`, K55 slow COMMIT | **Exact devel `data_commit.c` and `test_data_commit.c`**. Branch early/pre-publication COMMIT helper/API and its fsync callers removed; devel fsync/flush implementation and budget test restored. This removes the overlapping helper/TLS/capacity experiment rather than trying to combine two durability schedulers. |
| K47 packed re-probe | Keep devel's NET/NOT_FOUND fix. Branch retains only attempted-holder recovery when an asynchronous probe has already cleared the down bit, plus the separate span-overlay re-probe. Those cases are not represented by devel's down-only base retry. |
| K51 heartbeat and stop wake-up | Devel's implementation remains; the distinct branch rejoin-owner lifecycle remains. No second server heartbeat owner. |
| READDIR | Devel's late proof mask, bounded rescan and missing-proof refusal win. Additional branch SPLITTING pre-fence and metadata scan-all-lanes remain for the retained first-destination-without-bitmap-change case. The older branch mid-scan restart is removed. |
| AL/AK/AC/AA, Task I, ORCD dd profile | Auto-merges retained devel's selected read scratch/hash paths, I/O stats, published benchmark, allocator/transport changes and full stripe indexing. No old ram/query/transport selector is reintroduced. |
| Harness conflicts | Devel sanitizer/compiler wrappers, published-benchmark setup and crash-dump suppression win. Keep additional failure-output retention, producer ownership cleanup and strict FileID priming checks. Both external-TMPDIR and serial-clock scheduler tests remain. |

## Product change inventory

“Fix” below requires a **named retained failing reproduction**. A historical
latency failure alone is not proof that a nearby optimization fixes it.
**Refactor** means no matching retained failing reproduction was established;
these are explicit cut candidates, not required fixes or hidden acceptance
claims. Original failed results never become PASS. Commits identify the
origin, not necessarily an unchanged patch after reconciliation.

Evidence keys: [P](task-h-publication-repair-20261026.md) is the original repair
history; [C](task-h-k38-b6-repair-20261028.md) is the K38/B6 and reopened-cause
history; [B](task-h-devel-b2-integration-20261028.md) is the prior merge audit.
Raw path abbreviations below are absolute on the named host:

- **N27** = nuc `/home/maik/efs-artifacts/taskH-repair-20261027/`.
- **N29** = nuc `/home/maik/efs-artifacts/taskH-causes-20261029/`.
- **R** = node9901 `/data1/efs/evidence/taskH-repair-20261026/`.
- **M27** = Mac `/Users/mike/Documents/Codex/artifacts/taskH-repair-20261027/`.

Linked histories record exact binary/archive identities, oracle hashes,
commands, failed stores and cold verification limits. None requires an
unavailable private capture to reconstruct the source regression.

| ID / K row | Distinct product change / origin | Retained failing reproduction / evidence | Disposition |
| --- | --- | --- | --- |
| F01 / K38, K40 | Resolve definite receipt STALE under original completion budget; preserve earlier STALE across later BUSY (`0c1970678`, `84d6c351c`) | R `attempt-03` known loser hidden as UNKNOWN, P initial candidate evidence; original publisher control compiled against `bf143e486` fails the completion assertion; `test_w155_report_resolve.py`, `test_report_resolved_retry.py` | Fix; keep, alongside devel's newer pending-receipt fences. |
| F02 / K38 | Reuse an unchanged owned upload only with fresh exact ADMITTED ticket authority; do not infer a lost CAS from the client's own placeholder (`40856b152`, `e41cd19ad`, `e7b321502`, `fdc0f0900`, `5e63df1a0`, `1848fe5e0`) | R `attempt-31`: `report-stuck` chunks 57/90, false committed-placeholder proof and EIO; P “K38 limiter proof”. Captured-identity runtime control fails prior source | Fix; keep. Unconditional bodies, unchanged spans and detached ownership are separately validated cases of this one ownership rule. |
| F03 / K38 | Replay the exact sent snapshot after late PARTIAL, not the subsequently modified live map (`84d3c8f3a`) | N27 `attempt-63` close EIO; `late-replay-unit/result.json` hash `5e5991…47c3c`; `test_report_resolved_retry.py` actual inner block fails against a10 source; B comparison against b2 source | Fix; keep; not replaced by devel's receipt-cache eviction repair. |
| F04 / K38, K40 | Preserve live REPORT coordinator across bounded BUSY; route multi-chunk single-inode units to the covered inode leader (`fcc44fe5c`, `45102379f`) | N27 `attempt-37` diagnostic progress FAIL, REPORT inode 76084 switching BUSY/NOT_PRIMARY for 8.600 s; P coordinator section and actual eight-record routing regression | Fix; keep. |
| F05 / K38 | Rebuild publication identity only after a fully proven abort; never re-PREPARE an already aborted decision (`5e55c8225`, `a42933a72`) | N27 `attempt-08` retained STALE/abort-resolve tail; P “Publication abort cleanup”; actual `test_pub_unit_forward_abort.py`, `test_fin_abort_handoff.py` prior-path failures | Fix; keep the identity/verdict rule. Batching is separately R03 below. |
| F06 / K38 | Capacity pressure flushes only a frozen prior cohort, not future writes or the held unit recursively (`4f8f25e7c`, `5c81d6070`) | P pressure/held-unit stall captures and `test_put_drain.py` whole-inode negative control (three-cohort publication); retained D4 failed stores N27 | Fix; keep the bounded progress rule. |
| F07 / K38 | Retain immutable PUT through temporary quota refusal with no spare (`a10859489`) | P quota/unchanged-body failure; `test_put_quota_reclaim.py` fresh native quota/reclaim/cold gate and `test_put_retry_budget.py` prior-source runtime failure; C historical quota-gate limits | Fix; keep; not a change to global K45's compacted-prefix diagnosis. |
| F08 / K38 | Clear every GETCHUNKS record before a collect retry (`121e5fb24`) | C “Fixes and proof”: actual collector alternates delta/plain and packed/ordinary; old source fails strict client validation, fixed native/ASan pass; R retained K38 PROTO trace has no exact rejection site | Fix; keep the demonstrated collector defect, do not assert unique historical EIO attribution. |
| F09 / K38, D4 | Mix allocated inode bits into dcache buckets and striped I/O locks (`662789008`) | C strict D4 latency investigation; retained prior-source distribution regression `test_dcache_lock_distribution.py` (4096-spaced inodes used only 16 buckets), C preflight evidence | Fix of demonstrated collision defect; broader latency attribution remains limited. |
| F10 / K22 | Apply-serialized allocator reservation before dependent MKDIR PREPAREs (`5ecd9cefe`, `a10f96604`) | N27 `attempt-62/diagnostic-loaded`, preserved allocator CAS refusals and `diagnostic-cold-summary.json`; P allocator section, root log hash `b2b875…c84`; actual 16-loss CAS control then reservation success | Fix; keep current-format command kind 12; fresh clusters only. |
| F11 / K22 | Transaction own-entry waits use existing progress-renewal policy instead of fixed follower-read wait (`994c025e6`) | N27 `attempt-28` 400091–400103 µs waits and MKDIR refusal; P own-entry section; retained actual batch waiter prior-source failure at progressing 1.2 s | Fix; keep existing 400 ms no-progress / 2 s cap. |
| F12 / K22 | Read committed transaction rows before returning namespace receipt replay (`e32d1721a`, `f533e6054`) | N27 `attempt-66` eight committed cross-group MKDIRs refused until materialization; R `receipt-read-unit/` result hash `579abb…6601c7`; actual committed-staged receipt control | Fix; keep; receipt availability is not materialized-row authority. |
| F13 / K22 | Release KV scan-reader scope before coordinator proof (`bd9540650`) | R `receipt-read-gate-unit/`, result hash `fbc68d…4017`; prior library links and fails actual real-KV callback-after-scan assertion; P receipt-scan section | Fix; keep. |
| F14 / K22 | Scope pending receipt guards to retried identity and covering ACK (`0b0502dd3`) | P shared receipt-window failure and actual `test_namespace_receipt_pending.py` unrelated-pending/covered-history refusal controls | Fix; keep; does not undo devel pending-row eviction rules. |
| F15 / K22 | Coalesce mount-session bootstrap, then allocate namespace receipt; share direct CREATE deadline (`6e2566cfe`, `67350b59a`, `6c91140e0`) | N27 `attempt-21` four MKDIR BUSY at ~32.6 s; P bootstrap section, actual 16-worker session start/check control; `test_opid_admission.py`, `test_create_errors.py` | Fix; keep; no new deadline or admission size. |
| F16 / K22 | Retained CREATE stays nonterminal on BUSY row authority, including cross-group authority delegation (`3a5ad979f`, `1bf8d29f3`) | N27 `attempt-20` four CREATE NOT_FOUND after bare-success MKDIR replay of inode 2676 (`row gone`); actual `test_retained_create_authority.py` fresh-proof/missing-host controls | Fix; keep. |
| F17 / K22 | Pending snapshot import retries after delayed log ACK; replacement leader can supply requested image without old match state (`210a0ad2c`, `db289451c`) | N27 `attempt-67/bgeo-import-analysis.json`, hash `41dd63…a2f`: group-2 snapshot index 3657 prepared but never installed; P pending-import negative control; `test_raft.c` delayed ACK/pending snapshot and replacement-leader regressions | Fix; keep devel term/commit/install fences. |
| F18 / K22 | Finish frozen independent KV flush files in checked, joined cohorts (`16728b167`) | N27 `attempt-59`: 900 s gate FAIL, victim `kv-flush files=28 io_ms=2369`, immediate leadership loss; P section; actual `test_kv_flush_finish.py` failure/resource/manifest controls | Fix candidate for retained synchronous pause; correlation does not establish every election cause. |
| F19 / K38, K22 | Wall-clock empty acknowledged-prefix heartbeat/probe while Raft pump is delayed (`c4d2fba18`) | N27 `attempt-40` K22 required-pressure FAIL (closed index `d06b45…70bc`); P delayed-pump negative control, actual `test_raft.c` coarse-pump regression | Fix; distinct from devel K51 membership heartbeat lifecycle. |
| F20 / K38, K22 | Retain known-leader contact while its accepted heartbeat remains queued (`c88c0324c`) | N27 `attempt-43` K22 leader loss before refusal (closed index `580bf5…af5`); actual inbox/coarse tick negative control and `test_host_inbox.py`; no durability fence bypass | Fix; keep. |
| F21 / K42 | Exact-epoch revoke ACK and definite handoff retries (`67350b59a`, `81008234d`, `e9a08cbbc`) | P retained cleanup/unmount failures; `M27` revoke redirect/epoch tests; `test_revoke_redirect.py`, `test_session_finish_retry.py` previous-source failures | Fix; keep; uncertainty remains refusal. |
| F22 / K42 | Session revoke BEGIN uses already-reserved cleanup admission capacity (`1dc4ffb45`) | N27 `attempt-38` unmount BEGIN immediately BUSY; `M27/session-begin-reserve/manifest.json` hash `22d5fd…dece`; prior predicate fails exact BEGIN assertion | Fix; keep existing capacity bound. |
| F23 / K42 | Prove empty cleanup after ambiguous apply wait; preserve last certified cleanup progress on timeout (`b4331aef8`, `dbf87d011`) | P N27 `attempt-66`/earlier K42 failed cleanup captures; `test_session_cleanup_cli.py`, `test_session_finish_retry.py` actual waiter controls | Fix for empty-proof rule; timeout text alone is diagnostics (R09). |
| F24 / K38, K51 | Own/join rejoin thread before bounded daemon teardown (`602cccc0a`) | N27 `attempt-39` node-2 cold-stop SIGABRT after detached rejoin continued (closed index `926923…83e5`); `test_shutdown_bounded.py` paused rejoin/start-failure control; B distinguishes devel heartbeat owner | Fix; retain only rejoin ownership, devel heartbeat wins. |
| F25 / K38 | Exact PUT durability epoch owns fragment and ancestor namespace barriers; drain queued namespace proofs before ancestor GC (`6412320f6`, `becc5729c`) | N27 `attempt-32` foreground 14.31 s, fsync 14.128 s behind 9.355 s unrelated-filesystem barrier; closed index hash `9e7aa7…257d`; P exact epoch section; native `test_put_epoch_files.py` and `test_store_durability.c` controls | Fix; keep checked file/namespace barriers, not removed fsync precommit helper. |
| F26 / K36, K47 | Re-probe a recovered recorded span holder; packed attempted-holder health-clear race (`e2e9030cd`, `ece63c2a6`) | R `attempt-62/k36-before-cold/manifest.json`, hash `7cfd76…ac1`; NET/OK/NOT_FOUND failure and 16-file oracle; B extra clear-down-bit runtime case | Fix; devel owns original K47 base fix, keep only additional cases identified above. |
| F27 / K48 | READDIR SPLITTING scan includes a first destination before bitmap update; no premature EOF on layout transition (`236a25cb5`, `9ceececc9`) | C cross-group regression holds destination PUT before parent bitmap/DEL, unchanged bitmap throughout page; devel `3426111b4` fails, native/ASan fixed pass. `test_readdir_migration_race.py` | Fix; combine metadata scan rule / SPLITTING pre-fence with devel's newer host rescan. Historical listing failure's exact interleaving remains unproven. |
| F28 / K43 | Decided pin-only retention belongs to decision sweeper; partial page cannot certify pin-only (`dde6564ed`, `52d6f5890`) | C K43 retained failure and `test_recovery_pin_owner.py` prior-source actual scheduler negative; historical failed record was not typed, so no unique retrospective cause claim | Fix of demonstrated ownership defect; keep pin and uncertainty fences. |
| F29 / K36 | Prioritize observed PREPARE/BUSY transaction shard over fair cold discovery (`20c0a2ebe`) | R `attempt-95/focused-k36_recovery-2`: allocator shard 3820, intent `fbb239bc…896`, equal commit/apply but original 30+8 s refusal; all 16 payloads exact on hashed same-binary clones. C full hashes; actual scheduler old 0/64 versus fixed 1/64 | Fix; keep 64-scan cap, five-second age and fair cold budget. |
| F30 / K33, K36 | Reject failed dual-group redirect hints while retaining unhinted REPORT receipt replay (`ce5196205`, `254725fd9`) | C retained K36 failure before server-starvation fix; `test_metadata_retry_budget.py` dead-hint / no-hint identified replay runtime controls | Fix of routing loop; not sufficient by itself to close allocator starvation. |
| F31 / K33 | Lease/session exchanges use inherited caller deadline instead of premature eight-attempt NET (`b50f33258`) | C `lease-budget-unit/pipeline` retained actual OPEN-to-FUSE EIO negative control; prior source EIO at 8, repaired success at 10 within original bound. Historical fatal-read site unproven | Fix of demonstrated early exhaustion; **original fatal-read K33 subfinding stays OPEN / non-blocking**. |
| F32 / K33, K38 | PUT-ticket exchanges/degraded marks likewise respect inherited deadline (`f11d00963`) | N29 original native write-EIO captures; `native-write-errors-24-25-27.json` hash `346bf9…41f`, FLUSH at original `0x418480` after attempt-7 NET; C ticket-budget retained controls | Fix; immutable ticket identity, receipt semantics and deadline-less cap retained. |
| F33 / K33 | Read-only LOOKUP/GETATTR explores another voter after repeated failed leader hint (`bddd7376c`) | N29 `k33-exact-error-trace-1/round-50`: native LOOKUP `0x41e68c` → write-attempt EIO; C retained real RPC old 16/NET versus fixed 4/success, deadline cases | Fix; no mutation routing change; captured operation was a write lookup, not historical fatal read. |
| F34 / K33 | Pump relinquishes stopping leadership before handler drain, respecting receive-persistence fence (`85962d8cd`) | N29 `k33-routing-fixed-1/round-4`: read OPEN EBUSY, 27.220 s leader drain; all 64 ACK payloads exact on original-binary clones; C raw manifest `b58715…ff48` | Fix; actual core step-down/persistence-fence regression and repaired TCP/RDMA gates retained. |
| F35 / K45 / W130 | Management raft-resync retries only definite pre-acceptance BUSY within one inherited I/O deadline (`16c0e10c0`) | nuc October-28 `attempt-05`: W130 returned BUSY while receive persistence was active; closed 195-file manifest hash `117563…1c7`; C historical full-suite section and actual `test_mgmt_resync_busy.py` prior-source runtime failure | Fix; keep 30 s existing I/O bound, never retry uncertain transport/malformed reply or leader refusal. |
| R01 / K38, D4 | Fresh OPEN/GETCHUNKS FileID handoff to PUT cache (`bf3a4d7e9`) | N27 attempt-50 retains an 11.695 s latency failure; diagnostic-51 did not reproduce it and does not prove this cache handoff caused the failure | **refactor**; measured/optimization candidate, not a proven latency fix. |
| R02 / K38, K22 | Cohort first-use lane bootstrap (`b88b84459`, `0d0552d30`) | Prior loaded failures exist, but no distinct retained before-code failure attributable to scalar bootstrap rather than the other receipt/pressure defects | **refactor**; cut candidate. |
| R03 / K38, K43 | Batch PREPARE barriers, large owned command buffers, ABORT/RESOLVE finishers (`45ad1da38`, `16f9e243f`, `3987609ff`, batching parts of `a42933a72`, `c26286eab`) | Functional error/ownership controls exist; a retained failing scalar production reproduction specific to the batching change is not established | **refactor**; cut batching separately from F05's decision correctness. Preserve error/verdict semantics when cutting. |
| R04 / K38, D4 | Pair independent REPORT group ReadIndex work (`5db04aab3`) | `M27/report-read-overlap` serial-mutant test exists; no matching retained live failure uniquely attributed to serial proof dispatch (P says bounded latency improvement) | **refactor**; cut candidate. |
| R05 / K38, K22 | Yield pump work at inbox group boundaries (`2636d5a60`) | Scheduling controls and general loaded failures; no distinct matching retained starvation failure establishing this scheduling boundary | **refactor**; cut candidate. |
| R06 / K38, D4 | Query LOOKUP captures callback-owned identity/attrs from same fresh reply (`39965d653`) | P full-suite stats latency failure motivates work; callback race regression exists, but no retained failing callback-race run has been identified here | **refactor** under requested conservative rule; do not promote generic stats latency to proof of this race. |
| R07 / K38, D4 | Per-node/per-boot query refresh measurement and removal of unrelated query reads (`b36444b29`, query path changes) | Original measurement could subtract incompatible counters; no product correctness failure specific to removed read established | **refactor**; measurement correctness/tooling is separate from product acceptance. |
| R08 / K38, K22 | Storage-session fence delegates lagging proof to healthy voter (`ef896be58`) | Code/runtime authority tests exist; no matching retained failing live reproduction identified in the current inventory | **refactor** under requested rule; retain fail-closed safety if cut. |
| R09 / K38, K22, K42 | Prepare refusal, REPORT phase, filesystem/epoch/fsync stage, first storage-fence and benchmark errno diagnostics (`31ee8bee5`, `077a59dc0`, `d6a5d40ff`, `79d0811e5`, `8dd44c05b`, `977d10c7c`, `1aa4260fd`, `b618b13f7`, diagnostic part of `dbf87d011`, pack ACK event `4c7eaaa67`) | These report failures; they do not repair a retained product failure. B6's old event/counter-cut oracle is separately reproduced in `test_b6_log_accounting.py` | **refactor / instrumentation**, not a product fix. Keep evidence collector/oracle corrections distinguishable from product logging. |
| R10 / K22 / W166 | Forwarded STATUS refuses unhosted members; ticket-authority delegation and bounded-tail REPORT admission (`49583981f`, `0c513ab6b`) | Deterministic authority controls exist; no separately identified retained failing native capture has been established by this inventory | **refactor** under the requested rule; devel pressure predicate remains authoritative. |
| R11 / K39 / K55 harness | Manual `--dirty-fsync` selector / qualifier call tied to the now-removed `precommit=1` marker | Historical helper-specific gate is obsolete after devel durability wins; it is outside the two requested `test-all` recipes and is not claimed as executed | **refactor / obsolete harness**, cut with the withdrawn helper before merge; do not treat it as a current acceptance requirement. |

The inventory covers remaining branch differences in `src/`, `include/`,
and product-facing diagnostics. Current session ACK is 34 bytes (explicit epoch), and allocator reservation uses current command kind 12; qualification uses fresh matching-build stores. This is not a retained-store conversion or compatibility qualification.

Header/codec and simulator changes belong to
the corresponding allocator/session/retention rows; they are not additional
unlisted fixes. Fault hooks are test instrumentation, not additional product
features. Tests, forensic scripts, output retention, cold verifiers and docs
are supporting harness/evidence changes, not independent product fixes.

## Removed or superseded private product changes

| K row | Removed / devel-owned change | Evidence / reason |
| --- | --- | --- |
| K39 / K55 / K38 | Precommit overlap, helper capacity/TLS-stack changes and prepared proof API (`31dcb3cbe`, `4c74077e7`, `0ffc905b2`) | Devel durability fixes win. Older B6 failure and withdrawn-experiment history stay in P; no two proof schedulers retained. |
| K47 | Original packed NET-or-NOT_FOUND down-node re-probe | Already devel; do not count as a new Task H fix. F26 is only the extra case. |
| K45 / K46 | Compacted-prefix refusal, resync/campaign fencing | Already devel; unchanged. |
| K51 | Membership heartbeat owner, wake-up and join | Already devel; unchanged. F24 is only rejoin ownership. |
| K48 / K22 | Truncate maintenance fairness ports | Already devel; retain devel implementation. F27 is a different directory-migration case. |

## Reproduction and gate results

Intel2 is an 8-CPU Linux host; TCP loopback private clusters, mandatory direct
I/O as configured by current product/test defaults; fault build enabled.
No existing deployment or other agent's stores are changed. Existing Task AU
completed before a 70-second no-EFS quiet check. Two sequential full source
trees, eight build/test jobs, original hard deadlines and soft budget rows,
fresh external scratch `/home/maik/h33-{1,2}`. No ASan/LSan suppression is added.
The host's libfuse 3.16.2 and existing Valgrind headers / mock cluster-script
fixtures are used. RDMA hardware and any unavailable host capability must
remain explicit NOTRUN, never reported as PASS.

Tested source candidate: `aed85c52f`; archive SHA-256
`8c5fc1f6d3ffc760f4ad02d360bd754dd8c98136e79973d91dd98b88ced190b8`.
Raw root: intel2
`/home/maik/efs-artifacts/taskH-wave33-20261029/aed85c52f/`.
Controller `run.sh` and `source-manifest.json` are retained there. Each run
retains all command transcripts with hashes, binary before/after hashes and
all failed fixtures. Raw outputs and stores remain outside git.

| Run | Gate line | Verdict |
| --- | --- | --- |
| Initial run 1 | `TOTAL 557 FAIL 1 NOTRUN 10 STATUS FAIL (853s) OVERBUDGET 8 BUILD 0s` | FAIL; retained, not a requested zero-failure gate |
| Initial run 2 | NOTRUN after first failure | No completion claimed |
| Required clean run 1 | `TOTAL 557 FAIL 0 NOTRUN 10 STATUS INCOMPLETE (1021s) OVERBUDGET 8 BUILD 0s` | Zero-failure full run; host limitations remain explicit |
| Required clean run 2 | `TOTAL 557 FAIL 0 NOTRUN 10 STATUS INCOMPLETE (823s) OVERBUDGET 7 BUILD 0s` | Zero-failure full run; host limitations remain explicit |

The sole initial failure is case 465, `test_bench_published.py`: the first
published data write and expected name-collision refusal succeeded; the
metadata phase returned UNREGISTERED for one MKDIR and invalid-parent errors
for subsequent creates. The strict payload/count oracle failed. The failed
three-node fixture is kept at intel2
`/home/maik/h33-1/efs-w170-dedptk5z/bench/`; no verification claim is made for
it. All 557 transcripts were copied and hash-verified before the controller
stopped. The 581-file `run-1/closed-manifest.json` SHA-256 is
`8cccd973a76b917d30d795f4d0dc4d692dbed6044854c1db6fcd23817254f6c4`.
Binary hashes match and no owned processes/mounts remain; the manifest is
text evidence, not a payload audit. Run 2 did not start. No limit/assertion was weakened.

Task AV started on intel2 during the run, after the initial quiet check.
Its source/processes are under `/home/maik/efs-taskAV/`. This is a disclosed
isolation confounder, not proof of the failure's cause. A clean-devel W170
comparison at `clean-devel-w170/` refused its process guard before build;
that comparison is NOTRUN, not a baseline result. Other agents' resources
were not stopped. A second exact clean-devel `330c48aea` comparison at
`clean-devel-w170-2/` completed W170 with exit 0, but its observer detected
six Task AV daemons near the end. This is a functional PASS with incomplete
isolation, not proof of the merged failure's cause. Clean-devel archive
SHA-256: `feb3e0bfed82a043eca71fe2bd0dd9e0faab00a166e69423a2d95b7ef5822b9a`.
The comparison text manifest SHA-256 is
`4d988964700124ad785fc21049e77ba6da638a73cc907a9a059565b9d9b88ebe`;
before/after binaries match and its owned processes are absent.

The two fresh same-source complete gates are retained at intel2
`/home/maik/efs-artifacts/taskH-wave33-20261029/aed85c52f-repeat1/`, using
`/home/maik/efs-taskH-wave33-20261029/repeat1/run-{1,2}/src` and fresh
`/home/maik/h33-r{1,2}` scratch. The controller waited for Task AV's own
`GATE-DONE`, then passed the quiet check and started a two-second foreign-EFS
executable scan; detected overlap makes the controller refuse qualification.
It never signals foreign processes. Each run executes unchanged W170,
ticket-owner, data-commit and READDIR preflights; any preflight or full-suite
failure stops the controller and preserves evidence.
Task AV completed; the quiet check and all four preflights for both runs passed.
Clean run 1 retained all 557 command transcripts with verified hashes. Its
590-file closed text manifest SHA-256 is
`cade5db02177434150b4631220bb05338b69224ff8728bf7f30c06aff63eb1d1`.
Its binaries match before/after and its owned processes/mounts are absent.
The ten NOTRUN results are nine RDMA hardware-path cases (no usable IB
device) and the host-refused sanitizer attach probe; no suppression was added.
All eight soft-budget overruns remain in `summary.txt`; hard failures were
not reclassified. No foreign EFS executable was observed through that run.
Clean run 2 also retained all 557 command transcripts with verified hashes;
its 590-file closed text manifest SHA-256 is
`ffaecf125b63cd6ec718e0308261de012628f21ed4be6438a9f800e0cbee7fd1`.
Its binaries match before/after and its owned processes/mounts are absent.
It has the same ten host-capability NOTRUN results and seven retained
soft-budget overruns. K38 shared-chunk pressure, K22 leader admission and
W130 commands are included in the full recipe, not replaced by preflights.

The controller closed with exit 0 / COMPLETED. The 1,162-file qualification
manifest SHA-256 is
`a403b10c8c02f7ac2d7c5b6b7ff4042fb81d229b4136a9d798c4e8259e66646d`.
The append-only foreign-EFS log is empty and polling found no foreign EFS
overlap; closure found no recognized EFS daemon/client process on intel2.
**Observer limitation:** its terminal snapshot was not produced and the
background observer is no longer live. Empty logs/polling are evidence of
no observed overlap, not a complete end-to-end isolation certificate or
proof of absence of unrelated host CPU/I/O load. This limitation is retained
in `qualification-closed.json`, not reconstructed as a successful snapshot.
All failed evidence and stores stay on their original hosts; no bulk export
or pruning occurred. No code/test/script changed after tested merge
`aed85c52f`; later commits contain inventory/evidence only.
Diagnostic CREATE-result logging alone does not explain the initial failure:
devel's CREATE wrapper calls the same underlying API. The failure's attribution
remains unproven; later passing runs do not erase it.

Use `EFS_FAULTS=1 EFS_TRANSPORT=tcp EFS_TEST_TARGET=test-all EFS_TEST_JOBS=8
EFS_BUILD_JOBS=8 TMPDIR=NEW_EXTERNAL_SCRATCH scripts/make_test_summary.sh`
in a fresh exact-source tree with the prerequisites above. The retained
controller records exact build flags, preflights, source identity, complete
outputs and cleanup scope. No test limit is raised to satisfy the requested
zero-failure gates. Coordinator owns the eventual merge and refactor cuts.
