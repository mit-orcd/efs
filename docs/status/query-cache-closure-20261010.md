# W92 closure work — acceptance ledger

Status: in progress, devel-find-stats, exclusive Xorinox. Closure is not claimed.
The user explicitly requested work until W92 is closed. The existing 2^32 synthetic
index and staged real-filesystem requirements remain binding; the earlier three
profiling rounds do not satisfy them.

| Gate | Current state |
|---|---|
| Immutable disk catalog, incremental exact aggregates, bounded builder scratch | implemented, differential/live/cold gates pass in previous checkpoint |
| Separate build-phase diagnostics | implemented in cdc13b88; full native, live two-mount and Mac/Linux NFS gates pass |
| Aggregate retained-cache admission | implemented; native and live QUOTA/write/cold-recovery oracles pass |
| Durable changed-key watermarks and bounded catch-up/recovery | durable partition revisions, restart catch-up and bounded discovery implemented; focused/full native and live cold/disable-re-enable/publisher-loss/interrupted-cache gates pass |
| Explicit cardinality/offset/budget audit | revision saturation, complete source-allocation budgeting and representation audit complete; named deployment/live validation passes; actual 2^32 validation remains open |
| Increasing-cardinality RSS, cold-cache, concurrent export/mutation and freshness | 10m complete kernel-cold and concurrent old/new export oracles pass with fixed builder memory; named live contention/freshness passes; full actual scale pending |
| Synthetic partitioned index of at least 2^32 entries | pending; sparse offset probes alone cannot satisfy this |
| Replicated persistent feature policy | native, live, restart, quota, lost-reply and fail-closed gates pass |
| Staged real-filesystem and fault/platform gates | named native and Linux NFSv4.0 pass; actual 400 GiB write/cold/hash/export pending; virtual-export fix passes native suite plus fresh Mac/Linux NFS; named 1434aa83 Mac/Linux/native full repeats and 96 Mac queries pass |

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
[closure evidence](../../results/measure/20261010-w92-closure).

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
[scale pilot evidence](../../results/measure/20261010-w92-closure/scale-pilot).

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
unchanged limitations. See [raw ten-million evidence](../../results/measure/20261010-w92-closure/scale-10m-v4).
These are observed pilots, not extrapolated 2^32 acceptance. The version-5 repeat also completed with every file checked before and after
an inode-only mutation. Fresh build 291.206 s; unchanged refresh 0.617 s;
full export verification 7.196 s; mutation refresh 2.932 s (one partition,
2443 inode refolds), followed by a second full export verification in 7.446 s.
The old snapshot retained its original exact totals. Accounted builder memory
3,864,480 bytes; whole-process peak RSS 41,000 KiB. These are single shared-VM,
warm-kernel measurements, not a controlled throughput comparison or the 2^32 gate.
See [version-5 evidence](../../results/measure/20261010-w92-closure/scale-10m-v5).

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
| Query-source scan scratch | at most 256 relevant segments, 128 KiB per iterator block, no sequential readahead; below 33 MiB total | prefix filtering precedes bounded allocation; excessive fan-in/block size refuses BUSY |
| Source snapshot admission | 64 MiB host limit, complete retained source allocation charged | segment objects/indexes borrowed from authoritative KV are outside the cache snapshot allocation and included in process RSS measurements |
| Source revision checkpoints | 4096 uint64 counters, fixed sidecar and 128-bit lineage | saturation refuses comparison; invalid history forces derived rebuilding |
| Query page | uint32 visit budget: v1 at most 1,000,000, v4 paged at most 100,000; requested results at most 10,000 | bounded per call; full export resumes uint64 cursor across pages |
| Wire output / server reply allocation | output at most 16 MiB; aggregate reply admission 64 MiB | larger namespaces stream; no complete result buffered on server |
| Authorization scratch / paged export lease | 8192 fixed authorization entries; one server page session, 60 s idle / configurable finite total lease | concurrent exporters may receive BUSY; expired sessions refuse stale continuation |
| Python display/export | display at most 10,000 entries; file export consumes pages of 1000 | ordering a bounded display is not sorting every file in RAM |
| Authoritative source segment indexes/descriptors | borrowed from KV, not charged as query allocations; process/VM provisioning measured separately | whole-process RSS includes them; the synthetic process has finite NOFILE 524288, query read descriptors remain 32 |
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

## Sequential lookup and actual scale provisioning

Full inode folds reuse the already validated immutable raw inode record through
a point-read KV overlay. Other keys and scans still use the normal snapshot disk
backend; getattr retains its lane, transaction and stability checks. The fixed
64-entry decoded cache also supplies a same-partition predecessor hint. The
adjacent row must match the requested inode exactly; gaps and reverse order use
the original binary lookup. IO errors remain failures. Differential, retained
view and fault tests pass, including gaps, reverse order and write invalidation.
A ten-million-entry repeat is running before the actual billions-entry gate.

For the actual scale gate, a fresh 2 TiB sparse raw disk was attached live-only to
xefs1 as vdc, serial W92SCALE20261010. Its uniquely identified blank device was
formatted Btrfs and mounted at `/data1/efs/bench/w92-scale-2p32-20261010`, with
forced zstd compression and noatime. Existing vda/vdb disks and persistent VM
configuration were unchanged. Host backing is capped at 2 TiB with approximately
3.2 TiB initially free; the fixture reserves 100 GiB physical free space and
explicitly configures larger logical cache limits. This is synthetic production
record validation, not a claim of real EFS/Raft payload capacity. No billion-entry
result is claimed until actual population and independent streaming oracles pass.


## Actual-entry gate dispatched; further pilot recovery

The sequential ten-million repeat completed: fresh build 231.856 s, unchanged
refresh 0.633 s, complete export oracle 9.526 s. One inode mutation rebuilt one
partition/refolded 2443 inodes in 2.430 s; the retained original totals and a second
full export (9.206 s) pass. These are single shared-VM observations on a different
isolated volume, not controlled engine throughput. Builder allocation remains
3,864,480 bytes. The complete native suite also passes.

The test volumes were normally unmounted/remounted outside `/data1` because the
cluster's dedicated-filesystem preflight correctly rejects child mounts. The
big volume is now `/mnt/w92-scale-big-20261010`; the earlier 8 GiB pilot volume is
`/mnt/w92-scale-pilot-20261010`. A mount-move attempt was refused by shared mount
propagation; no production disk changed. Xorinox had rebooted and lost `/tmp/efs`,
so checkouts were restored without wiping retained state; deployment uses the
correctly identified `648495c6` binaries, direct IO/TCP.

The actual 4,294,967,296-file generator is running in detached screen
`w92-2p32-20261010`, nice 19 / idle IO priority, with explicit 8 TiB generation,
16 TiB aggregate logical limits and 100 GiB physical reserve. It writes every
production-format record; no sparse offset simulation or smaller recycled index
is counted. Terminal markers, two full independent streams, restart and retained
mutation oracles must pass before accepting it. It has not finished.

A separate `--recover` harness mode makes bounded advisory DONTNEED requests for
regular files in the private fixture, never following symlinks or dropping global
host caches. It opens the durable KV/checkpoint, catches up the prior one-inode
change, and verifies the complete export. On the version-5 ten-million fixture:
20785 regular files hinted, catch-up 11.266 s, one partition rebuilt, complete
stream verified in 6.520 s, rc=0. Eviction is advisory; this complements rather
than proves universal physical-cold behavior. The live load gate now scopes its
readers to its private tree, records per-create latency/convergence, and checks
every acknowledged payload independently through the peer mount.


## Live command and loaded-publication checkpoint

The `648495c6` deployment passes command-source, raw filename, peer catalog,
lookup-only traversal, fresh descendant authorization, opened-descriptor lifetime,
real `.efs` collision and bounded identity/FORGET recovery gates. Paged exports
pass display bounds, full NUL/NDJSON streams, retained generation during mutation,
second-session BUSY admission, explicit close/stale and permission revocation.
The first raw-RPC attempt targeted loopback while efsd binds its configured VM IP;
the harness now accepts `EFS_QUERY_TEST_HOST` and the actual endpoint retry passes.
This was test endpoint configuration, not a daemon failure.

Eight writers and two query readers overlapped on a private tree. Both readers
observed multiple generations and bounded BUSY refusals without partial success.
2131 acknowledged 256-byte payloads were independently verified through the peer
mount; exact counts/545536 bytes converged 6.775 s after the writer phase. Create
latency p50/p95/p99 was 44.33/97.91/189.52 ms while other live suites and the
low-priority scale generator shared the VMs. This is a functional contention and
truthful freshness measurement, not an uncontended throughput baseline.


## Disabled tracking, cold payloads and aggregate refusal

All daemons were restarted with `EFS_COMMANDS_SERVER=0`: capabilities were zero
and queries refused without successful bytes. A normal client fsynced 500 fresh
4096-byte payloads. After re-enable, every payload passed its independent oracle;
exact 500-file/2048000-byte stats returned with refresh_error=0. Startup logs show
`query-catchup: rc=0`, only seven rebuilt partitions and 755 inode refolds in
4619 ms. This demonstrates durable tracking while the query worker is disabled,
not a full 4096-partition rediscovery. All 26 independently derived persistence
cases also survive this cold restart. Core POSIX is 212/212 on each client,
concurrent 64/64; extras 4 pass and the existing mmap limitation is skipped.

The aggregate-limit phase configured 1 MiB on every publisher. It returned
QUOTA (-11), empty failure output, and retained advertised capabilities. The
first harness incorrectly required the same error again in refresh_error;
request status already carries it, so the corrected gate accepts either error
location while forbidding successful bytes. The retry fsynced and independently
verified another 500 payloads (2048000 bytes) despite cache refusal. Normal
limits were restored after the phase. All 500 payloads pass again after restart;
exact 500-file/2048000-byte stats return with refresh_error=0.

## Source-engine descriptor boundary in the actual-entry gate

The first 2^32-target run stopped during source population at 75.854 s, before
any disk-catalog acceptance. Its inherited soft NOFILE limit was 1024 and its
source directory had 1020 files; peak process RSS was 142436 KiB. The failed
fixture/logs are retained and not counted as a populated billions-entry index.
The printed `files` value in failed population records is the requested target,
not proof of actual records. The recovery diagnostic on the already failed source
did not directly reproduce EMFILE, so that trace alone does not prove the cause.

A fresh retry started at 131072 descriptors and remained active past 4101 open
source descriptors, beyond the original limit. Its finite process limit was
subsequently raised to the existing hard limit, 524288, to provision the entire
source segment set; the directory/screen label retains `fd131072` to preserve
launch history. The query disk read cache is still 32 descriptors. Authoritative
KV segment descriptors and sparse indexes are separate source-engine costs,
included in process measurements rather than hidden as query allocations.
The private generator uses nice 19, idle IO and oom_score_adj=700, preferring it
over production daemons under memory pressure. Neither failed/progress runs nor
raising source resource limits satisfy the required actual-entry acceptance.


## Live publisher loss and interrupted derived recovery

A paged export was pinned, then fsynced mutation traffic started. The daemon was
SIGKILLed only after observing a non-current generation without its completed
manifest (`view-eqSC9N`). Its incomplete directory still existed before restart.
The two surviving replicas served all 500 earlier payloads through a fresh client;
the mutation writer acknowledged all 2100 files without error while the publisher
was absent. After restart, the incomplete orphan was removed. The fresh client
verified all 2100 deterministic payloads and exact 8601600-byte totals, with no
refresh error. The old publisher token returned STALE (-14), no bytes, and the
new snapshot has a different boot identity. These are W92 derived-cache/publisher
recovery gates; they do not close unrelated W55 NFS publication or W66 routing.

## Query-source scan scratch gap and fix

The final allocation audit found private scans allocated merge slots for every
source segment before prefix filtering. Their iterator blocks also lacked a
query-specific bound. This allocation belongs to the query builder's source IO,
so the catalog-owned 3.69 MiB figure alone did not fully describe its working set.
Private scans now prefilter immutable ranges before allocation and admit at most
256 relevant segments, with at most 128 KiB per block and no sequential readahead.
The iterator/merge scratch is below 33 MiB, separately bounded from the 64 MiB
retained source snapshot and catalog-owned buffers. Excess fan-in/oversized blocks
return BUSY and cannot poison authoritative live reads/writes. Live merge scans,
compaction and authoritative formats retain their existing behavior/limits.

Boundary tests cover exact/one-over fan-in, irrelevant segments, oversized block
refusal before callback output, and successful authoritative reads afterward.
KV, disk differential, retained-view and IO/fault gates pass. The complete native
suite passes after the final private-readahead guard; deployment and actual
scale validation of this latest fix remain pending. The source engine's borrowed
sparse indexes/descriptors remain separate provisioned costs, included in measured
whole-process RSS. No constant whole-process RSS claim hides those inputs.

Private range exclusion also uses a fixed 128 KiB tail buffer when a segment's
last key is not already cached. It does not promote source-wide tail caches or
allocate a full block merely to decide whether a segment is irrelevant. Checked
length arithmetic rejects corrupt tails; oversized unknown tails refuse the
private scan. A complete-source unit verifies no heap allocation, no cached-tail
mutation and no readahead on this path.

Incremental own-partition updates now reuse unchanged canonical inode stats when
their complete stat inputs are row-only: LOCAL directories, or regular files
with no active lanes and no APPEND_CUR records in either partition view. Lane
and auxiliary cases retain conservative folding. A focused differential gate
checks exactly one refold for a single row edit, then a stable-row open append
cursor updates size correctly. The initial new test incorrectly used a
directory-only comparison helper on a regular file; it was stopped and corrected
to compare inode attributes directly. The corrected focused regression passes;
the complete suite passes with that corrected test.

The second actual-entry attempt was intentionally interrupted before completion
to provision its source engine: the 3 GiB VM was using 2385 MiB swap, with the
generator at about 559 MiB resident/2.5 GiB virtual memory. Its terminal 143
marker records that interruption, not an acceptance failure. The partial fixture
is retained. A new latest-code run will use a temporarily enlarged source VM;
this does not change the fixed query-cache budgets or claim constant whole-process
memory. The full 2^32 gate remains pending.

The complete suite then exposed a pre-existing white-box overflow probe race:
it published UINT32_MAX segment counts into the live store between locks, and
the background compactor crashed in kv_seg_first_key while the snapshot test
waited for the mutex. The preserved coredump identifies that path. The overflow
probe now uses an isolated structure with its own mutex; the live segment counts
are never changed. The fan-in fixture also waits for source quiescence before
borrowing its segment. The corrected complete native suite passes, including the whole-source tail,
KV, differential catalog, retained-view, IO/fault and architecture gates.
Raw evidence: [complete gate](../../results/measure/20261010-w92-closure/source-tail-native-passed.log).

## Latest bounded-source ten-million-entry repeat

`d219e2bd` production source passes a fresh ten-million-entry oracle with bounded
private scan/tail buffers and row-only stat reuse. Fresh build 233.101 s, full
export verification 15.438 s; unchanged reuse 0.655 s, cold restore exact; retained
mutation 4.925 s with one rebuilt partition and **one refolded inode**. The second
full export also checks every path/identity/sequence. Accounted catalog scratch
remains 3864480 bytes; source snapshot charges 112752/112859 bytes before/after
mutation. Wall time varies on shared VMs, so the reduced fold count is the
substantiated optimization, not a measured wall-time speedup. Time/allocation
evidence is [retained here](../../results/measure/20261010-w92-closure/scale-10m-bounded-v3).

The source-scale VM has been gracefully drained for a temporary increase from
3 GiB to 24 GiB RAM. Its inactive original XML is preserved on the host at
`/data1/efs/bench/w92-xefs1-original-20261010.xml`; existing authoritative and
private test disks are retained without formatting. Deployment/recovery and
the fresh actual 2^32-entry run follow this capacity provisioning.

## Deployed load and NFS platform findings

The retained cluster is restored on `d219e2bd`, direct-I/O/TCP and normal budgets.
Normal client mounts remain opted out; dedicated diagnostic mounts are opted in.
The fresh actual 2^32-entry run is `w92-2p32-bounded-24g-v4`, on the retained
private disk with 24 GiB RAM, finite 524288 NOFILE, no core dumps and low priority.
Population logs distinguish completed files from the requested target. At the
first recorded snapshot, 226492416 file/name pairs were populated; the index/full
export gate remains incomplete. This is not yet 2^32 acceptance.

Deployed command permission/collision/immutable-descriptor gates and full
paged-export/retained-generation/fresh-authorization gates pass. Concurrent load
acknowledges and independently hashes 1133 payloads; exact totals converge in
5.120 s. Create p50/p95/p99: 63.969 / 246.046 / 534.140 ms. Two readers sample
215/201 requests including BUSY; query p50/p95/p99: 9.317 / 296.393 / 414.472 ms
and 10.109 / 330.106 / 544.373 ms. BUSY 168/157 is reported, not filtered away.
These shared-VM measurements overlap the low-priority synthetic generator.

The Mac's standard SSH NFS tunnel was absent; it has been restored. The retained
Mac mount then returned ESTALE after gateway recreation. A local privileged
remount is pending from the user because sudo requires their password.

A fresh Linux NFS mount negotiated 4.2. Its intermittent directory-open EINVAL
is traced on the wire to GET_DIR_DELEGATION returning NFS4ERR_INVAL; corresponding
FUSE responses succeed. Explicit 4.0 matches the macOS helper and avoids that
negotiation. Version 4.2 is not certified by this gate; protocol evidence is
[retained here](../../results/measure/20261010-w92-closure/nfs42-delegation-status.tsv).

The explicit 4.0 gate found a W92 freshness defect: repeated stats calls kept
receiving generation 16 (99 files) for over 120 seconds, while a native fresh
query served generation 17 with the complete 140-file/561155-byte oracle. A
fixed `v1.stats` name allowed NFS to retain the old virtual result. Each simple
command now uses a fresh 128-bit nonce component, validated by the synthetic
path parser. Legacy fixed paths remain accepted; server query semantics and
immutable acquired-result bytes remain unchanged. Native codec/command tests
pass on Mac and Linux (28 tests). The identified gateway candidate passes explicit NFSv4.0 source, lookup-only,
help, exact stats, JSON/NUL, fresh-command convergence after mutation, unchanged
retained-result bytes, paged client-path export and all 141 payload hashes
(561174 bytes). Eighty consecutive stats invocations also pass in 2.284 s,
exceeding the 64-slot identity pool without exhaustion. Raw candidate, pre-fix
reproduction and sequential logs are retained in the closure evidence directory.
The export maps every NFS caller to UID/GID 0; these gates certify that actual
policy and do not claim per-client UID authorization. Mac native execution,
persistent remote export policy and the large scale gates remain open.

## Remaining real-payload capacity fixture plan

The historical 400 GiB actual-payload requirement is still open; synthetic files
are not a substitute. Production quotas total 300 GiB raw and cannot hold that
2+1 payload safely. Provision three fresh capped 270 GiB XFS test volumes on
the same Xorinox VMs and run a separate direct-I/O/TCP/files namespace on port
18432, with 250 GiB quota per node and a distinct client mount. Existing port
17432 services and /data1 are retained. About 600 GiB raw fragments plus metadata
fit those private quotas; host physical capacity must be rechecked before launch.
The generator will stream bounded deterministic nonzero payloads, fsync every
acknowledged file, and independently verify all data and exact query totals
through a fresh client after a cold restart. Record namespace cardinality and
measured physical/logical allocation separately from the actual synthetic gate.
Do not format any existing filesystem or expand production quotas. The private
units/disks must be addressed explicitly; shared helpers that discover all efsd
processes cannot distinguish both namespaces. This is a proposed fixture, not
a completed gate or an end-to-end billions-of-file capacity claim.

The three private 270 GiB raw disks have now been serial-verified, formatted
without force, and mounted separately. The port-18432 namespace uses 250 GiB
per-node quotas and explicit units `w92-capacity-efsd`. Client 115 runs
`w92-capacity-400g-20261010` detached. The 80-file/20 MiB smoke write and
independent full hash/stat verification pass; this is a harness check only.
At the first compact progress check, 23936 of 1638400 real files were acknowledged.
Full write, cold recovery, every-payload and complete-export gates remain pending.

The first capacity writer stopped at 09:33:38 UTC with exit 1 after 124983
acknowledged files (32763543552 payload bytes), before the 400 GiB gate. The
client reported CREATE PROTO (-7), not ENOSPC; all three private daemons remained
active with about 248 GiB free each and no PUT/disk-write errors. The original
[failed-run checkpoints](../../results/measure/20261010-w92-closure/w92-capacity-write-failed/write.log)
are retained. A later diagnostic exclusive CREATE for the absent failing name
succeeded and left an unacknowledged zero-byte tail. The explicit resume harness
rehashes every acknowledged payload before proceeding and only rewrites the
unacknowledged tail, recording each repair. Its full-acknowledgement 80-file
smoke passes. This recovery does not convert the failed run into a passed gate.

Review found a concrete retained-CREATE retry defect: an initial probe can return
BUSY before parent ReadIndex determines the route; the client incorrectly
converted that response to PROTO. The proposed fix preserves BUSY and the exact
operation identity, and admits/submits nothing until a route is received. The
actual extracted CREATE regression now exercises three such probes and passes
on Mac; candidate Linux/live and native repetition follow. The original log
without per-probe tracing does not prove which validation branch fired.

The unrouted-BUSY fix passes the actual extracted retained-CREATE and FUSE
admission deadline regressions on Mac, the retained-CREATE regression on Linux,
and the [unchanged complete native suite](../../results/measure/20261010-w92-closure/w92-unrouted-busy-native-passed.log).
The initial resume was intentionally stopped with SIGTERM/143 during its read-only
acknowledgement rehash to install the fixed private client. The new detached
`w92-capacity-busy-fixed-20261010` resumes from the preserved original write
checkpoints with client diagnostics captured to a persistent file. Actual full
payload/cold/export acceptance remains pending.

## Named rollout — 1a25d20a193b

The committed policy and unrouted-CREATE fixes pass the full native suite;
named `1a25d20a193b` binaries/source are deployed on all six Xorinox guests.
All three binary SHA-256 digests agree across the six guests. Only normal port
17432 services/mounts were drained and cold restarted; no data was formatted.
Separate capacity/policy services and the actual-entry generator stayed running.
The normal clients on 115/116 remain `.efs` opt-out; gateway and the named
diagnostic mount remain opted in for platform acceptance. All node policy GETs
return default version 0/mask 3.

Native commands and one freshly mounted, explicitly NFSv4.0 Linux client pass
all 141 payload hashes/561174 bytes, exact stats, fresh command convergence,
retained immutable bytes and complete paged client-path export. Two earlier
NFS invocations passed but found stacked mounts; they are not the final mount
gate. The admin user cannot traverse `/home/efs`, so mountpoint checks for cleanup
now run under sudo before a single fresh mount. Earlier acknowledged disabled
and quota fixtures also recover all 1000 payloads/4096000 bytes after rollout.
Mac execution remains pending its user-privileged stale-mount refresh.

[Deployment/digests](../../results/measure/20261010-w92-closure/w92-1a25d20a-deployment.json),
[native commands](../../results/measure/20261010-w92-closure/w92-1a25d20a-native-commands.log),
[fresh NFSv4.0](../../results/measure/20261010-w92-closure/w92-1a25d20a-nfs40-fresh-single.log),
[retained payloads](../../results/measure/20261010-w92-closure/w92-1a25d20a-retained-payloads.log).

The fixed capacity client has rehashed the original acknowledged prefix and
resumed new writes. Its recorded zero-byte unacknowledged diagnostic tail is
rewritten, with original failed-run evidence retained. No 400 GiB or actual
2^32-entry completion gate is claimed. The latest actual-entry population
checkpoint is 2345664512 entries; whole generator/source-process RSS is
4305072 KiB, distinct from the fixed query-builder/scratch budgets.

The named [loaded writer/query repeat](../../results/measure/20261010-w92-closure/w92-1a25d20a-loaded.log)
also passes with eight writers/two concurrent readers: 1126 independently
verified payloads (288256 bytes), changing publication generations and exact
post-close stats in 10.573 s. Create p50/p95/p99 are 76.766/217.434/418.663 ms.
The actual-entry generator and separate real-payload writer overlap this test;
this shared-load measurement is not a controlled speedup comparison.

## Explicit kernel-cold and concurrent ten-million-entry gates

The named `1a25d20a` library passed a fresh-process recovery/export of all
10,000,000 paths after root ran `sync` and cleared guest kernel caches. Guest
Cached fell from 14,602,776 to 834,260 KiB. Recovery took 28.252 s and the complete
independent export check 6.300 s; one partition rebuilt, builder allocation was
3,864,480 bytes, and whole-process peak RSS was 29,084 KiB. Host/device caches
were not controlled; this is guest kernel/process cold recovery, not power-loss
or physical-device cold performance. The two larger fixtures continued running.

`tests/bench_query_disk_concurrent.c` retains an old generation, starts a complete
independent export, changes one source inode, and builds a replacement while
that export continues. It checks every old and new path, every directory/root
aggregate, restores the private source and checkpoints it for an independent
fresh-process recovery check. It refuses missing/non-directory fixture roots.
Compilation with `-Wall -Wextra -Werror` and a 1000-entry smoke test pass.

The first 10m run reached the default 32 GiB aggregate retained-generation quota
and correctly returned QUOTA; its old export still checked all 10m paths. That
failed attempt is preserved. With only this private fixture's disk budget raised
to 128 GiB, the repeated gate passes: 10m old and 10m new paths checked, 1,557,796
old rows emitted during replacement construction, 3,864,480 builder bytes and
225,718 bytes of retained source-owned allocations. Whole-process peak RSS was
32,736 KiB; elapsed time was 57.93 s under overlapping cluster/scale load. RAM
budgets and normal 32 GiB cache defaults are unchanged. This is a correctness and
bounded-memory gate, not a controlled speed comparison.

An additional fresh-process recovery verifies all 10m paths and exact aggregates
after the concurrent test: 29.104 s recovery, 12.798 s complete export verification,
zero rebuilt partitions, exit 0. Compact raw evidence, including the quota
failure, is in
[the cold/concurrent results](../../results/measure/20261010-w92-closure/cold-kernel-and-concurrent-10m).
The actual 2^32 run still owes its own complete build/export/mutation/recovery
oracles; these pilots do not substitute for it.

## Remaining scale handoff and service restoration

Do not restart either fixture on an observation timeout. Inspect the terminal
marker first and then confirm its process: the actual-entry generator is PID
6246 on xefs1, screen `w92-2p32-bounded-24g-v4`; the capacity writer is PID
269152 on client 115, screen `w92-capacity-busy-fixed-20261010`. PIDs are a
checkpoint, not permanent identities: verify command lines before signaling.
The actual-entry run has an 8 TiB generation budget, 16 TiB aggregate budget and
100 GiB free reserve; those are private logical limits, not preallocated storage.

After the actual-entry run records exit 0, preserve its complete log/time/markers
and check every build, cold and mutation verdict. Then run the named recovery
harness against the same 4,294,967,296-entry root in a fresh process, following
an explicit guest `sync`/kernel-cache clear, with the same large disk budgets.
Its complete streaming oracle must pass; a count in a progress line is insufficient.
The ten-million concurrent gate already passes; the completed large root can
also be exercised by the concurrent harness, retaining its original source
mutation and checking complete old/new exports. Do not open its source while
the original generator or recovery process still owns it.

After the capacity resume records exit 0 and `resume-complete.json` proves all
1,638,400 files, drain/unmount its client using `/tmp/efs/scripts/client.sh stop`.
Cold-stop all three **only** `w92-capacity-efsd.service` instances, then recreate
their explicit transient units: port 18432, preserved
`/mnt/w92-capacity-node-20261010`, quota 250G, files/direct-I/O/TCP,
`EFS_MD_RAFT_N=3`, finite `EFS_QUERY_EXPORT_MAX_MS=86400000`, respective node IDs
and addresses, and nodes 2/3 joining `192.168.122.111:18432`. Do not format,
use the normal cluster-wide stop helper, or restart port 17432 for this gate.
Start a fresh named `/tmp/efs/efs-fuse` opt-in client on 115 and run the capacity
harness in `verify` mode against `w92-real-400g-20261010`, writing evidence to a
new local directory outside EFS. This independently checks every payload hash
and length, all directory names, exact rollups and every exported query path.
A successful write or sampled read cannot replace that cold verification.

The named Mac/Linux/native platform gates and normal opt-out restoration
are complete (see the named platform verdicts below). Before closure, retire
the remaining private capacity service namespace and any diagnostic clients, and
restore xefs1's original 3 GiB memory configuration from its saved XML after
all scale processes have exited. Preserve datasets and raw evidence. Verify
normal port-17432 daemon arguments, all normal clients, NFS service and retained
namespace after restoration. No pushes, production formatting, D25 activation,
W55 incident closure or W66 consensus claim are part of this handoff.

The named 2^32 guest-kernel-cold recovery is queued in detached admin screen
`w92-2p32-kernel-recovery-20261010`. Its
[exact wrapper](../../results/measure/20261010-w92-closure/scale-recovery-handoff/w92-scale-recovery-20261010.sh)
waits for the original marker, refuses a failed/missing original process, checks
all population/build/cold/mutation verdicts, then clears guest caches and runs
the named full recovery oracle. It preserves the large private disk budgets and
sets NOFILE **inside** the `runuser` shell: the guest's `runuser` otherwise resets
the soft limit to 1024 (hard limit remains 524288). The waiting wrapper is live;
this is queued work, not completed recovery. Results use the original fixture
stem plus `-kernel-recovery.{cache-state,log,time,exit,finished}`.

The 400 GiB cold handoff is queued in a local detached screen
`w92-capacity-cold-handoff-cdc13b88` (replaces the intentionally retired
previous watchers without interrupting the writer), using this
[exact one-shot script](../../results/measure/20261010-w92-closure/capacity-recovery-handoff/run.sh).
It retries SSH observation failures without restarting the writer. Only an exit-0
marker plus the complete 1,638,400-file/400-GiB independent write verdict permits
the handoff. It preflights the named daemon hashes and all three private unit
arguments, drains the test client, stops all three private daemons, recreates
only those units with preserved storage and mounts a fresh named opt-in client.
It then queues the full verifier in remote screen
`w92-capacity-cold-verify-20261010` on 115; results are outside EFS in
`/home/efs/w92-capacity-cold-verify-20261010`. Normal port-17432 services are not
part of this script. The local screen must remain running (a sleeping/disconnected
Mac delays the handoff). Its launch is verified; no cold-capacity pass is claimed.
Local handoff logs/markers and the remote verification markers must be inspected
before treating either stage as complete.

## Complete statistics oracle audit

A final oracle audit found that the synthetic gate checked total bytes/files and
selected counts but omitted some immediate byte totals, empty directory counts
and timestamp extrema. The production implementation is unchanged. The harness
now independently verifies all ten rollup fields for the root and every directory:
counts, bytes, immediate/tree minimum and maximum times. Sampled file attributes
also check inode/generation, exact mode, link count, UID/GID, all three timestamps,
allocation and lane counts as well as length. Every exported path remains checked.

Both scale and concurrent harnesses compile with `-Wall -Wextra -Werror` against
the named deployed library. A fresh 1000-entry construction, full export, mutation,
concurrent old/new export and fresh-process recovery all pass the stronger oracle.
The existing ten-million-entry fixture also passes complete recovery/export with
all fields checked: 21.095 s recovery, 5.688 s verification, zero rebuilt
partitions and the unchanged 3,864,480-byte builder. These timings overlap other
work and are not a speed comparison.

The stronger concurrent 10m gate also passes both complete old/new exports and
all rollups: 4,187,842 old rows emitted during replacement construction,
3,864,480 builder bytes, 225,718 retained source-owned bytes, 19.541 s and exit 0.

The waiting billion-entry recovery wrapper now uses
`/home/efs/w92-query-scale-full-oracle`; its previous waiting screen alone was
replaced, leaving the original generator running. The original gate's narrower
statistics checks will be supplemented by this complete fresh-process gate,
which still owes a passing terminal verdict.
[Raw logs and harness/library hashes](../../results/measure/20261010-w92-closure/full-stat-oracle)
preserve the strengthened checks. W92 remains open.

## Mac NFS handle-recovery fix

A separate user-owned macOS mount succeeded without sudo, resolving the earlier
stale-mount prerequisite for the platform gate. The complete oracle then failed
with ESTALE reading a new directory's `.efs/find`. The manual gateway remount
also omitted `EFS_FUSE_EXPORT=1`; restoring that required flag did not alone fix
the failure. The standard cluster gateway helper already sets the flag.

A 15-second gateway capture restricted to the `/dev/fuse` descriptor proves the
sequence for all four command names: successful LOOKUP returns an ephemeral
virtual node, entry invalidation sends FORGET, then NFS reconstructs the handle
with `LOOKUP(nodeid, ".")`. The identity was discarded at FORGET, and that lookup
returned ENOENT, becoming ESTALE at the Mac.

The fix retains an unreferenced identity in the existing fixed 64-slot table
until recycling, keeps open/lookup references protected, and supports this
virtual-file self lookup through the existing attribute/feature/permission
checks. It adds no unbounded cache or new disk format. Recycled node IDs still
never alias old handles. Focused extracted-code regressions cover FORGET/CLOSE,
immutable version/size, both pin types, exhaustion/recycling and denied/missing
handle recovery; they pass on Mac and Linux and are included in `make test`.
The entire native suite on the host passes with the candidate (exit 0).

With the candidate gateway, the complete Mac NFSv4.0 oracle passes all 141
payload hashes (561174 bytes), source/help, lookup-only behavior, stats, JSON,
NUL, fresh-command convergence, retained-result immutability and full paged
export paths. A further 96 sequential fresh stats requests pass in 9.021 seconds,
beyond the identity-table capacity. A fresh Linux NFSv4.0 mount passes the same
complete oracle. The gateway native FUSE path also passes the same 141-file
full oracle. The new Mac mount is hard/TCP, port 2049 through the existing
tunnel; the gateway maps all NFS credentials to root, so this is not a UID-501
ACL-isolation result. NFS restart grace is 90 seconds and was observed before
the first successful candidate open; it is separate from the captured ESTALE.

[Failure traces, decoded requests and candidate gate results](../../results/measure/20261010-w92-closure/mac-nfs)
are preserved. Candidate human version is
`1a25d20a193b-virtual-export-candidate`, retaining the compatible client build ID
for this temporary gateway test; the committed named rollout and repeated
platform gates remain owed. Storage daemons and both large fixtures kept running.
These command-handle results do not close the original W55 NFS publication incident.

### Named 1434aa83 rollout and full population, 11:53 UTC

The exact committed source archive was built on Xorinox and its identical
binaries installed atomically on all six guests. All normal storage daemons
now run 1434aa83, files/direct-I/O/TCP, preserving /data1. The initial client
mount preceded node 1's recovered listener and failed discovery; the daemon
then became ready, and mounting succeeded without another daemon restart.
Normal clients remain opted out. The acceptance gateway explicitly enables
FUSE export handles. Native FUSE's full 141-file command/hash oracle passes
on this named build; fresh Mac and Linux NFS repeats are running.

The original synthetic process populated all 4294967296 entries in
11236.399 seconds with rc=0; index construction and its full acceptance
sequence remain outstanding. The original capacity resume process is still
live. The previous local cold-handoff watcher was intentionally stopped for
the named deployment and replaced by w92-capacity-cold-handoff-1434aa83;
the writer itself was not interrupted. The handoff preflights the exact
1434aa83 server SHA256 and polls recovered port readiness before mounting.
No completion or cold-recovery claim is made for either large fixture yet.

### Named platform gates passed, 11:55 UTC

1434aa83 passes the complete 141-file/561174-byte oracle independently on
Mac NFSv4.0, Linux NFSv4.0 and native FUSE. All report refresh_error=0 and
boot 17023102757887622. The additional 96 fresh Mac nonce queries pass in
3.314 seconds. Every byte of the three previously acknowledged candidate
fixtures was independently rechecked after the named cold restart: 423 files,
including the raw invalid-UTF8/newline/escape names, all match.

[Named deployment and platform evidence](../../results/measure/20261010-w92-closure/named-1434aa83)
includes all six guest binary identities, normal and private unit arguments,
the initial discovery failure and successful remaining rollout, and complete
platform verdicts. Acceptance-only NFS mounts and native diagnostic mounts are
retired; the normal gateway is restored with commands disabled and
FUSE export support enabled. Private capacity and policy services are preserved.
The actual capacity resume is at 363648 files/88.78125 GiB and still live; the
4b original process and queued kernel-cold recovery are also live. Neither
large acceptance gate has a complete terminal verdict yet.

### Completed policy fixture retired, 12:01 UTC

The independent 19432 policy gate has complete native/live/cold/quorum/lost-reply
and storage-fault evidence. Its diagnostic client was drained and unmounted,
then only w92-policy-efsd.service was stopped on all three nodes. All private
policy data directories are preserved. Normal efsd and private 18432 capacity
services remain active on every node.
[Retirement evidence](../../results/measure/20261010-w92-closure/policy-retirement-20261010.log).
The capacity fixture and original actual-entry run were not stopped; their
complete terminal/recovery verdicts remain required for closure.

### Acceptance audit: build-phase diagnostics, implementation pending rollout

The original acceptance sequence requests distinct index and statistics build
costs. Snapshot source/publication timestamps and a total refresh log do not
supply those phase costs. The candidate now records exclusive monotonic wall
phases for raw/name preparation, derived inode/aggregate work and final
verification/fsync/manifest persistence, exposed in command JSON as `build`.
Restores report the current restore interval and zero original build costs;
timings are not stored in the cache format. The catalog wrapper adds 40 charged
bytes, with no cardinality-dependent memory and no change to disk records,
watermarks, query matching or publication barriers.

Focused RAM/disk tests check phase presence, restored versus built state and
retained-generation immutability. Live command checks require the diagnostic
contract. Full Linux regression is in progress on an isolated host source copy.
The first archive missed root README.md and failed the architecture gate; that
failure is preserved. The second repeat reached that gate but lacked linked
result artifacts; all 11454 tracked result files were then staged, and all five
documentation checks pass. The third complete regression run is live.
Server compilation succeeds, as do warning-as-error Mac compilations of both
changed query implementation files. The Mac top-level make invocation encounters
its existing Linux-only fopencookie logger; it is not a Mac native-suite pass.
Named diagnostic deployment and live verification remain owed. The two large
original fixtures continue untouched, with no disk-format conversion required.

[Phase-diagnostic staging and compilation evidence](../../results/measure/20261010-w92-closure/build-phase-native)
preserves both archive failures and the successful preflight. They are staging
failures, not failed cache oracles, and do not count as a complete native pass.

The completed third Linux `make -j4 test` repeat exits 0, including the new
RAM/disk phase-state checks, partial IO/fsync, quota, cancellation and retained
catalog gates plus the complete native and documentation suites. Its
[raw log and terminal markers](../../results/measure/20261010-w92-closure/build-phase-native)
are preserved. Runtime phase JSON still needs named deployment/live verification;
this native pass does not close the two large-scale acceptance gates.

### Named phase rollout and recovery pilot, 12:37 UTC

All six guests have identical cdc13b88 binaries. The full committed archive
includes root documentation and its linked evidence; result files live on
/home/efs with a /tmp/efs/results symlink so the RAM-backed /tmp does not absorb
hundreds of MiB. All normal daemons retain files/direct-I/O/TCP, port 17432,
/data1 and quota 100G. Listener readiness is checked before remounting.

The complete unprivileged two-mount command oracle passes the phase contract,
raw filenames, source/help, immutable FD/seek, fresh permission denial,
symlink scope, real .efs collision precedence, more-than-16 pinned identities,
exhaustion and 128 sequential release/recovery requests. The first invocation
passed its early phase/permission gates but omitted the test port environment;
its KeyError is preserved. The corrected invocation explicitly supplies host
and port and exits 0. Fresh Mac and Linux NFSv4.0 complete 141-file/561174-byte
oracles also pass. An additional 96 Mac nonce requests check the phase object
and exact counts, all pass in 3.634 seconds. Example published wall phases:
index 388.524 ms, stats 101.704 ms, final persistence 54.072 ms, total refresh
564 ms including pin/authority/checkpoint work. This is one observed refresh,
not a throughput result or CPU-time claim.

The cdc13b88-linked full ten-million-entry recovery oracle checks every exported
path, all directory rollup fields and sampled inode attributes, exit 0:
19.415 s catch-up plus 4.344 s full verification, zero rebuilt partitions,
3864520 accounted builder bytes (40 more than the previous wrapper). The queued
actual 2^32 kernel-cold verifier now resolves this same new-code full oracle,
SHA256 77ba5036d876e338697727b2ef41b4cb50981faa445a165b23b25e5fc2b769f4;
only its waiting executable was atomically replaced, and the original generator
was not restarted. Its latest verified PID 6246 is live, with all entries
populated but no complete build/export/mutation verdict yet. The capacity writer
PID 269152 is live at 404480 files/98.75 GiB. Its new local handoff watcher
preflights exact cdc13b88 server SHA256 before any private stop.

[Named rollout, command and NFS evidence](../../results/measure/20261010-w92-closure/named-cdc13b88)
includes all six guest identities and normal/private unit arguments, pilot logs
and preserved first-invocation failure. The normal gateway and clients are
restored to commands disabled; acceptance-only mounts are retired. Private
18432 capacity daemons and both large-run watchers stay active. W92 remains
open for complete actual-entry and 400-GiB terminal/cold/export verdicts and
final resource cleanup; these new diagnostic gates do not replace them.


## Bounded external-merge syscall reduction

The actual 2^32 run remains live: its completed source population is preserved,
but at 12:47 UTC only 49 of 4096 name/alias partitions were complete. IO counters
continued advancing; this is not a stopped job. Inspection found the external
merge issuing a separate pread/pwrite for every record on every merge pass.
This produces billions of small syscalls at the required cardinality.

`names_build` now reuses its existing 2048-row sort allocation as two input
windows and one output window, each 682 records. It performs the same sorted
merge with batched IO, without adding cardinality-dependent memory or changing
the cache format, ordering, admission charge or durability barriers. Both
name and alias merges have a focused 6098-record independent qsort oracle,
unequal inputs and partial final batches, syscall bounds (under 20 reads and
12 writes), partial output followed by ENOSPC, and cancellation before IO.
The production `io_at` retry/error boundary remains in use.

Mac compilation with warnings as errors, Linux disk differential/I/O tests
and the complete Linux native repeat pass (exit 0). The native log records
10 reads and 9 writes for each 6098-row name/alias merge. A fresh process
linked against the new library also passes the complete 10m recovery oracle:
16.600 s catch-up plus 6.452 s full verification, zero rebuilt partitions and
3864520 builder bytes. This is a recovery compatibility check, not a measured
full-build speedup. The waiting actual 2^32 kernel-cold oracle was atomically
updated to the same executable, SHA256
`5f0b8dc19399c7efba74f5cddb06c159bf963f3d7f1e0fd08ed4abdeff9a1237`.
Normal and private capacity daemons remain on cdc13b88. The original scale process and
capacity writer were not interrupted. At 12:47 UTC the real writer had synced
413184 files / 100.875 GiB; its complete cold-verification gate remains pending.

[Focused merge evidence](../../results/measure/20261010-w92-closure/buffered-merge).


## Reuse completed 2^32 source with the batched implementation

The harness now accepts explicit `--populate-only` and `--resume-build` modes.
The latter requires an existing real source directory and skips source generation;
it emits `source_reused`, never a fabricated `populated` verdict. All original
build, complete export, unchanged refresh, close/reopen restore, retained-view
mutation and complete postmutation export checks still run. The separate fresh
kernel-cold oracle also remains mandatory. This harness is Linux-specific because
its cache-dropping helper uses `posix_fadvise`; the existing Mac product gates
are separate. Linux harness compilation with warnings as errors passes.

A fresh 100000-file pilot completes population, reused-source build, unchanged
refresh, cold restore, mutation and full recovery, all rc 0. The first build took
106.082 s on the concurrently loaded VM; this is not a before/after performance
comparison. It accounts for 3864520 fixed builder bytes. Full exports verify
100000 entries before and after mutation; the stronger directory/inode oracle
is used throughout. Raw logs and exit marker are retained.

The old 2^32 process (PID 6246) was deliberately superseded after its complete
4294967296-file population verdict was checked and the guest synced. Its old
kernel-cold watcher was retired first. The source population, cache candidate,
log, time and explicit superseded marker are preserved; old exit 143 is the
intentional termination, not an acceptance failure. No source namespace was
regenerated or wiped. The first retirement command found the watcher belonged
to admin's screen inventory rather than root's and stopped before any signal;
the subsequent command used the verified owner.

The replacement screen `w92-2p32-batched-v5` runs the same fixture
`/mnt/w92-scale-big-20261010/scale-4294967296-bounded-24g-v4` with
`--resume-build`. Time parent PID 554205 and actual harness PID 554207 are live;
its separate log/time/pid and terminal markers use `scale-4294967296-batched-v5`.
The linked executable SHA256 is
`6f8e6f9d324e602c030dd8f78c0846e7e8d7cd5a84b60b2a4313ad0a6e7959d1`.
It contains fd457076's batched merge and the latest complete oracle. The root
screen `w92-2p32-batched-recovery-20261010` (watcher PID 554216) requires both
the retained original population verdict and all new build/cold/mutation
verdicts before dropping guest caches and verifying the same source fixture.
It checks the stored time-parent PID while waiting. Do not treat the old 143
marker as the current run's verdict or restart from an observation timeout.

The original 4 MiB query/64 MiB owned source-pin budgets, 8 TiB per-generation,
16 TiB aggregate, 100 GiB reserve, 524288 descriptor ceiling, nice 19 and idle
IO priority remain. Whole-source borrowed memory remains separately measured.
At the initial live check the new run had RSS 7583328 KiB, and source IO counters
were advancing. Normal/private capacity daemons and the real writer were not
stopped. The writer reached 424576 files / 103.65625 GiB; its own cold handoff
still waits for the complete 400 GiB verdict. Both terminal gates remain open.

[Reused-source pilot, original population proof and exact handoff scripts](../../results/measure/20261010-w92-closure/scale-reuse-handoff).


## Bounded raw-record and slot batching

The replacement's first live check showed 24816090 write syscalls after 99 s,
while it was still building index partitions. The raw cache writer still issued
one write per key/value record and one per index slot. It now uses two fixed
64 KiB windows charged under the already-budgeted, mutually exclusive sort
scratch. A compile-time assertion proves these windows fit that allowance;
builder accounting stays 3864520 bytes. Raw bytes are flushed completely before
matching index slots. Both tails flush before the existing checked raw/index
fsync barriers. Any partial write or reserve failure aborts the candidate;
authoritative metadata and the published old cache are unaffected.

The focused oracle checks all 10000 raw records and matching offsets/lengths
with 16 writes, empty values that fill the index window first, maximum-size
values spanning raw window flushes, partial raw output followed by ENOSPC,
partial index output followed by ENOSPC, and cancellation. Linux differential
and IO tests pass, as does Mac compilation of both changed files with warnings
as errors. A fresh 100000-file populated/reused-source build, unchanged round,
cold restore, retained-view mutation/full export and fresh recovery pass;
first build 107.644 s, fixed 3864520 builder bytes. The tiny partition pilot
is dominated by many file/fsync operations; it is not evidence of a full-scale
throughput improvement.

The complete native repeat runs in host screen `w92-raw-native-20261010`, with
`raw-native.log`, `.exit` and `.finished` under
`/data1/efs/bench/w92-phase-native-20261010`. This raw batching change has not
been put into the live scale process, waiting cold oracle or cluster daemons;
wait for the complete native verdict before advancing those binaries. The
current large run and its watcher are still PID 554207/554216, and the 400 GiB
writer/handoff remain unchanged. W92 is still open.

[Raw batching focused and complete pilot evidence](../../results/measure/20261010-w92-closure/raw-batching).


## Raw batching full gates and named rollout

The complete native repeat exits 0. The large reused-source run was intentionally
advanced to the tested raw-batched library; its completed original source remains
untouched. The previous v5 build was deliberately terminated before any build or
mutation verdict and its exit 143/log preserved. The current screen is
`w92-2p32-raw-batched-v6`, time parent PID 570453, harness PID 570455. Its log/time
stem is `scale-4294967296-raw-batched-v6`; source fixture remains v4. The executing
binary and queued full cold oracle share SHA256
`67b563c13be9718a6f6ac216b2e0661d966418c657f9ee73cedfdf57b3f35e4a`.

The initially generated v6 runner retained v5's terminal marker names. Its first
cold watcher saw the predecessor's preserved 143 marker and refused; the build
itself remained healthy. Those old v5 terminal files were moved to `.superseded`,
the refusal record preserved as `.skipped-marker-mismatch`, and the corrected
root watcher now waits for the active wrapper alone to create its legacy v5
terminal names. It copies those actual verdicts to canonical v6 names, waits for
the finished marker, validates original population plus every new phase, then
runs the same strong kernel-cold oracle against the retained source. The exact
executing runner is `handoff/active-run.sh`; `handoff/run.sh` has corrected names
for any future launch. Current watcher PID 574959, root screen
`w92-2p32-raw-recovery-20261010`, is verified live. No restart was used to repair
this marker problem. At 3:34 elapsed, 49 name-index partitions were complete;
this is a progress observation, not a complete gate or controlled speedup.

Named cf4e365a daemon/client/mgmt compilation succeeds. The first build wrapper's
nested shell quoting made its exit marker unreliable; its compiler/link output
is preserved and a separate structured script waits for that make and then
requires a real make verdict, exit 0. Named source and binaries were staged and
hashed on all six guests, with prior binaries retained. A normal-only cold
rollout exits 0; all normal servers run files/direct-I/O/TCP on port 17432 with
3-voter metadata, `/data1`, 100G quota. Source/code archive is 13 MiB and excludes
result artifacts, retaining each guest's existing disk-backed result symlink.
The private capacity servers on 18432 were not restarted or reformatted.

All six binary identities match, and `/proc/PID/exe` verifies the actual normal
server and client mappings, separately from private old-code mappings:

- efsd: `caf15279c8aa176882678e656ae368a38957b8ca891c36967fde9f9145a12629`
- efs-fuse: `9e40c2820808c4639116071d20896d982c47edac564759efa244d4b84169d4cc`
- efs-mgmt: `ef51dae578e4871cd8d85056175d52ac49c9e4267064a51950fa05c09bf9b302`

Native two-mount command/permission/identity gates exit 0. Fresh Mac and Linux
NFS complete oracles each verify all 141 files / 561174 bytes, every payload
hash, full paged paths, stats, raw names, retained results and new-command
convergence, exit 0. As before, gateway credential squashing is not a native
per-user permission proof. Acceptance NFS and node111 mounts are retired;
normal gateway/client mounts are commands-disabled and NFS service active.
The user's separate standard Mac mount was not remounted or claimed fresh.

The capacity handoff was advanced to local screen
`w92-capacity-cold-handoff-cf4e365a` (PID 44782), preflighting the exact new server
hash before its eventual private all-three cold stop. The writer and its
acknowledged source/checkpoints were not interrupted. Full 2^32 and real 400 GiB
terminal/cold/export/hash gates and final private-fixture/RAM cleanup are still
open; named pilots and full native/platform repeats do not replace them.

[Named binary, mapped runtime, native and platform evidence](../../results/measure/20261010-w92-closure/raw-batching).
