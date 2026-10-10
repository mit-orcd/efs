# W92 closure work — acceptance ledger

Status: in progress, devel-find-stats, exclusive Xorinox. Closure is not claimed.
The user explicitly requested work until W92 is closed. The existing 2^32 synthetic
index and staged real-filesystem requirements remain binding; the earlier three
profiling rounds do not satisfy them.

| Gate | Current state |
|---|---|
| Immutable disk catalog, incremental exact aggregates, bounded builder scratch | implemented, differential/live/cold gates pass in previous checkpoint |
| Aggregate retained-cache admission | implemented; focused and complete native suites pass; live deployment gate pending |
| Durable changed-key watermarks and bounded catch-up/recovery | durable partition revisions and restart catch-up implemented in private build; focused gates pass; bounded discovery, live and bounded-discovery gates pending |
| Explicit cardinality/offset/budget audit | revision saturation and complete source-allocation budgeting implemented; broader audit in progress |
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

The root scan is proportional to generation file count (at most five partition
files per populated shard per generation), not file cardinality; costs under
long-lived exports still need measurement. Aggregate admission alone does not
finish durable incremental recovery or the scale gates.

[W92](../backlog/work-items.md#w92) · [status index](README.md).

## Source allocation and revision audit

Dirty metadata revision counters now saturate and both live and pinned revision
APIs return `EFS_ERR_OVERFLOW` at exhaustion. An older finite snapshot remains
valid; authoritative KV writes/reads are not poisoned by a derived hint limit.
The query worker records hint-read failures rather than silently retaining a
zero refresh error. Exhaustion is a refusal, not a comparison that can wrap and
misclassify changed metadata as unchanged.

Source snapshot admission now charges its fixed object, copied memtable entries
and both segment/pin arrays, with checked segment-count addition. The existing
64 MiB host source budget remains. `efs_kv_lsm_snapshot_bytes` exposes this retained
footprint; borrowed storage-engine segment objects and temporary per-reader scan
iterators remain separate audit items. Tests cover exact admission, one byte below
it, impossible segment counts, finite retained views and counter exhaustion.
KV durability, incremental differential and complete native gates pass.
The deployment/live gates for this next logical fix are pending.

## First native-format scale pilots

The bounded generator in `tests/bench_query_disk_scale.c` writes actual production
KV and disk-catalog records, distributed over 4096 partitions. It enumerates every
exported file and checks its name, parent, inode, generation and per-partition
sequence against an independent streaming oracle; it checks every directory's
aggregate and samples folded inode attributes. Both complete exports, unchanged
refreshes and close/reopen checkpoint restores pass.

| Synthetic files | Fresh build s | Unchanged refresh s | Full export verification s | Builder MiB | Whole-process peak RSS MiB |
|---:|---:|---:|---:|---:|---:|
| 100,000 | 12.855 | 0.433 | 0.133 | 3.471 | 17.539 |
| 1,000,000 | 27.169 | 0.618 | 7.119 | 3.471 | 25.262 |

Single runs, warm kernel caches, production KV compaction concurrent with building.
The source fixture uses NOSYNC; these are synthetic index correctness/memory pilots,
not real EFS capacity, power-loss durability or performance conclusions. The fixture
is an isolated 8 GiB loopback Btrfs volume with zstd compression on Xorinox; production
remains XFS/direct-I/O/TCP. Logical fixture sizes are 136,784,153 and 1,311,154,559
bytes. `du` allocated blocks are not a reliable compressed physical-space projection.
The initial launcher lacked `/usr/bin/time`, failed before fixture creation, and was
retried after installing that utility; its failure evidence is retained separately.
No 2^32-entry acceptance claim is made. Compact raw logs are in
[scale pilot evidence](../../results/measure/20261010-w92-closure/scale-pilot/).

The cardinality audit also found unchecked 64-bit tree-count additions. Fresh and
incremental disk folds now refuse aggregate overflow before changing counts or
bytes. Boundary tests prove files, directories and bytes each refuse independently
without changing the prior accumulator. Actual-source Linux IO-fault and differential
catalog tests pass. The Mac native build encounters the pre-existing Linux-only
`fopencookie` logger; Linux is the verified build platform for these gates.

Matching authoritative checkpoint watermarks already permit restore followed by
process-local hint rebasing; restore verifies on-disk digests, not the entire source
namespace. Changed-watermark recovery currently falls back to rebuilding. Durable
changed-key/catch-up watermarks and bounded discovery remain pending.

## Durable partition checkpoints and cold incremental catch-up

Private implementation now maintains 4096 saturating partition change counters
independently of the process-local scheduling hints. Each applied metadata-family
write advances its partition counter, including imports, transaction keys and
writes while command serving is disabled. They are captured atomically with the
source snapshot. A flush checkpoints the counters with the exact source MANIFEST;
WAL replay recovers later changes. Compaction carries only flushed counters, never
counters for a live WAL tail. Interrupted WAL reset can conservatively replay and
advance a counter twice; this causes extra rebuilding, never false reuse.

`QUERY-REVISIONS` is an optional 32 KiB derived sidecar in the KV directory. It
contains a random 128-bit lineage, counters, the BLAKE3 digest of the corresponding
MANIFEST and a self-digest. Its durable replacement precedes the authoritative
MANIFEST rename. A crash in between, missing/corrupt sidecar or failed optional
write makes the pair invalid and gives the next boot a different lineage. Cache
reuse requires matching lineage and finite counters. Failure to create/read derived
state does not fail authoritative KV writes or prevent opening metadata; unavailable
lineage refuses disk-cache publication. This is a local persisted history hint,
not a replacement for the host's Raft/transaction/import authority seal.

Disk catalog manifest version 3 records this lineage. Version 2 caches are derived
state and rebuild on upgrade; authoritative KV/WAL formats remain unchanged. Exact
restore now compares persisted source revisions rather than rebasing old hints.
A stale completed checkpoint can instead feed the bounded disk builder, which
reuses unchanged raw partitions and refreshes changed ones; its stale contents
are never directly published. Host authority/import validation remains required
before checkpointing and publication. Candidate cancellation leaves the prior
checkpoint and retained output intact.

Focused Linux regressions pass flush/WAL recovery, disabled-command writes,
finite retained snapshots, cancelled catch-up, rename/export/aggregate comparison
with the RAM reference, and missing/corrupt sidecar invalidation. Changed restart
catch-up rebuilds fewer than 4096 partitions; a new lineage rebuilds all of them.
The complete native suite and optional-sidecar-write-failure gate pass. Production
has not yet received this implementation. Reverse-alias and lane-dependency
discovery still scan immutable index streams; removing global discovery remains
part of the bounded catch-up work rather than claiming this checkpoint closes W92.

## Bounded dependency discovery checkpoint

Each persisted partition now carries a fixed 64-bit conservative lane-dependency
filter. Unchanged partitions with no possible dependency on a changed source
partition reuse their derived file without walking its inode rows. Hash collisions
only add work; the exact per-inode dependency check still decides folding. Revised
partitions rebuild their filter while streaming rows. This removes global inode
visits for unchanged refreshes and ordinary lane-free tiny-file updates without
retaining a namespace-sized reverse map in RAM. Disk manifest version 4 includes
this field; earlier derived versions rebuild safely.

Linux 12,000-file differential/retained-view tests, IO-fault tests and the rich
catalog oracle (cross-shard lane-only changes, hardlinks, subtree mutations and
transaction folding) pass. A detached ten-million-file native-format scale case
is running in the same isolated fixture. Alias discovery and affected-parent edge
folding still need their global stream scans removed; this checkpoint is partial
bounded-discovery progress, not W92 closure.

## Indexed alias discovery and affected-parent folds

Disk manifest version 5 adds a 24-byte reverse-alias record per physical dentry
(child inode, parent inode, raw ordinal), sorted per parent-shard by child inode.
Names and aliases share one bounded external merge-sort implementation. Unchanged
partitions retain both indexes by immutable links; new alias files participate in
fsync, content-digest, cold-restore and quota gates. Alias sorting reserves two
24-byte copies per dentry conservatively, in addition to name-sort work.

At most 4096 binary seeks per changed inode replace the global alias stream scan.
All matching hardlink parents are invalidated. Affected directory aggregates read
only their dentry prefixes: one lane for LOCAL, at most 64 for HASHED/SPLITTING,
using the same authoritative-placement test as fresh folds. Changed raw partitions
still stream and compare their own records; exceeding the 1024 change/ancestor
bound triggers an explicit bounded-memory full background rebuild. No request
access builds a catalog. Work is bounded in memory, not promised constant time.

A fixed 64-entry decoded inode cache removes repeated binary probes for hot parent
directories. Derived writes invalidate overlapping entries before the syscall,
including failed/partial writes; an epoch prevents inserting a stale concurrent
read. Epoch saturation disables new insertions. Immutable name pages use their
canonical persisted records plus the placement check, avoiding an identical raw
point lookup per result. This does not change permissions or snapshot lifetime.

The rich RAM/full-rebuild oracle, 12,000-file external-sort/retained-view gate and
IO/fsync/cancellation/overflow gates pass. Added tests prove unchanged reuse performs
zero inode/dentry discovery reads, cache hits avoid reads, and derived writes and
epoch exhaustion preserve fresh results. A 4,000-file smoke test checks every file
before and after one inode-only mutation and verifies the old snapshot's totals;
mutation rebuilds one raw partition and refolds two inodes. Accounted builder
memory is 3,864,480 bytes, below 4 MiB. The complete native suite passes; live gates remain pending for
this checkpoint.

The preceding version-4 ten-million-file run completed successfully: fresh build
259.494 s, unchanged refresh 0.475 s, full export oracle 166.327 s, builder
3,672,472 bytes and whole-process maximum RSS 42,424 KiB. Every exported file
passed the independent sequence/identity oracle; close/reopen restore also passed.
Its source NOSYNC, warm/shared VM environment and isolated compressed fixture are
unchanged limitations. See [raw ten-million evidence](../../results/measure/20261010-w92-closure/scale-10m-v4/).
These are observed pilots, not extrapolated 2^32 acceptance. A version-5 repeat
with an inode-only mutation and retained/full export oracles is next.

## Cardinality and budget audit

| Quantity / owner | Representation and bound | Consequence |
|---|---|---|
| Global disk-catalog inode count, rollup counts/bytes, raw offsets | uint64; tree additions and positional IO checked | 2^32 distributed files fit arithmetic; overflow refuses a candidate |
| Raw records per partition | uint64 in manifest; builder/restore reject above UINT32_MAX | at most 4,294,967,295 records across the six raw families per partition, not that many files plus unlimited extra records |
| Global name cursor | uint64: shard in upper bits, 32-bit row; terminal `4096 << 32` | 4096 partitions, bounded resume position; no 32-bit global file count |
| Directory metadata nents/nlink | uint32 | individual directory limits still apply; distributed layout does not make these fields 64-bit |
| Native disk node/name/alias records | versioned local ABI; file digests and expected lengths checked | v5 adds aliases; earlier derived formats rebuild, no authoritative-format upgrade |
| Positional disk IO | checked against INT64_MAX before pread/pwrite | never wraps a signed filesystem offset |
| RAM reference backend | uint32 node/edge ordinals and configured catalog budget | retained for differential tests; not the billions-of-files backend |
| Work counters | uint32 refold count saturates; rebuilt partitions at most 4096 | diagnostic work counters do not define namespace size |
| Incremental changes/ancestors | 1024 entries each, sorted fixed arrays | overflow triggers a full bounded-memory background rebuild, not silent omission |
| Lane dependencies / decoded inode cache | 64-bit filter per partition; 64 decoded nodes | fixed resident allocations; filter collisions only add work |
| Source snapshot admission | 64 MiB host limit, complete retained source allocation charged | segment objects/indexes borrowed from authoritative KV are outside the cache snapshot allocation and included in process RSS measurements |
| Source revision checkpoints | 4096 uint64 counters, fixed sidecar and 128-bit lineage | saturation refuses comparison; invalid history forces derived rebuilding |
| Query page | uint32 visit budget, at most 1,000,000 visits; requested results at most 10,000 | bounded per call; full export resumes uint64 cursor across pages |
| Wire output / server reply allocation | output at most 16 MiB; aggregate reply admission 64 MiB | larger namespaces stream; no complete result buffered on server |
| Authorization scratch / paged export lease | 8192 fixed authorization entries; one server page session, 60 s idle / configurable finite total lease | concurrent exporters may receive BUSY; expired sessions refuse stale continuation |
| Python display/export | display at most 10,000 entries; file export consumes pages of 1000 | ordering a bounded display is not sorting every file in RAM |
| Cache logical disk quotas | default 16 GiB generation, 32 GiB root; each configurable up to 16 TiB | scale requires explicit provision/configuration; shared links charged conservatively |

The native cache working estimate includes fixed record/sort buffers and candidate
work, rather than allocating an array per namespace inode. Linux v5 smoke accounts
3,864,480 bytes under the 4 MiB exercised builder budget. These arithmetic checks
and pilots do not substitute for the required 2^32 actual-entry synthetic gate,
concurrent load/freshness measurements or staged real-filesystem acceptance.

## Large-export lease duration

The cardinality audit found the fixed one-hour active export lease could prevent
completion of a billions-of-files stream. `EFS_QUERY_EXPORT_MAX_MS` now configures
its finite total lifetime, allowed 60000–604800000 ms (one minute to seven days),
default still 3600000 ms. Invalid/overflowing values report a configuration warning
and retain the default. Idle expiry remains sixty seconds, publication shutdown
still clears the lease, only one paged snapshot is retained per host, and every
page repeats authorization. Longer retention is charged by the aggregate cache
budget; no namespace-sized memory allocation or protocol-format change is added.
Boundary/default/invalid-value tests verify an active 24-hour lease survives the
old one-hour point while idle and hard expiry still refuse continuation. Integrated
Linux compilation and focused lease tests pass; live expiry/resource gates remain pending.
