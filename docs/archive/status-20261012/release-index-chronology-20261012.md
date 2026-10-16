# Superseded release-index chronology

Historical source-specific checkpoints below are not current runtime instructions or release gates. Current acceptance is indexed in docs/status/README.md.

## Historical release continuation

This chronology precedes the human stop and current 410/v2 checkpoint; running/next/deployment statements below are historical, not authority to resume.

Query stage follow-up: [ORCD diagnosis](../../status/orcd-query-starvation-disposition-20261012.md)
and [Mac boundary review](../../status/mac-query-stage-disposition-20261012.md) support separate
opt-in stage instrumentation (capture/authority/transaction/import, pending shard/kind,
build/checkpoint/publication lock and request root/generation). Focused normal,
trace-enabled and sanitizer regressions pass; new full frozen gates precede one
diagnostic run. No additional correctness fix is proven yet. Diagnostic review notes initial
lag-update writer lock is unmeasured and tracing perturbs latency; keep separate
quiet acceptance.

[AMD query-stage diagnostic](../../status/v030-query-stage-diagnostic-20261012.md): d485 full
normal/fault plus sixteen sanitizers and ordinary both-mode cold/cleanup PASS;
single instrumented run FAIL/restored0. Observed146 transaction-cut
collisions, zero recorded pending refusals,46.915/48.816s publication gaps.
Separate coherent-cut/root diagnosis and foreground admission/proposal analysis
are assigned; no safe additional repair is proven.

The coherent-cut actual-helper negative control now rejects an A-only image
after sideB delivery/revision advance; normal/sanitizer pass and fence-removal
test mutant fails. [Oracle and limits](../../status/v030-query-stage-diagnostic-20261012.md).
No production ordering/fence repair is inferred.

Second bounded query discriminator is prepared: fixed txn-kind PUT/DEL activity
hints with applied vectors, precise root/binding leaf reasons/durations and initial
lag-lock timing. [Definition and pending qualification](../../status/v030-query-stage-diagnostic-20261012.md).
No fence filtering, source promotion or quiet acceptance.

Release diagnostic update: peer reviews `aaf6a3e8` / `79adbf22` integrated; cut activity and nonzero initial lag-lock events now emit after unlock. [Diagnostic limits and pending gates](../../status/v030-query-stage-diagnostic-20261012.md#reviewed-diagnostic-freeze-preparation).

AMD `57748bb5` validation stopped on the new root test fixture indentation warning; mounted gates did not start. Fixture corrected, fresh frozen validation required; see [diagnostic record](../../status/v030-query-stage-diagnostic-20261012.md).

Release `95d3f1b1`: full normal/fault suites, 17 focused sanitizers and fully sanitized LSM catalog PASS; both-mode ordinary overwrite/cold/physical prerequisites PASS. One instrumented run still FAIL: retained roots absent, 145 transaction-cut collisions, 47–48 s publication gap; cold checks NOTRUN, mounts restored. [Exact evidence and limits](../../status/v030-query-stage-diagnostic-20261012.md#second-frozen-amd-diagnostic-95d3f1b1). No push or release acceptance.

A local cut-only retry proposal now has baseline-fail/candidate-pass and unsafe-all-BUSY negative controls, preserving transaction/authority/import fences and existing budgets. It increases proof load and is not deployed or accepted. [Policy and evidence](../../status/v030-query-stage-diagnostic-20261012.md#local-scheduling-remedy-proposal-not-deployed). Reviewed Intel namespace fixture repair is integrated, repaired live qualification NOTRUN.

Reviewed frozen `b239dc7d` (1,517 verified inputs) handed to NUC/ORCD; AMD full prerequisites and gated quiet qualification underway. A separate test-only mixed-cut oracle order correction isolates the safety negative control, without changing executing inputs. [Handoff and qualification scope](../../status/v030-query-stage-diagnostic-20261012.md#frozen-qualification-handoff-and-separate-oracle-checkpoint).

Frozen `b239dc7d` full normal/fault, 18 focused sanitizers, fully sanitized LSM and both-mode ordinary overwrite PASS. Its one QUIET direct run still FAIL; buffered/cold checks NOTRUN, mounts restored and jobs ended. Shorter cut retry has no demonstrated release-pressure benefit. [Exact quiet evidence and next discriminator](../../status/v030-query-stage-diagnostic-20261012.md#reviewed-candidate-quiet-result-b239dc7d-still-blocked). No push, release tag or promotion.

Local bounded nonblocking deep-case/capture-work diagnostics now pass focused native/sanitizer/failure-observer checks; workload/fences/deadlines unchanged. Peer/full Linux gates pending, no new live run. [Next discriminating plan](../../status/v030-query-stage-diagnostic-20261012.md#local-nonblocking-phasework-diagnostic-proposal).

Diagnostic follow-up preserves original stop exceptions while guaranteeing observer cleanup; separates policy whole-path capture from source/restore API intervals. [Peer scope and pending freeze](../../status/v030-query-stage-diagnostic-20261012.md#peer-reviewed-diagnostic-timing-and-cleanup-follow-up). Retained Mac backlink inconsistency remains a separate blocker with guarded refusal; NUC direct ordinary gates are source-specific, not AMD pressure acceptance. Current W97 means receiver feedback/queue pressure; historical root-walk comparison is W111.

Release scope is now true-defect testing/fixing only. The next shared candidate restores pre-b239 scheduling and quarantines the unproven retry optimization; all fixes/fences/diagnostics retained. Diagnostic eligibility is historical, not active policy. All owners must qualify the same immutable candidate after safe drain/restoration. [Exact inverse and remaining gates](../../status/v030-query-stage-diagnostic-20261012.md#defect-only-shared-candidate-optimization-quarantined).

- [NUC repair gap: source trace, initial-scope fencing dependencies and strict protection gates](../../status/nuc-repair-gap-design-20261012.md) — W74 remains open; no repaired-protection claim.

October 12 qualification checkpoint: frozen `def46109909c793e58ae2c453c5de537cdb2c627` AMD normal/fault native and affected sanitizers PASS; NUC core/peer/two-client PASS; matched-4G seven-client Intel and NUC FAIL. Receiver drain scheduling investigation continues before a new central freeze. See [current qualification ledger](../../status/v030-tcp-direct-qualification-20261012.md).

Current central qualification freeze: `03bd89fb7de56c2c6af44ab3bd26c54caf4b8274`, bounded FIFO inbox drain. New full/live gates NOTRUN; previous def failures retained. [Qualification ledger](../../status/v030-tcp-direct-qualification-20261012.md).

09:10 EDT checkpoint on `03bd89fb`: AMD full native/fault/sanitizers PASS; NUC core/peer/isolated PASS; Intel prerequisites PASS; Intel/NUC seven-client gates RUNNING. Release acceptance pending. [Ledger](../../status/v030-tcp-direct-qualification-20261012.md).

Final `03bd89fb` live verdict: Intel and NUC seven-client FAIL. NUC data/cold/physical/drain checks PASS independently; [NUC report](../../status/nuc-release-03bd-group3-20261012.md). Targeted deep-path diagnostic next; candidate unaccepted.

Intel03 also reports EIO/EBUSY in fsync/basic I/O cases; classification pending from retained logs. These remain explicit release blockers alongside deadlines. [Ledger](../../status/v030-tcp-direct-qualification-20261012.md).

Qualification STOP checkpoint: newer human stop reported by Intel; no further launches/fixes pending resume.03bd remains unaccepted with seven-client and hard I/O blockers. [Ledger](../../status/v030-tcp-direct-qualification-20261012.md).

Latest alpha scope: `.efs` commands/stats/find and derived query-cache service excluded; explicitly disable client/server command flags. WITHOUT-query acceptance NOTRUN; ordinary-I/O/deep-path blockers remain. [Scope](../../status/v030-release-scope-20261012.md). Work remains stopped.

RESUMED by direct human instruction: frozen `ecdc8c6034b610a2bf69e7a79664d8a5e990de84`, `.efs` disabled. AMD heavy full qualification, Intel focused I/O failures, NUC lightweight deep/configuration.15-minute reporting active; new live results pending. [Scope](../../status/v030-release-scope-20261012.md).

Frozenecdc split progress: Intel focused70/70 I/O+client stops PASS (evidence integrated); NUC focused14/14 deep+config/peer/cleanup PASS (evidence pending). AMD heavy seven-client qualification RUNNING. [Ledger](../../status/v030-tcp-direct-qualification-20261012.md).

NUCecdc focused evidence integrated: [report](../../status/nuc-release-ecdc-focused-20261012.md),14/14 deep checks plus disabled-feature/configuration/basic/peer/cleanup PASS. Reduced workload; AMD full acceptance still separate.

10:04 EDT ecdc: AMD native/sanitizers PASS, full .efs-disabled seven-core FAIL on deep/names deadlines; cold bytes PASS, globalphysicalempty timeout requires live-object attribution. Intel/NUC focused PASS retained. [Ledger](../../status/v030-tcp-direct-qualification-20261012.md).

AMD bounded labelledecdc broad diagnostic running; isolatedREADDIR5e035dcd review pending, not deployed. Acceptance remains failed. [Ledger](../../status/v030-tcp-direct-qualification-20261012.md).

10:19 EDT: current frozen qualification `e59e815fab2916340baad585dc08712ed182b7ea` reviewedREADDIR fence reuse; new gates pending. ecdc diagnostic reproduces directory failures; isolated oracle running. [Ledger](../../status/v030-tcp-direct-qualification-20261012.md).

Isolatedecdc cold/physical/queue **PASS20.095s**; broad deadlines/globalemptyFAIL retained. Newe59 candidate preparing common tests.

Newe59 NUC21/21focused deep/readdir PASS evidence integrated; Intel70/70I/O PASS but listing/names timeout13cases. AMD native/fault progressing,broadqueued. [Ledger](../../status/v030-tcp-direct-qualification-20261012.md).

E59 AMD nativePASS/fullFAIL. Corrected Intel actualworker evidence points toward flush/context-exit costs, not measuredREADDIRdelay. Additionaldiagnostic rejected byautomaticapproval citingnewerstop; humanclarificationpending, no newlaunch. [Ledger](../../status/v030-tcp-direct-qualification-20261012.md).

Humanclarification explicitly resumes qualification/diagnostics. Intel targetedRPC/readtiming run dispatched on frozene59; ordinarydurability/deadlines unchanged. [Ledger](../../status/v030-tcp-direct-qualification-20261012.md).

IntelRPCdiagnosticevidence integrated; genericprofiler excludes manual publication/PUT steps. Flush bottleneck attribution remainsincomplete; root reviewing exactphases. [Ledger](../../status/v030-tcp-direct-qualification-20261012.md).

Isolated11859810 opt-in phase diagnosis preparing on AMD Linux; build/focusedprerequisites guard launch. Acceptance freeze remains e59, no passclaim. [Ledger](../../status/v030-tcp-direct-qualification-20261012.md).
