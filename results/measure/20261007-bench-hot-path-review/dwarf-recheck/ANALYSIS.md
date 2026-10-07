# EFS benchmark analysis

Host: `xorinox`. Event: `cycles`.
Binary SHA256: `f8cbefa384f3db48cc5c286d7591cd639ae2487256787027460e87e8be80f5a6`. Commit: `unknown`.

Throughput comes from untraced baseline runs. Perf is a separate rerun; optional strace is another rerun.
CPU sample shares show where execution was sampled, not wall-time spent waiting or proof of an I/O bottleneck.
Profiles filter to efs-bench threads; fio ceiling child processes are excluded. Source annotations need matching debug/source files.
Raw I/O profiles capture only the parallel timed loop; setup, read population and post-run validation are excluded by perf-control acknowledgement.
Data profiles combine write and read phases (and path-count ladder when multiple roots are supplied).

Storage root `/data1/efs/bench`: filesystem device `259:0`, initial free bytes 3551103401984.

| Case | Result | Baseline measurements | Hottest sampled symbols |
|---|---|---|---|
| [data-buffered-w0-qd16](data-buffered-w0-qd16/baseline.stdout) | PASS | write/1/16 GiB_s=0.761 ops_s=12463.4 p50_us=1505 p99_us=3438 max_us=4684; read/1/16 GiB_s=40.900 ops_s=670109.0 p50_us=23 p99_us=27 max_us=77602 | blake3_hash_many_avx512 (62.8%), _copy_to_iter (7.6%), selinux_inode_permission (1.1%) |
| [data-direct-w0-qd16](data-direct-w0-qd16/baseline.stdout) | PASS | write/1/16 GiB_s=0.719 ops_s=11773.3 p50_us=1449 p99_us=3252 max_us=116731; read/1/16 GiB_s=1.477 ops_s=24201.7 p50_us=709 p99_us=1185 max_us=4093 | blake3_hash_many_avx512 (12.3%), xfs_btree_get_leaf_keys (4.1%), __memmove_avx512_unaligned_erms (3.5%) |

## Baseline comparisons

- buffered write, 1 path(s), engine checksum, 65536 bytes, legacy, default: best observed 0.761 GiB/s, p99 3438 us, `data-buffered-w0-qd16`.
- buffered read, 1 path(s), engine checksum, 65536 bytes, legacy, default: best observed 40.900 GiB/s, p99 27 us, `data-buffered-w0-qd16`.
- direct write, 1 path(s), engine checksum, 65536 bytes, legacy, default: best observed 0.719 GiB/s, p99 3252 us, `data-direct-w0-qd16`.
- direct read, 1 path(s), engine checksum, 65536 bytes, legacy, default: best observed 1.477 GiB/s, p99 1185 us, `data-direct-w0-qd16`.

These are best observations within this run, not production configuration recommendations.

## CPU hot paths


**data-buffered-w0-qd16 sampled CPU:** BLAKE3 64.1%, other 10.4%, memory copies/fills 8.3%, synchronization/scheduling 1.1%
[Flat](data-buffered-w0-qd16/perf/flat.txt), [threads](data-buffered-w0-qd16/perf/by_thread.txt), [call chains](data-buffered-w0-qd16/perf/callers.txt), source annotations under `data-buffered-w0-qd16/perf/annotate-*.stdout`.


**data-direct-w0-qd16 sampled CPU:** other 29.5%, storage I/O/filesystem 26.4%, BLAKE3 12.9%, synchronization/scheduling 11.4%, page pinning/IOMMU 5.5%, memory copies/fills 4.1%, time/vDSO 0.4%
[Flat](data-direct-w0-qd16/perf/flat.txt), [threads](data-direct-w0-qd16/perf/by_thread.txt), [call chains](data-direct-w0-qd16/perf/callers.txt), source annotations under `data-direct-w0-qd16/perf/annotate-*.stdout`.


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
