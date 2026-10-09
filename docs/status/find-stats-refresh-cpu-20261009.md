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
