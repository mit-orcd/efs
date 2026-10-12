# v0.3.0-pre-alpha scope decision — October 12, 2026

User decision: “the container stuff is not needed for this release”; “repair and
recovery is not needed for this release”; “we only focus on tcp/directio”.

Release qualification targets the files storage engine over TCP with direct I/O.
Containers, buffered-I/O and RDMA qualification, and repair/recovery implementation
and acceptance are deferred beyond this release. Their existing failures, gaps
and work items remain open; this scope decision does not close them or transfer
older passes to a new source. Existing inactive repair prerequisites already
integrated need not be removed, but unintegrated experimental repair stages are
excluded from the release plan and activation remains disabled.

Small-file packing and query progress remain release priorities. Four planning
gate groups remain; this is not an exact command count:

| Gate group | Owner / cluster | Required coverage |
| --- | --- | --- |
| 1 — Read-progress safety | AMD | Actual Raft/context-fence regressions, fresh-arrival and own-apply checks, affected sanitizers and integration review. |
| 2 — Packing/query acceptance | Intel | TCP/direct I/O, original-budget seven-client workload, stats/find, deep paths and packed writes. No competing build/profile workload during quiet acceptance. |
| 3 — Files correctness and reclamation | NUC | TCP/direct I/O/files core POSIX, concurrent/peer, packing mutations, deletion/GC and physical reclamation using an unconfounded oracle. |
| 4 — Final merged qualification | AMD | Freeze accepted merged source, full normal/fault native suites and final affected sanitizer checks; reconcile Intel/NUC live results at that exact freeze. |

## Common source and parallel execution contract

All three agents start from release baseline
`8b7391b3f3edcf9fc99440d9aad3c240325acb83` on `release-v0.3.0-pre-alpha`.
Later documentation-only assignment commits do not change this implementation
baseline. No agent starts testing from its divergent Intel/NUC/AMD fix branch.
The read-progress candidatea9f158ce is not in this baseline and has not been
accepted: the coordinator must prepare a reviewed common candidate containing
only intended changes, then publish its full commit SHA before launches.
All three clusters test that same frozen candidate SHA; no independently
cherry-picked equivalent trees, moving branch tips or inherited branch passes.

Groups1–3 may run in parallel on their separate clusters after that common
candidate is prepared and testing is explicitly resumed. Each owner verifies
source inputs and normal/fault binary identities, records exact command,
configuration, commit, result and evidence, and uses isolated test stores.
Live fixtures use TCP/direct I/O/files; experimental repair remains disabled.
Any implementation correction creates a new common candidate SHA and affected
passes must be rerun on that SHA. Do not change deadlines or classify failed
prerequisites as passing dependent gates.

Group4 follows review/integration rather than running against a moving source.
If the accepted final implementation differs from the groups1–3 candidate,
Intel reruns group2 and NUC group3 on the final SHA while AMD runs group4.
This makes final native and live qualification parallel across three clusters
without mixing source revisions. If implementation is identical, retain the
verified source-specific evidence and avoid unnecessary duplicate runs.

Assignments are documented only; execution remains STOPPED until explicitly
resumed. No push, tag or shared branch promotion is authorized.

Retain existing durability protections and normal regression coverage that
checks acknowledged-write correctness. Dedicated repair, crash-recovery,
second-loss and container qualification are not release blockers under this
scope. Recovery has not been declared safe or complete; candidate notes must
state these unqualified capabilities explicitly. NFS-specific acceptance is
outside TCP/direct-I/O qualification and remains tracked separately.

Execution remains STOPPED under the user's “Document, commit, and stop” reply.
Documenting scope does not authorize new test launches, deployment, merge of
unqualified candidates, promotion, tag or push. Current evidence and excluded
commits are in the [execution ledger](amd-all-suites-20261011.md).
