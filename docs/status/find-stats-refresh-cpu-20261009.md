# W92 NUC node 2 CPU — October 8, 2026 New York

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
[measurement evidence](../../results/measure/20261009-node2-query-cpu/).

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

No production optimization is implemented or benchmarked by this profile.
