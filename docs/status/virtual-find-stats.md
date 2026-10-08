# Fixing the virtual .find and .stats interfaces

Status: open; implementation and live acceptance remain required. Reviewed
Oct 7, 2026 against HEAD `5e7e2bd5` plus the current working tree. These are
synthetic read-only FUSE interfaces, not block or character devices: `.stats`
is a regular file; `.find` is a virtual directory with query files. Keep the
existing lookup-only interface unless a separate product decision changes it.

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
