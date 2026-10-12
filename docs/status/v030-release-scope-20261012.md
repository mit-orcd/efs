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

1. Review and qualify the read-progress fix: affected actual Raft/context-fence
   regressions and sanitizers before integration.
2. Pass original-budget seven-client packing/query acceptance on TCP/direct I/O,
   including stats/find and deep paths. Do not extend deadlines to obtain a pass.
3. Pass direct-I/O files correctness: core POSIX, concurrent/peer, packing
   mutations, deletion/GC and physical reclamation with an unconfounded oracle.
4. Freeze one reviewed merged revision and run normal/fault native suites plus
   applicable affected sanitizers and TCP/direct-I/O live qualification.

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
