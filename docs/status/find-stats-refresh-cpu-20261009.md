# W92 NUC node 2 CPU — October 8, 2026 New York

## Retained partition lookup keys: 800122c5e86d

Removed per-generation global inode hash construction. Immutable partition
blocks retain sorted 64-bit keys; lookup uses shard routing, binary search and
the current catalog's local ordinal range. Keys are strictly validated and
fully accounted. Differential/boundary/lifetime, command/export/permission,
POSIX 212/212, peer 64/64 and restart gates pass. Deployed gracefully with data,
direct I/O/TCP, .efs and 1 GiB cap preserved. Nearby warm median 57 ms versus
prior block build 147 ms; n2 builder cycle share 28.30% versus 44.59%. Live
window differences limit attribution. Loaded export returns 1,224,379 unique
paths in 22.09 seconds; no precise prior export-time baseline. Remaining work
includes pointer-vector scans, directory rollups and changed-block folding.
See [full evidence](../../results/measure/20261009-catalog-index/README.md).

## Compact immutable inode blocks: a62cbd5f6ca2

Deployed reference-counted partition-local decoded inode/stat blocks with
inode/lane dependency validation and directory-only mutable rollups. Unchanged
blocks are retained without global row decoding; accounting charges shared
blocks fully. Full native, differential/lifetime, POSIX 212/212, peer 64/64,
command/export/permission and restart gates pass. Data preserved, cap unchanged.
Publication recovers past the former NOMEM point: 1,181,069 files at
1,015,659,920 accounted bytes. Loaded export completes with 1,187,139 unique
paths. Nearby warm median 147 ms versus prior healthy 284 ms; node-2 page-fault
share 1.38% versus 23.63%. Live workload/host-load differences limit comparison;
no aggregate CPU or controlled throughput claim. Global lookup index and
pointer-vector/directory scans remain software hot paths. See
[full evidence and limitations](../../results/measure/20261009-catalog-blocks/README.md).

## Deadline clock follow-up: 64cdf183d265

Deployed one targeted fix: retain atomic shutdown checks per callback, read the
deadline clock every 256 callbacks, and force a deadline check before publish.
Repeated simultaneous four-node 30-second profile records 11,977 samples,
zero lost. Node 2 clock_gettime inclusive share falls 5.89% to 0.23%. Private
command/export/permission/restart gates, POSIX 212/212 and peer 64/64 pass.
Data preserved; direct I/O/TCP and .efs remain enabled. The 8,000-file workload
completed and live stats report no refresh error. Total CPU reduction is not
established across these differing live windows; array rebuilding and page
allocation still dominate. See [full comparison and raw evidence](../../results/measure/20261009-query-clock/README.md).

## Follow-up: partition-local stat reuse, 093295be288b

October 9 UTC: profiled node 2 for 30 seconds at 199 Hz under the active
eight-worker tiny-files workload. Baseline build `abcd9b7b1b6f` recorded 5,597
samples, zero lost. Global inode hash probes for unchanged stat reuse dominated
builder instructions: the indexed load accounted for 46.97% and its probe
branch 14.01% of local annotation periods.

Commit `093295be288b` records each partition's inode-vector range. Identical
retained partitions have identical sorted keys, allowing direct partition-local
ordinal reuse. Lane dependencies are still validated; changed inputs use
authoritative getattr. Count/inode checks fail closed. The additional 16 KiB
range array is accounted. Differential tests insert/remove an inode in an
earlier partition to catch global offset shifts; lane-only mutation gates pass.

Deployed all five binaries after graceful generator, mount and server drain;
existing data preserved, direct I/O/TCP and client/server commands enabled.
Deployment exit zero. Restarted into `tiny-ordinal-093295be`, eight workers,
depth eight, 1,000 files/folder, bounded 20,000-file target. At 29.1 seconds:
4,397 files, 151 files/s. This is not a controlled throughput comparison.

| Measurement | Before | After |
| --- | ---: | ---: |
| 30-second perf samples | 5,597 | 6,222 |
| Query thread inclusive process cycles | 79.69% | 65.68% |
| Builder inclusive process cycles | 79.05% | 64.50% |
| Builder self process cycles | 50.69% | 26.56% |
| Median of 60 nearby warm publications | 445.5 ms | 376 ms |
| Warm build range | 416–493 ms | 301–727 ms |
| Namespace files | 1,076,952–1,080,705 | 1,091,252–1,095,546 |

Median refresh improved 15.6%. Live writes, namespace growth and varying host
load limit this comparison; cycle shares are not absolute CPU measurements.
Remaining software work includes global inode unpacking/allocation, page
faults, index construction and dependency checks. No hardware ceiling claimed.
Catalog accounting was about 1.03 GB against the unchanged 1 GiB cap; continued
growth needs monitoring.

Validation: differential catalog unit PASS; full native `make -j8 test` exit
zero; private publication/RPC, two-mount command options, bounded display and
export, pinned generation and permissions PASS; POSIX 212/212, peer 64/64;
restart reconstruction and live capped find/stats PASS. The page fixture gate
now waits for all 215 files to reach the background snapshot. An intermediate
private run was invalidated when deployment moved its build directory before
the restart stage; the complete suite was rerun from the stable installed path.

Raw profiles, annotations, timings, screen capture, validation and deployment
evidence (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261009-catalog-ordinal`).

User observed node-id 2 at 100% CPU and requested strace investigation.
Installed build: devel `ab050595c3b8`, direct I/O/TCP, `.efs` enabled.
Node 2 PID 1488325; the existing tiny-file generator remains active. No code,
configuration, service or generator changes were made for this measurement.

## Measured cause

A three-second /proc thread CPU sample measures 100.9% aggregate process CPU,
with thread 1488961 alone at 91.3% of one core. A separate ten-second 99 Hz perf
cycle/callgraph recording has no lost samples. That thread accounts for 92.09%
of sampled cycles; inclusive `host_query_thread` is 89.98% and
`efs_query_catalog_build` 89.46%. Major builder work includes:

- `efs_meta_apply_getattr`: 48.01% inclusive, especially cached transaction-row
  reads and comparisons.
- Name-index iteration / `add_edge`: about 16.9% inclusive.
- Node rebuilding/allocation and memory clearing, including page faults.

This is the background `.efs` snapshot publisher, not foreground find rendering
or an observed GC/disk saturation problem. The preceding find result overflow
is a different W92 limit; fixing output paging alone will not remove this CPU.

A six-second `strace -f -c` attached to all 33 initial threads (and new children)
records 102,795 syscalls: futex 51.96%, recvfrom 32.65%, sleep 6.27%, poll 5.44%
of accumulated syscall elapsed time. These are overlapping waits across
threads, not process CPU percentages. It also records 1,235 fdatasync and 2,086
fsync calls under active writes. The syscall summary alone cannot attribute
CPU or prove underlying storage is saturated; perf provides the attribution.

## Why refresh consumes nearly a core

`host_query_thread` in `src/server/raft_host.c` wakes every 100 ms and starts a
refresh after **256 metadata changes OR two seconds**. Two seconds is not a
minimum refresh interval. Under the active CREATE/write/close workload, the
256-change trigger is reached quickly and refreshes run almost continuously.

`src/meta/query_catalog.c:efs_query_catalog_build` reuses unchanged partition
caches and name indexes, but still reconstructs the global inode vector/index,
folds getattr for **every inode**, iterates **every partition's names** to rebuild
edges, and recomputes directory rollups. Partition reuse therefore does not make
the complete catalog refresh proportional to the changed namespace.

Success logs show generations 693–699 building in **1,296–1,474 ms**, about
**1.4–1.6 seconds between publications**. Only **8–13 of 4,096 partitions**
are rebuilt, while catalog files grow from 1,010,348 to 1,011,028. This explains
the sustained CPU even without invoking `.efs/find` repeatedly.

The last captured catalog accounts **940,787,102 bytes (897.2 MiB)** against the
configured **1,024 MiB** limit. That is catalog accounting, not total process
RSS; source snapshots, the old publication and the candidate also occupy
memory. Continued growth risks cap refusal and stale publication, and should
not be addressed by blindly increasing the memory ceiling. The excerpt also
contains one refresh BUSY (`rc=-13`) after a successful publication; this is
not evidence of a permanent failure or disk error.

Cluster status at capture: four nodes up, identical build, all heal workers
idle; status reports about 123.24 GiB usable quota usage against 500 GiB
capacity. That quota-accounted space differs from metadata's logical file-byte
rollup; this CPU investigation does not audit that space difference.

## W92 follow-up

- Make catalog updates incremental beyond shard cache/name-index reuse: reuse
  unchanged folded inode metadata and update affected bindings/ancestor rollups,
  preserving immutable generations and authoritative metadata reduction.
- Introduce an explicit background CPU/work budget or refresh cadence with
  truthful lag/overdue reporting. Scheduling less often trades freshness for
  CPU; it does not remove the O(namespace) rebuild cost or establish a hard T.
- Reduce catalog memory per file and measure old/candidate/source peaks. Preserve
  the bounded cap, error reporting and valid old snapshot on refresh failure.
- Gate a loaded million-file namespace: small mutation batch versus full build,
  sustained CREATE workload, idle CPU, freshness, RSS, restart catch-up and exact
  independent stats/find oracles. Do not infer a hardware limit from this trace.

No implementation fix or improved throughput is claimed. The active generator
and `.efs` service were left running. Raw perf data/reports, thread counters,
strace summary, refresh excerpts and cluster status are retained in
[measurement evidence](../../results/measure/20261009-node2-query-cpu).

## Requested 30-second callgraph follow-up

A fresh 30-second `perf record -F 199 -g -p 1488325 -- sleep 30` capture on
unchanged installed build `ab050595c3b8` records **6,838 samples, zero lost**.
The generator remains active; no simultaneous strace attachment was used.
Raw recording and full/self/inclusive/thread reports are retained under
`results/measure/20261009-node2-query-cpu/profile-30s/`.

| Function | Self cycles | Inclusive cycles |
| --- | --- | --- |
| `host_query_thread` | 0.00% | 86.82% |
| `efs_query_catalog_build` | 3.97% | 86.56% |
| `efs_meta_apply_getattr` | 6.04% | 49.55% |
| `efs_txn_read_ex` | 2.39% | 40.37% |
| `cache_get` | 19.68% | 22.51% |
| `add_edge` | 13.81% | 15.73% |
| libc key comparisons (`memcmp`) | 13.01% | 14.60% |
| libc memory clearing (`memset`) | 4.33% | 4.45% |

Inclusive rows overlap; they must not be added together. Percentages describe
sampled CPU cycles, not elapsed-time or syscall-wait shares. Thread 1488961
accounts for 88.85% of cycles in this capture.

The dominant call chain is:

```text
host_query_thread
  efs_query_catalog_build
    efs_meta_apply_getattr       # folded stat for every catalog inode
      efs_txn_read_ex
        cache_get               # binary search + compare + copy
      efs_txn_reduce_read_ex
        efs_txn_read_ex
          cache_get
```

The second major chain is full name-index iteration into `add_edge`, looking
up parent/child nodes, resolving local/hashed layout precedence, then accumulating
immediate counts, bytes and timestamps. Node reconstruction adds allocation,
clearing and page-fault cost (`add_node` 7.35% inclusive; page-fault entry 6.27%
inclusive, overlapping allocations). This confirms the earlier attribution
with a longer recording rather than establishing a new storage limit.

Nearby publication logs continue to show about 1.4–1.6-second builds with only
8–10 partition rebuilds. The last captured publication has 1,022,266 files,
4,355 directories and 951,042,964 accounted catalog bytes (about 907 MiB).
The unchanged 1 GiB cap remains a separate approaching constraint.

### Optimization priorities supported by this trace

1. Avoid folding stats for unchanged inodes. Cache folded results with all
   relevant dependencies, including cross-shard lanes/reductions and transaction
   decisions; an inode-shard revision alone is insufficient for correctness.
2. Reuse immutable namespace relationships and update affected directory/
   ancestor rollups instead of reconstructing every edge for a small mutation
   batch. Preserve layout precedence, rename/hardlink handling and exact totals.
3. After reducing whole-catalog work, investigate repeated cached lane reads:
   `efs_txn_reduce_read_ex` reads a materialized lane that getattr reads again.
   Sharing a decoded read within a verified immutable cut may remove redundant
   binary searches/copies, but must retain pending-transaction reduction and
   authoritative stat semantics. Do not replace folded size with base_size.
4. Limit refresh work/frequency with truthful lag reporting as a scheduling
   measure, rather than treating it as an algorithmic or hardware-ceiling fix.

The profile above is the pre-fix baseline; the implementation and deployment below follow it.


## Incremental reuse fix and NUC deployment

Production commit `5084f8c2d557` on `devel-find-stats` is installed in
`/data1/efs/src` on all four NUC nodes and both clients, with direct I/O, TCP,
server/client commands enabled and the existing 1024 MiB catalog cap. Existing
stores were preserved. Both clients were gracefully unmounted/drained and all
four daemons terminated gracefully before installation. The generator had
already stopped before deployment. The previous source/binaries are retained
at the backup path recorded in the deployment evidence.

The catalog now reuses folded inode stats only when the inode partition and
all active lane partitions are unchanged. Immutable per-partition namespace
contributions are reused only when their recorded name, parent layout, child
inode and lane dependencies are unchanged. Layout precedence and hashed
masking remain authoritative. Directory links and rollups are replayed into
the new immutable generation. Global inode vector/index reconstruction and
ancestor aggregation still run; this is not yet an O(changed entries) builder.
Exact inode-vector allocation also avoids a transient capacity doubling at
2^20 inodes. Cached contributions remain inside the explicit catalog budget.

### Measured result

A new 30-second, 199 Hz perf recording of node 2 during the same eight-worker
random tiny-file workload contains 7,153 samples with no lost samples. Sixty
nearby warm publications take **463–639 ms, median 508 ms**, versus the prior
roughly 1.3–1.6 seconds. The namespace is slightly larger (over 1.03 million
files), so this is an observational comparison, not a controlled throughput
experiment. The screen reports 6,262 files after 40 seconds, 157 files/s;
there is no demonstrated file-creation throughput improvement.

Inclusive getattr falls from 49.55% to **0.54%** of process cycles; add_edge
from 15.73% to **0.55%**; cache_get from 22.51% to **0.20%**. The builder remains
70.26% inclusive and 47.03% self: global array/index reconstruction, dependency
checks and memory/page-fault work remain. Faster publication can still run
frequently under the unchanged 256-change trigger. This does not prove the
machine is at a hardware ceiling or promise low total CPU under sustained
mutation.

Cold publication still folds the whole namespace: the first successful build
took 3,688 ms for 1,033,117 files and 4,386 directories. Warm reuse works after
that; accounted catalog memory was 964,379,748 bytes initially. The 1 GiB cap
remains a scale constraint.

### Validation and separate limitations

- Native executable unit tests passed, including a new differential oracle:
  incremental and full catalogs built from the same pinned source agree on
  every inode/stat, directory binding and rollup. Coverage includes cross-shard
  lane-only changes, rename/unlink/hardlinks, hashed layout, pending-transaction
  refusal, cancellation and memory limits. Unchanged input refolds zero stats
  and reuses all 4,096 edge partitions.
- All five architecture documentation checks passed after adding README and
  LICENSE to the isolated source fixture. The optional ASAN/UBSAN executable
  could not link because the NUC sanitizer runtime was missing; no sanitizer
  pass is claimed.
- Private direct/TCP publication and v1/v2/v3 RPC gates passed, including
  restart rebuild, immutable identity, redirects and explicit errors.
- Full private direct/TCP two-mount command and display/filter suites passed,
  followed by POSIX **212/212** (19.7 s), peer **64/64** (15.6 s), and persisted
  restart recovery. These private tests do not establish million-file acceptance.
- Live deployment smoke passed on both installed mounts: scoped ls/find/stats
  returned complete, correct results for a newly created and removed probe.

The loaded mounted command suites exposed a separate existing scan limit:
find visits at most 1,000,000 index entries across the whole namespace, even
for a scoped query. With this namespace above that threshold, suffix `.txt`
and empty-pattern queries return EFS_ERR_BUSY; the client retries until its
8-second budget, and command tests time out. Direct queries to both publishers
confirm suffix status -13 in 44–72 ms, while exact `file.txt` succeeds in
17–23 ms. This is distinct from the earlier 16 MiB response cap. Paging/scoped
index traversal and a non-retryable, limit-specific error remain W92 work.
The broad loaded command suites are **not passing** and W92 is not closed.

The profiling generator was launched with a 75-second SIGINT timeout in the
existing tiny_files screen and a new output directory
`tiny-incremental-5084f8c2`. Its ThreadPoolExecutor drains already queued folder
tasks after interruption, so shutdown takes longer than the nominal timeout.
The generator has now exited after draining. A subsequent ten-second /proc
CPU sample measures node 2 at **1.8% of one core** while quiet; this is not
a comparable loaded before/after CPU measurement. No daemon restart or forced
write discard was performed during this drain. A quiet repeat of the live
suffix gate still times out at the same scan cap; the options repeat refuses
its already existing test directory, so it adds no acceptance evidence.

Raw deployment/profile evidence is retained in
results/measure/20261009-catalog-incremental (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261009-catalog-incremental`)
and on NUC at `/data1/efs/deploy-query-5084f8c2`. Native unit/build logs and private
fixture logs remain under `/data1/efs/query-incremental-*`.


## Follow-up: bounded display and full export

The subsequent `abcd9b7b1b6f` deployment adds bounded query pages, a default
100-result display cap, an override up to 10,000, and complete streaming file
exports. Root `.txt` queries and a 1,068,326-path export now pass on NUC; the
historical failure above remains reproducible only on the old command/wire
path. See [the contract and acceptance](efs-command-pages-20261009.md).
