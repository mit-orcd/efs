# W92 — local disk-backed query catalog

Implementation checkpoint, Oct 9, 2026, `devel-find-stats`, code commit `9eb3dd51`. W92 remains open;
this is functional backend progress, not acceptance of 2^32-file capacity.

## Implementation

The host background publisher defaults to `EFS_QUERY_BACKEND=disk`; `ram` keeps
the previous implementation for comparisons. The cache lives at
`<storage>/mdraft/query-cache`, outside the exported namespace. It is derived
from authoritative Raft/KV metadata and can be rebuilt without losing user data.

Each immutable generation has 4096 partition descriptors. Raw records and
binary key indexes, sorted name indexes, inode/stat rows and directory rollups
live in partition files. An external merge sort uses 2048 name rows, descriptor
access uses a 32-FD cache, and directory reduction uses a disk queue. Namespace
cardinality does not determine the builder's heap allocation. Unchanged raw
partitions are hard-linked; derived rows and rollups are currently recomputed
on disk on refresh. Exact/prefix queries seek sorted names; suffix/substring
queries scan with bounded pages.

Completed generations have a versioned native-ABI manifest and BLAKE3 file
digests. Publication checkpoints both groups' applied indexes and terms via
`current`. Restore requires a transaction-safe authoritative source with exactly
matching watermarks and verified file lengths/digests. Stale/corrupt caches are
refused and rebuilt; startup logs `query-restore`. In-memory revision counters
are rebased after authorized restore because their boot timeline can differ.

A publisher lease excludes concurrent startup recovery. Before opening retained
views, recovery deletes abandoned generation directories, preserves `current`,
and refuses directory/symlink traversal. Retained query snapshots keep their
generation files until the final reference is released. The current checkpoint
survives clean shutdown. Import-generation changes during long builds refuse
publication; disk builders check shutdown without extending RPC deadlines.

## Validation and deployment

Initial RAM/disk differential tests cover 3000 files, spilled name sorting,
paged traversal, directory rollups, rename, retained snapshot paths, fixed
accounted memory below 4 MiB, budget refusal, cancellation and unchanged raw
partition reuse. Checkpoint restore, watermark mismatch refusal, orphan cleanup,
exclusive publisher lease and corrupt-file refusal pass. A second differential
run at 12000 files also passes under the same accounted memory bound. The
free-space guard refuses writes when filesystem available space is below
`EFS_QUERY_DISK_RESERVE_MIB` (default 64; allowed 1..1048576). It samples every
256 writes; syscall errors remain checked on every write. This is a best-effort
free-space guard, not a disk quota or reservation against other writers.

A private four-daemon Xorinox direct-I/O/TCP fixture passed RPC query versions,
`.efs` display/filter/sort commands, streaming exports, permissions, POSIX, peer
and restart oracles. This is a smoke gate. The stamped committed build additionally passed RPC
publication, restart restore/rebuild and independent filename oracles. An old
fixture assertion insisting on `rebuilt=4096` incorrectly rejected a successful
checkpoint restore; test commit `e4d29d16` accepts either recovery path while
checking the expected counts and post-restart filenames. The initial code gates passed separately from the
architecture link gate: the private source copy omits referenced results archives,
so that link gate fails and is not reported as passing. A later concurrent
rerun failed the unrelated one-second/QD256 benchmark test because some workers
were not scheduled before its deadline; record a quiet rerun before claiming the
latest complete code gate passes. A further rerun encountered a transient
`test_client_processes.py` process-identification race; the lifecycle, fold and
writeback groups subsequently passed unchanged. Final complete code-gate rerun passed; evidence is listed in the ledger.

Normal Xorinox deployment uses files/direct-I/O/TCP and retains existing stores.
The initial retained-namespace build returned EFS_ERR_IO after about 112 seconds;
ordinary services/mounts started, but query publication was not accepted. A
private stopped-state metadata clone reproduces this refusal. Inode 4113 is a
directory with parent 9876 and nlink=2. A live `level_2` entry points to it in
shard 0; parent 9876 is LOCAL and its authoritative shard is 1684. Authoritative
`efs_meta_apply_lookup(9876, "level_2")` returns NOT_FOUND. The catalog therefore
has no authoritative incoming edge and refuses publication. Both the previous
RAM builder and the disk builder have this refusal rule; the prior RAM build
hit its memory cap before reaching this check. A focused wrong-shard-directory
regression preserves the refusal in both backends. Existing metadata is not
relocated or deleted by this cache task. Repair/provenance investigation is
needed before the normal-cluster query gate can pass.
Client `.efs` commands remain mount opt-in (`EFS_COMMANDS=1`); ordinary gateway
and test-client mounts remain opted out. Final deployment runs identical `9eb3dd51` binaries on all three servers,
confirmed against `/proc/<pid>/exe`; direct-I/O/TCP and runtime user `efs` were
preserved. Servers, gateway and normal test-client mounts were restored. The dedicated
opt-in query mount returned EIO because the retained namespace has the above
placement inconsistency. Private valid-namespace queries pass; normal-cluster
query acceptance does not. Runtime and diagnostic evidence is retained in
[the ledger](../../results/measure/20261009-w92-query-disk/README.md).

## Remaining W92 obligations

Incremental derived-stat maintenance, explicit cache disk quotas/reserves,
resumable partial builds, larger measured RSS/latency/freshness gates, concurrent
mutation/export and publisher-loss evidence remain open. No synthetic 2^32-entry
gate has run. Global counts/offsets are 64-bit; each raw partition is limited to
UINT32_MAX records and value records to 16384 bytes, with explicit refusal.
Manifest native ABI changes require rebuilding; this is not a portable format.

[W92 scope](../backlog/work-items.md#w92) · [status index](README.md) ·
[W98 all-node query discussion](all-node-metadata-queries-20261009.md).
