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
