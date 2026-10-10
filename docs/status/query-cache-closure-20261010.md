# W92 closure work — acceptance ledger

Status: in progress, devel-find-stats, exclusive Xorinox. Closure is not claimed.
The user explicitly requested work until W92 is closed. The existing 2^32 synthetic
index and staged real-filesystem requirements remain binding; the earlier three
profiling rounds do not satisfy them.

| Gate | Current state |
|---|---|
| Immutable disk catalog, incremental exact aggregates, bounded builder scratch | implemented, differential/live/cold gates pass in previous checkpoint |
| Aggregate retained-cache admission | implemented; native and live QUOTA/write/cold-recovery oracles pass |
| Durable changed-key watermarks and bounded catch-up/recovery | durable partition revisions, restart catch-up and bounded discovery implemented; focused/full native and live cold/disable-re-enable/publisher-loss/interrupted-cache gates pass |
| Explicit cardinality/offset/budget audit | revision saturation, complete source-allocation budgeting and representation audit complete; actual scale/live validation pending |
| Increasing-cardinality RSS, cold-cache, concurrent export/mutation and freshness | pilots and live contention/freshness pass; latest source-scratch deployment, live command/pages and measured query/writer load pass; full actual scale pending |
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
These are observed pilots, not extrapolated 2^32 acceptance. The version-5 repeat also completed with every file checked before and after
an inode-only mutation. Fresh build 291.206 s; unchanged refresh 0.617 s;
full export verification 7.196 s; mutation refresh 2.932 s (one partition,
2443 inode refolds), followed by a second full export verification in 7.446 s.
The old snapshot retained its original exact totals. Accounted builder memory
3,864,480 bytes; whole-process peak RSS 41,000 KiB. These are single shared-VM,
warm-kernel measurements, not a controlled throughput comparison or the 2^32 gate.
See [version-5 evidence](../../results/measure/20261010-w92-closure/scale-10m-v5/).

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
evidence is [retained here](../../results/measure/20261010-w92-closure/scale-10m-bounded-v3/).

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
