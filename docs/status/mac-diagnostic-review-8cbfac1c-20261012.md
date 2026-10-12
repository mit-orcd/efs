# Mac peer review of 8cbfac1c — 2026-10-12

Exact diagnostic commit: 8cbfac1c32c0475f2951cb571cd9f289d748d989. Review and regression execution only; no deployment or release edits.

PASS independently on Mac: actual capture mixed-cut safety/count/scheduler tests normal and ASan/UBSan; actual C work sender normal and ASan/UBSan; deep60 native phase tests including unchanged canonical operation AST, worker entry, controlled leaf failure, missing/full observers, cap and normal cleanup. Initial sandbox Unix-socket denial was environmental and the authorized unsandboxed rerun passed. A missing source header in the first archive was corrected before successful runs. No failed first invocation is treated as a product failure or acceptance PASS.

Source review found no changed fence, scheduler decision, durable format, request return or original case operations/deadline. Sender emission is after host/query locks and retired-catalog release. Record attempts are bounded, sockets nonblocking and descriptors balanced. This is suitable for one coordinated diagnostic run with synchronous legacy trace unset, subject to AMD's normal/fault/sanitizer prerequisites and the manager's authorization. It is not quiet acceptance.

Interpretation limits communicated to AMD and manager:

- capture_ms uses the existing attempt `now`, preceding lag/query-lock work. It is whole-path capture elapsed, not exclusive host_query_source duration.
- disk restore/catchup are not timed in build_ms; total residual also includes import revalidation, snapshot/candidate release, publication locks/logs and retired free. Do not attribute residual to fsync or paired authority calls.
- reason is final capture reason; final rc may instead be a build/checkpoint/publication failure. capture_rc distinguishes them.
- pairs counts API invocations, not wire attempts, retransmissions or quorum-proof coverage. Dropped datagrams and exhausted caps imply unknown work.
- observer shutdown follows `stop()` in fixture finally. A stop exception can skip observer/handle cleanup. Normal cleanup passes but exceptional-stop cleanup is not proved; recommend nested finally to close the observer without hiding the primary failure.

The proposal's own documentation already records whole-path timing and restore/catchup residuals. Those are analysis constraints, not evidence that the original-budget gate is fixed. Independent Mac retained inode 4091 remains a separate graph-consistency blocker.

## Follow-up 20a3c872

Bounded changed-bits review and independent tests PASS: exact `source_ms` wraps host_query_source, existing policy `capture_ms` and scheduler decisions unchanged; initial restore/catchup get a separate `restore_ms`; record label now explicitly `capture_reason`. Observer shutdown runs in finally if daemon stop raises. Actual shutdown AST test preserves the controlled stop exception and proves socket/thread cleanup; native deep60 observer test PASS. Actual sender and capture functions ASan/UBSan PASS on Mac. Residual is still residual, timings include internal callbacks and millisecond rounding, and missing records remain unknown. No alternate-source deployment or release execution performed.

Human/manager narrows scope to true defects only. These observations support diagnosing the original correctness failure; they authorize no retry tuning, optimization, benchmark comparisons, or acceptance inheritance.

## Shared freeze 31abdd5e

Reviewed exact31abdd5e03be224fb95ca484ddbf3fa9dfd2899b minimal inverse. query_refresh.h is byte-identical to95d3f1b1. Publisher scheduling uses original refresh_due and backoff; no accelerated admission state remains. Capture fences/four-cut/proof/import/deadline body preserved. fast_eligible is explicitly diagnostic-only; actual policy_interval_ms is recorded. Historical quiet-window optimization oracle is removed from release qualification, while the unchanged actual mixed-cut safety assertion remains first. Independent actual capture normal/ASan+UBSan PASS; removing the fence fails that intended mixed-cut assertion in both modes. Native sender and phase/exceptional cleanup controls PASS. Corrected cleanup preserves failing stop as primary when observer also fails and closes all handles; no acceptance inheritance or deployment.

Manager approves one common frozen candidate for all five agents. Mac verifies source bundle SHA2564df62867ebdd4e7da1b9e59000fa24edfc4150b606e12d5f4129f52394168a5a, 1557-input manifest3c377f4776a66b4567bf9f4c078663aaffb7ac318e25667b939b18659c89e169 and52-directory manifest272154ba5370b5c23d5defaef1e4f497c4be4fbc32daf1dea788f4d0809a3110. Isolated Mac build in `/tmp/efs-mac-31abdd5e-l4zcba`; all1557 source contents verified before build and all52 directories materialized. Installed stores and service untouched. Supplemental97f1c77e is outside this freeze and will not be added to executing inputs or inherited into gates.

Shared31abdd5e isolated Mac Linux execution: normal real-LSM audit, retained-orphan preflight, actual capture and sender PASS. Fully instrumented ASan/UBSan real-LSM audit PASS. Phase observer test FAILS before observer startup at canonical AST hash: Fedora Python3.13.13 ast.dump omits empty lists by default and adds FunctionDef type_params schema. Read-only normalization (show_empty=True and exclusion of verified empty type_params) restores exactly expected1cdb1d66e21f56f3a1e03b878d8cfef6807168c7d04c026f70823b840d2357e7. This is a reproduced test portability defect; AMD/manager notified for minimal central fix. Executing source is unmodified; phase test remains FAIL for this candidate, not an inherited local-host PASS. Full native/fault and mounted suites NOTRUN in this Mac scope. All1557 file hashes and52 directories reverified after tests; installed efsd active and unchanged. No pressure/optimization work or owned mounted fixtures.
