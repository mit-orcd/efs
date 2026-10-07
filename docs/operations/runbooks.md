# Measurement runbooks

[Operations](operations.md) · [Cluster testing](cluster-testing.md) ·
[Performance contract](../how-it-works/performance.md) · [Queue](../status/README.md)

The Sep 21–Oct 1 investigation instructions and measurements are
[archived intact](../archive/measurement-runbooks-20261007.md). They target the
recorded fcstor deployment and include obsolete execution/approval tooling;
they are not a current NUC/xorinox inventory or an agent's next-work queue.

For a new measurement, select the current queue item and its named gate, read
the matching script and deployment runbook, and verify running build/topology.
Preflight may itself contact the cluster; run it only in the authorized scope.
Use an isolated namespace and preserve existing data. Remounting clients,
profiling and changing membership are mutations even when binaries stay the
same. Follow current authorization/tooling rather than a historical approval
parameter or shell-helper path.

Retain raw output, exact invocation, build, configuration and failure status
with each summary. Distinguish flush-in-clock throughput from persistence
acceptance, process restart from power loss, and engine measurements from
FUSE/cluster results. Do not infer a pass from a script's existence.
Update the queue/index with evidence and the next gate; put dated narratives
in the archive rather than growing the active handoff into a result log.

Historical script families include `samedir_rate.sh`, `open_cost.sh`,
`ior_hard_scaling.sh`, `dd_wall.sh`, `posix_isolated.sh`, `posix9_none_count.sh`
and `raft_snap_state.sh` under `tests/measure/`. Their old numbers and limits
remain in the linked archive and performance guide; inspect current source
before rerunning them against a different build or deployment.
