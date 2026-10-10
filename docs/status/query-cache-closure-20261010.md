# W92 closure work — acceptance ledger

Status: in progress, devel-find-stats, exclusive Xorinox. Closure is not claimed.
The user explicitly requested work until W92 is closed. The existing 2^32 synthetic
index and staged real-filesystem requirements remain binding; the earlier three
profiling rounds do not satisfy them.

| Gate | Current state |
|---|---|
| Immutable disk catalog, incremental exact aggregates, bounded builder scratch | implemented, differential/live/cold gates pass in previous checkpoint |
| Aggregate retained-cache admission | implemented; focused and complete native suites pass; live deployment gate pending |
| Durable changed-key watermarks and bounded catch-up/recovery | pending; current revisions are process-local hints and cold rebasing scans authority |
| Explicit cardinality/offset/budget audit | in progress |
| Increasing-cardinality RSS, cold-cache, concurrent export/mutation and freshness | pending |
| Synthetic partitioned index of at least 2^32 entries | pending; sparse offset probes alone cannot satisfy this |
| Staged real-filesystem and fault/platform gates | previous smaller gates pass; closure matrix pending |

## Aggregate cache admission

`EFS_QUERY_DISK_TOTAL_MIB` defaults to 32768 MiB per private cache root; accepted
configuration range 1–16777216 MiB. The existing per-generation maximum defaults
to 16384 MiB, and filesystem free reserve defaults to 64 MiB. These are conservative
logical-byte guards, not physical block reservations against other disk writers.
Large-scale deployments must provision and configure larger budgets explicitly.

A nonblocking `.budget.lock` serializes builders in one root without blocking
query readers. Before creating a candidate, scan its existing generation files
and root-level regular files with bounded directory iteration. Shared links are
counted per generation; orphan candidates and copy files count. Existing usage
plus the candidate's conservative charge must fit both limits, including temporary
copy/sort work. Concurrent lease releases only reduce physical usage; admission
keeps its conservative initial total until the next build. A crashed builder's
lock is released by the kernel, and its remaining files count on the next attempt.
There is no volatile ledger that could forget retained generations after restart.
The root must be a private cache directory; unexpected directories/symlinks refuse
admission. Lowering a total limit does not prevent reading a valid old checkpoint.

Focused actual-source tests cover shared links, orphan copies, exact boundary,
concurrent builders, cancellation/retry, lease-release recovery, configured caps,
and preservation of the old checkpoint after aggregate refusal. Existing mutation,
retained-view, partial IO and fsync tests and the complete native suite pass. Logs are retained in
[closure evidence](../../results/measure/20261010-w92-closure/).

The root scan is proportional to generation file count (at most four partition
files per populated shard per generation), not file cardinality; costs under
long-lived exports still need measurement. Aggregate admission alone does not
finish durable incremental recovery or the scale gates.

[W92](../backlog/work-items.md#w92) · [status index](README.md).
