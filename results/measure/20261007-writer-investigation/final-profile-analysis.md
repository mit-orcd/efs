# EFS benchmark analysis

Host: `xorinox`. Event: `cycles`.
Binary SHA256: `e1cf62b3a8d25bedcfa9c43982f830654522cd61916a4c05c5784538a6c3a186`. Commit: `unknown`.

Throughput comes from untraced baseline runs. Perf is a separate rerun; optional strace is another rerun.
CPU sample shares show where execution was sampled, not wall-time spent waiting or proof of an I/O bottleneck.
Profiles filter to efs-bench threads; fio ceiling child processes are excluded. Source annotations need matching debug/source files.
Raw I/O profiles capture only the parallel timed loop; setup, read population and post-run validation are excluded by perf-control acknowledgement.
These four data profiles isolate read/write phases with --data-rw split and --data-full-paths.

Storage root `/data1/efs/bench`: filesystem device `259:0`, initial free bytes 3532525670400.

| Case | Result | Baseline measurements | Hottest sampled symbols |
|---|---|---|---|
| [data-buffered-w0-qd16-write](data-buffered-w0-qd16-write/baseline.stdout) | PASS | write/1/16 GiB_s=30.099 ops_s=493143.1 p50_us=31 p99_us=44 max_us=2160 | copy_page_from_iter_atomic (40.3%), __d_lookup_rcu (3.9%), __memset_avx512_unaligned_erms (2.4%) |
| [data-buffered-w0-qd16-read](data-buffered-w0-qd16-read/baseline.stdout) | PASS | read/1/16 GiB_s=36.931 ops_s=605077.2 p50_us=26 p99_us=35 max_us=14049 | blake3_hash_many_avx512 (51.9%), _copy_to_iter (17.9%), filemap_get_read_batch (1.4%) |
| [data-direct-w0-qd16-write](data-direct-w0-qd16-write/baseline.stdout) | PASS | write/1/16 GiB_s=5.802 ops_s=95061.8 p50_us=156 p99_us=290 max_us=13939 | xfs_btree_get_leaf_keys (11.3%), xfs_extent_busy_trim (8.0%), xfs_rmapbt_init_high_key_from_rec (6.3%) |
| [data-direct-w0-qd16-read](data-direct-w0-qd16-read/baseline.stdout) | PASS | read/1/16 GiB_s=2.074 ops_s=33973.3 p50_us=350 p99_us=2357 max_us=68747 | blake3_hash_many_avx512 (36.8%), __memmove_avx512_unaligned_erms (7.2%), __bio_iov_iter_get_pages (4.5%) |

## Baseline comparisons

- buffered write, 1 path(s), engine checksum, 65536 bytes, legacy, default: best observed 30.099 GiB/s, p99 44 us, `data-buffered-w0-qd16-write`.
- buffered read, 1 path(s), engine checksum, 65536 bytes, legacy, default: best observed 36.931 GiB/s, p99 35 us, `data-buffered-w0-qd16-read`.
- direct write, 1 path(s), engine checksum, 65536 bytes, legacy, default: best observed 5.802 GiB/s, p99 290 us, `data-direct-w0-qd16-write`.
- direct read, 1 path(s), engine checksum, 65536 bytes, legacy, default: best observed 2.074 GiB/s, p99 2357 us, `data-direct-w0-qd16-read`.

These are best observations within this run, not production configuration recommendations.

## CPU hot paths


**data-buffered-w0-qd16-write sampled CPU:** memory copies/fills 42.7%, other 28.9%, storage I/O/filesystem 4.5%, synchronization/scheduling 1.6%
[Flat](data-buffered-w0-qd16-write/perf/flat.txt), [threads](data-buffered-w0-qd16-write/perf/by_thread.txt), [call chains](data-buffered-w0-qd16-write/perf/callers.txt), source annotations under `data-buffered-w0-qd16-write/perf/annotate-*.stdout`.


**data-buffered-w0-qd16-read sampled CPU:** BLAKE3 53.7%, memory copies/fills 19.1%, other 12.1%, storage I/O/filesystem 0.5%
[Flat](data-buffered-w0-qd16-read/perf/flat.txt), [threads](data-buffered-w0-qd16-read/perf/by_thread.txt), [call chains](data-buffered-w0-qd16-read/perf/callers.txt), source annotations under `data-buffered-w0-qd16-read/perf/annotate-*.stdout`.


**data-direct-w0-qd16-write sampled CPU:** storage I/O/filesystem 76.0%, other 12.3%, page pinning/IOMMU 1.6%, synchronization/scheduling 0.8%, memory copies/fills 0.7%, time/vDSO 0.7%, unresolved samples 0.4%
[Flat](data-direct-w0-qd16-write/perf/flat.txt), [threads](data-direct-w0-qd16-write/perf/by_thread.txt), [call chains](data-direct-w0-qd16-write/perf/callers.txt), source annotations under `data-direct-w0-qd16-write/perf/annotate-*.stdout`.


**data-direct-w0-qd16-read sampled CPU:** BLAKE3 39.8%, other 33.7%, memory copies/fills 7.2%, page pinning/IOMMU 6.2%, storage I/O/filesystem 3.6%, synchronization/scheduling 3.4%
[Flat](data-direct-w0-qd16-read/perf/flat.txt), [threads](data-direct-w0-qd16-read/perf/by_thread.txt), [call chains](data-direct-w0-qd16-read/perf/callers.txt), source annotations under `data-direct-w0-qd16-read/perf/annotate-*.stdout`.


Cluster net/store/read/metadata modes were not requested: no seed supplied.

Recorded perf events: `cycles`. Requested events can be narrowed by host permissions.

Raw buffered I/O is a warm bounded working-set measurement, not physical-device throughput. High QD means more synchronous workers, not asynchronous queue slots.
Raw operation-cycle latency includes benchmark loop bookkeeping and scheduling; compare throughput and latency together. Idle workers invalidate a case; max_start_us records release-to-first-operation delay.
Unresolved kernel/vDSO addresses are retained, not guessed. Time/vDSO and unresolved samples are separate categories.
Category totals are heuristic, mutually exclusive self-sample classifications above the 0.3% report threshold; inspect symbols/callers before choosing changes.
BLAKE3 reuses per-worker buffers: this measures warm-buffer CPU throughput, not disk bandwidth. Streaming omits per-buffer finalize overhead.
Raw allocating writes fill the window once then overwrite it: allocation_ops and overwrite_ops distinguish first fill from warm overwrites. They are not an append-only allocation ceiling.
Local data writes wrap within the configured bounded working set after filling it; profiles include create and replacement work. Direct local CLI without --window retains append-only writes.
Buffered storage reads are warm; compare direct-I/O cases and the separate ceiling probe before inferring device limits.
