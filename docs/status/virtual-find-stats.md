# W92 — .efs/find and .efs/stats replacement commands

Status: open. The accepted Oct 8 user revision replaces synthetic `.find` and
`.stats` with executable `.efs/find` and `.efs/stats`. W91 supplies readable
Python command source and the separate versioned query path. Real user entries
named `.find`, `.stats` or `.efs` must remain available. Compatibility with the
legacy synthetic paths is not required.

The [Oct 8 command checkpoint](efs-commands-20261008.md) records implemented
command/query infrastructure, tests, activation and remaining acceptance.
Earlier findings below are historical; do not mistake the former client tree
walker or missing server catalog for the current source/deployment state.
W92 remains open pending loaded/scale/fault and other declared gates.

## Required direction

The existing [server-owned refresh design](../backlog/ideas.md#parked-server-side-stats--find-refresh)
remains the proposed architecture: server-owned directory rollups and a
partitioned name index; reads consume committed snapshots. Refresh on either
object/change lag exceeding N or elapsed time exceeding T. Rename-only trees
must converge. Do not rebuild on cat, make a client-local index authoritative,
rebuild an entire index per create, or route all index work through one shard.
This status entry activates planning and acceptance work; it does not assert
that a wire format, defaults, or implementation have already been approved.

## Current code and gaps

`src/client/efs_fuse.c` has the following behavior:

- `stats_compute_locked` runs `efs_export_ensure_rollups` against the local
  export table under `g_client.lock`. The 16-entry cache defaults to a 1-second
  TTL. `as_of` timestamps rendering, not a server commit watermark or proof
  that peer mutations are included. Cached reads avoid the global lock, but
  cache misses still perform rollup work under it. The comment claiming a
  brief lock is not a measured upper bound.
- `find_query_ensure` performs recursive READDIR RPCs on cache miss; the cache
  has 16 slots and a 5-second TTL. Thus getattr/open/cat can trigger a tree
  walk. `find_walk_dir` returns success after the 65,536-entry or depth-128
  limit, and skips paths that do not fit its buffer. Callers cannot distinguish
  complete results from limited results. Reads are not an atomic tree snapshot.
- Both read paths obtain a fresh-or-cached rendering per read request; open
  stores the synthetic inode rather than retaining an immutable text snapshot.
  TTL expiry, concurrent queries or cache eviction can change bytes/length
  between offsets on the same fd. Define and implement per-open snapshot
  ownership, consistent size/EOF, seek/read and release semantics.
- The virtual branches of `lookup_child`, getattr and open precede ordinary
  parent/search permission checks. The recursive find walker has no caller
  access check for descendant directories, and the result cache is keyed by
  directory and pattern rather than caller authorization. Implement explicit
  permission enforcement and authorization-safe caching; verify behavior as
  unprivileged users rather than relying on synthetic 0444/0555 modes.
- `find_query_len` collapses failures to -1; getattr/open translate that to
  ENOENT, while read translates query errors to EIO. `.stats` read similarly
  maps snapshot failure to ENOENT. Preserve invalid-query, access, allocation,
  transport and missing-object distinctions in the syscall error mapping.
- Feature polling keeps the last mask on outage and virtual reads recheck it.
  Define bounded polling/deadlines and disable semantics for already-open fds,
  kernel-cached entries and a failed refresh. A disabled feature must not be
  recreated by stale query state.

These are source-level findings and required gates, not newly reproduced
production incidents. Existing POSIX tests `virt_stats_readable`,
`virt_find_query` and `virt_find_not_a_real_dir` provide basic read-only smoke
coverage; their feature-off soft skips do not validate the cases above.

## Implementation sequence

1. Specify snapshot scope and identity: export/directory, committed source
   watermark(s), snapshot version, permissions, freshness and completeness.
   Define initial-build behavior, query matching compatibility and filename
   escaping (including newline names), and distinguish unavailable from empty.
2. Maintain partitioned server rollups/name indexes from committed namespace
   changes: create, unlink, rename, hardlink, size/publication changes and
   directory movement. Preserve immediate versus recursive counts and sparse
   logical versus allocated-byte meanings. Handle cross-shard transactions
   without publishing half-applied changes. Rebuild from durable metadata at
   recovery with bounded work; never from a FUSE client's partial table.
3. Add bounded background refresh by N/T with per-partition watermarks,
   batching, backpressure, leadership/restart recovery and failure diagnostics.
   A watermark must not advance past work that failed. Determine whether lag
   is namespace mutations or objects; rename/delete must count toward it.
4. Add versioned snapshot/query RPCs and compatible clients. `.stats` reports
   separate stats and name-index build times, watermarks/change lag, age,
   configured N/T and overdue state. A rendered `as_of` alone is insufficient.
   `.find` consumes the committed index and explicitly reports or errors on
   limits; it must never present incomplete output as complete.
5. Pin immutable results per open, enforce permissions before execution and
   delivery, bound concurrent query memory/output/time, handle eviction and
   release safely, and retain accurate error codes. Cache authorization must
   remain correct after chmod or ownership changes. Specify NFS path rendering:
   gateway-local absolute paths are not necessarily valid on an NFS client.

## Acceptance required before closure

- Two independent cold mounts observe create, peer write/publication, unlink,
  rename, hardlink and moved subtrees after the declared lag budget; a quiet
  rename-only tree refreshes on T. Stats and index freshness are separate.
- Repeated cat/stat adds no full rollup/tree walk; concurrent writers show
  bounded lock hold, RPC work and tail latency. Record baseline, workload,
  N/T, node count, build and RSS rather than claiming an unmeasured speedup.
- Multi-read/seek fd output remains one coherent snapshot across refresh,
  cache pressure and mutation; fresh opens converge within policy. Test more
  than 16 concurrent directories/queries, release, restart and outage.
- Unprivileged users cannot query protected directories or reuse another
  user's result. Cover nested permissions, supplementary groups, chmod,
  symlinks, FUSE and gateway/NFS credentials. Permission denial is EACCES.
- Large/deep trees, long/newline/non-ASCII names, invalid patterns, RPC failure,
  allocation failure and cancellation produce explicit outcomes without
  silent truncation or partial-success output. Validate resource bounds.
- Feature on/off transitions work on live mounts, including cached dentries,
  open fds and outages under the specified policy. Tests must explicitly
  enable each feature and fail if an expected interface is missing.
- Failover/restart resumes or rebuilds indexes without lost changes or false
  freshness. Run full POSIX and peer suites plus focused snapshot/fault gates;
  retain logs and exact deployed build IDs.

No code or feature defaults were changed by this review. No live deployment
or performance acceptance was performed.

## W92 activation and first repair checkpoint — Oct 8

Work proceeds on `devel-find-stats` from committed `devel` e86b85ac in the
isolated `work/efs-find-stats` checkout. NUC's prior deployed baseline fails
all four new `tests/live/virtual_interfaces.py` gates: open .find output grows
from 3500 to 3534 bytes after TTL/mutation; peer-created 4096-byte file leaves
.stats counts zero; invalid pattern returns ENOENT; protected .stats succeeds.
Raw evidence: results/measure/20261008-w90/baseline.jsonl.

First repair pins immutable bytes in tagged virtual open descriptions, releases
without normal file leases/writeback, retains per-open size across refresh,
rejects silent walk depth/entry/path/output limits, preserves query errno,
and enforces parent/subtree read/search permissions. Non-root legacy query
cache reuse is disabled pending authorization-safe server query snapshots.
Open-result bytes have a 64 MiB aggregate limit; query output a 16 MiB limit.
Literal newline paths fail EILSEQ until a versioned output format exists.
This is a correctness foundation, NOT closure: client-local stats and legacy
read-triggered walks still need replacement by server snapshots/indexes.

The first client repair compiles without warnings on NUC. Live fixed gates,
server architecture, staged 400 GiB population and final acceptance remain
outstanding at this checkpoint. Feature switches SET_FEATURES are retired in
current protocol; revisit the older toggle gate wording against current code.

Second checkpoint: `d078711b` passed the full NUC POSIX suite (216 pass,
0 fail, 1 skip; 75.2 s) and peer suite (64/64; 49.9 s). The subsequent inode
size fix `4f3aa3b0` repeated these at 68.6 s and 48.1 s. `467244c3` uses
content-versioned query identities; focused immutable snapshot, invalid query,
and permission gates pass. Stats still fails (peer-created file reports zero).

Capacity testing subsequently exposed kernel lookup-reference retention:
expired dentries were not forgotten after query fds closed, eventually filling
all 64 query identities. The repair explicitly queues dentry invalidation after
lookup replies and on close, preserving open snapshots until release/FORGET.
A regression covers 256 distinct stat-only queries, 128 sequential opens,
two exhaustion/drain cycles, and a fresh query in another directory. With the
repair, recovery and subsequent snapshot gates pass on the real mount.
The older phase3 capacity FAIL (80 opens) used the same unchanged query 80
times: sharing an immutable identity is intentional, so that harness expectation
was wrong. The revised capacity gate opens distinct queries.

Population is resumable in the dedicated `w90-scale` tree via
`tests/live/w90_populate.py`: 131,072 16-KiB files plus 64-MiB bulk files,
staged 16/64/128/256/380 GiB new logical bytes. Existing ~20 GiB is preserved.
Stop with `/data1/efs/w90-control/PAUSE`. Each underlying store filesystem
must retain 50 GiB and 100,000 free inodes. Initial overnight pipeline aborted
before any population when the capacity regression exhausted query identities;
do not interpret that aborted run as load or final acceptance evidence.
Server snapshot/index implementation and its acceptance gates remain open.

Work-item numbering reconciliation: the independently updated main devel docs
now allocate W90 to scratch-file expiry and W91 to the directory-local .efs
command interface. This branch's find/stats repair is therefore **W92**. Earlier
commits, `w90-*` test/load paths and retained evidence keep their historical
labels; they refer to this repair, not scratch expiry. Do not overwrite or
import concurrent main-checkout edits while reconciling documentation.

Latest executable continuation state: [Oct 8 checkpoint](find-stats-checkpoint-20261008.md).


Offline query foundations now exist on this branch: full local KV snapshots
(including unflushed state), per-shard metadata dirty hints and bounded immutable
name-candidate indexes. See the latest checkpoint for commits and retained unit/
sanitizer evidence. They are not connected to live server refresh/RPCs yet;
.stats remains incorrect and W92 remains open.

An offline immutable catalog now folds logical sizes through the real metadata
getattr reducer, reuses unchanged shard caches/name indexes, and resolves split
layout precedence before stats/find output. Catalog and name-index unit tests
pass on NUC and focused Mac ASan/UBSan. These are builder foundations; host
publication, fresh permissions, RPC consumers and live/scale gates remain open.
See the continuation checkpoint and query-catalog evidence ledger.

Host background publication now has a private four-node smoke gate covering
Raft mutations, quiet refresh, partial-host refusal and persisted restart
rebuild. Bounded internal read callbacks are staged; user query RPCs and fresh
permission checks remain unconnected. Busy writes may refuse a transaction-safe
cut and exceed T, so expose old publication age/error. Full live and scale gates
remain open. Details and raw evidence are in the latest continuation checkpoint.


The v1 RPC and FUSE consumers are now connected on this branch and pass a private
NUC fixture: nine virtual gates, full POSIX/peer and persisted restart against an
independent oracle. Same-parent name binding and canonical path rendering defects
were fixed. Main NUC is **not yet deployed**; its old stats/walk behavior remains.
See the latest checkpoint and query-rpc evidence for explicit budgets, aggregate
permission policy, retained failures and pending loaded/scale/fault/NFS gates.
