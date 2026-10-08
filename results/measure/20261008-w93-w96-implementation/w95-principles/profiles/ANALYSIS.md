# EFS benchmark analysis

Host: `amd-395`. Event: `cpu-clock`.
Binary SHA256: `3a62c83bb411980249b82c77990b61ded7a0dcbd38f1cf575ba177e7813aec16`. Commit: `unknown`.

Throughput comes from untraced baseline runs. Perf is a separate rerun; optional strace is another rerun.
CPU sample shares show where execution was sampled, not wall-time spent waiting or proof of an I/O bottleneck.
Profiles filter to efs-bench threads; fio ceiling child processes are excluded. Source annotations need matching debug/source files.
Raw I/O profiles capture only the parallel timed loop; setup, read population and post-run validation are excluded by perf-control acknowledgement.
Engine bounded-write windows are populated before timing. Store-call latency includes admission, scheduling and I/O; p99 is a bounded histogram estimate and max is an exact observed duration. Runs below 10,000 samples are labelled low_samples; maxima are not guarantees.
Data profiles follow --data-rw: both combines phases, split/read/write isolate them. The path-count ladder is omitted with --data-full-paths.

Storage root `/data1/efs`: filesystem device `259:2`, initial free bytes 483984748544.

| Case | Result | Baseline measurements | Hottest sampled symbols |
|---|---|---|---|
| [principles-files-write-allocating-each-512-qd4-buffered](principles-files-write-allocating-each-512-qd4-buffered/baseline.stdout) | PASS | write/files/allocating/each/1/4 GiB_s=0.003709 ops_s=7779.026 payload_bytes=512 logical_payload_bytes=512 record_bytes=544 container_alignment=4096 directory_sync=ancestors latency=operation_to_durable_completion latency_samples=8192 p50_us=503 p95_us=823 p99_us=1063 max_us=2261 reclaimed_allocated_bytes=0 syncs=49152 verified_objects=8192 | filemap_get_folios_tag (10.4%), __refill_objects_node (7.4%), ext4_match (5.5%) |
| [principles-files-write-allocating-batch-512-qd4-buffered](principles-files-write-allocating-batch-512-qd4-buffered/baseline.stdout) | PASS | write/files/allocating/batch/1/4 GiB_s=0.003691 ops_s=7740.164 payload_bytes=512 logical_payload_bytes=512 record_bytes=544 container_alignment=4096 directory_sync=ancestors latency=operation_to_durable_completion latency_samples=8192 p50_us=16319 p95_us=17407 p99_us=22271 max_us=22577 reclaimed_allocated_bytes=0 syncs=9472 verified_objects=8192 | ext4_match (8.1%), blake3_compress_in_place_avx512 (5.7%), __refill_objects_node (4.9%) |
| [principles-container-write-allocating-each-512-qd4-buffered](principles-container-write-allocating-each-512-qd4-buffered/baseline.stdout) | PASS | write/container/allocating/each/1/4 GiB_s=0.003208 ops_s=6727.615 payload_bytes=512 logical_payload_bytes=512 record_bytes=4096 container_alignment=4096 directory_sync=ancestors latency=operation_to_durable_completion latency_samples=8192 p50_us=509 p95_us=1079 p99_us=1343 max_us=5374 reclaimed_allocated_bytes=0 syncs=49152 verified_objects=8192 | _raw_spin_unlock_irqrestore (13.4%), jbd2_journal_add_journal_head (5.6%), __jbd2_journal_file_buffer (4.9%) |
| [principles-container-write-allocating-batch-512-qd4-buffered](principles-container-write-allocating-batch-512-qd4-buffered/baseline.stdout) | PASS | write/container/allocating/batch/1/4 GiB_s=0.070139 ops_s=147091.555 payload_bytes=512 logical_payload_bytes=512 record_bytes=4096 container_alignment=4096 directory_sync=ancestors latency=operation_to_durable_completion latency_samples=8192 p50_us=795 p95_us=1263 p99_us=1463 max_us=2382 reclaimed_allocated_bytes=0 syncs=1536 verified_objects=8192 | blake3_compress_in_place_avx512 (16.0%), _raw_spin_unlock_irqrestore (12.0%), __filemap_add_folio (8.0%) |
| [principles-files-write-allocating-each-512-qd4-direct](principles-files-write-allocating-each-512-qd4-direct/baseline.stdout) | PASS | write/files/allocating/each/1/4 GiB_s=0.004454 ops_s=9340.034 payload_bytes=512 logical_payload_bytes=512 record_bytes=4096 container_alignment=4096 directory_sync=ancestors latency=operation_to_durable_completion latency_samples=8192 p50_us=333 p95_us=771 p99_us=1095 max_us=2767 reclaimed_allocated_bytes=0 syncs=49152 verified_objects=8192 | __refill_objects_node (10.5%), __iomap_dio_rw (7.8%), __ext4_check_dir_entry (5.8%) |
| [principles-files-write-allocating-batch-512-qd4-direct](principles-files-write-allocating-batch-512-qd4-direct/baseline.stdout) | PASS | write/files/allocating/batch/1/4 GiB_s=0.006910 ops_s=14492.310 payload_bytes=512 logical_payload_bytes=512 record_bytes=4096 container_alignment=4096 directory_sync=ancestors latency=operation_to_durable_completion latency_samples=8192 p50_us=8447 p95_us=9727 p99_us=11903 max_us=12817 reclaimed_allocated_bytes=0 syncs=9472 verified_objects=8192 | __refill_objects_node (13.6%), __iomap_dio_rw (6.4%), _raw_spin_unlock_irqrestore (4.5%) |
| [principles-container-write-allocating-each-512-qd4-direct](principles-container-write-allocating-each-512-qd4-direct/baseline.stdout) | PASS | write/container/allocating/each/1/4 GiB_s=0.003052 ops_s=6399.865 payload_bytes=512 logical_payload_bytes=512 record_bytes=4096 container_alignment=4096 directory_sync=ancestors latency=operation_to_durable_completion latency_samples=8192 p50_us=523 p95_us=1119 p99_us=1391 max_us=7967 reclaimed_allocated_bytes=0 syncs=49152 verified_objects=8192 | native_queued_spin_lock_slowpath (11.3%), _raw_spin_lock (10.6%), __iomap_dio_rw (8.4%) |
| [principles-container-write-allocating-batch-512-qd4-direct](principles-container-write-allocating-batch-512-qd4-direct/baseline.stdout) | PASS | write/container/allocating/batch/1/4 GiB_s=0.043891 ops_s=92046.204 payload_bytes=512 logical_payload_bytes=512 record_bytes=4096 container_alignment=4096 directory_sync=ancestors latency=operation_to_durable_completion latency_samples=8192 p50_us=1031 p95_us=1799 p99_us=4159 max_us=4964 reclaimed_allocated_bytes=0 syncs=1536 verified_objects=8192 | __iomap_dio_rw (11.4%), __jbd2_journal_file_buffer (11.4%), native_queued_spin_lock_slowpath (11.4%) |

## Baseline comparisons


These are best observations within this run, not production configuration recommendations.

## CPU hot paths

**principles-files-write-allocating-each-512-qd4-buffered syscall trace:** [summary](principles-files-write-allocating-each-512-qd4-buffered/strace/summary.txt); this separate run uses wall-clock syscall time (-w) and includes setup, population, cleanup and ptrace overhead; it is not a timed-phase latency measurement.

**principles-files-write-allocating-each-512-qd4-buffered sampled CPU:** other 68.6%, storage I/O/filesystem 23.9%, synchronization/scheduling 7.4%, BLAKE3 0.5%
[Flat](principles-files-write-allocating-each-512-qd4-buffered/perf/flat.txt), [threads](principles-files-write-allocating-each-512-qd4-buffered/perf/by_thread.txt), [call chains](principles-files-write-allocating-each-512-qd4-buffered/perf/callers.txt), source annotations under `principles-files-write-allocating-each-512-qd4-buffered/perf/annotate-*.stdout`.

**principles-files-write-allocating-batch-512-qd4-buffered syscall trace:** [summary](principles-files-write-allocating-batch-512-qd4-buffered/strace/summary.txt); this separate run uses wall-clock syscall time (-w) and includes setup, population, cleanup and ptrace overhead; it is not a timed-phase latency measurement.

**principles-files-write-allocating-batch-512-qd4-buffered sampled CPU:** other 59.3%, storage I/O/filesystem 26.8%, synchronization/scheduling 7.3%, BLAKE3 6.5%
[Flat](principles-files-write-allocating-batch-512-qd4-buffered/perf/flat.txt), [threads](principles-files-write-allocating-batch-512-qd4-buffered/perf/by_thread.txt), [call chains](principles-files-write-allocating-batch-512-qd4-buffered/perf/callers.txt), source annotations under `principles-files-write-allocating-batch-512-qd4-buffered/perf/annotate-*.stdout`.

**principles-container-write-allocating-each-512-qd4-buffered syscall trace:** [summary](principles-container-write-allocating-each-512-qd4-buffered/strace/summary.txt); this separate run uses wall-clock syscall time (-w) and includes setup, population, cleanup and ptrace overhead; it is not a timed-phase latency measurement.

**principles-container-write-allocating-each-512-qd4-buffered sampled CPU:** other 72.4%, synchronization/scheduling 14.8%, storage I/O/filesystem 10.5%, memory copies/fills 1.4%, page pinning/IOMMU 0.7%
[Flat](principles-container-write-allocating-each-512-qd4-buffered/perf/flat.txt), [threads](principles-container-write-allocating-each-512-qd4-buffered/perf/by_thread.txt), [call chains](principles-container-write-allocating-each-512-qd4-buffered/perf/callers.txt), source annotations under `principles-container-write-allocating-each-512-qd4-buffered/perf/annotate-*.stdout`.

**principles-container-write-allocating-batch-512-qd4-buffered syscall trace:** [summary](principles-container-write-allocating-batch-512-qd4-buffered/strace/summary.txt); this separate run uses wall-clock syscall time (-w) and includes setup, population, cleanup and ptrace overhead; it is not a timed-phase latency measurement.

**principles-container-write-allocating-batch-512-qd4-buffered sampled CPU:** other 60.0%, BLAKE3 16.0%, synchronization/scheduling 16.0%, storage I/O/filesystem 8.0%
[Flat](principles-container-write-allocating-batch-512-qd4-buffered/perf/flat.txt), [threads](principles-container-write-allocating-batch-512-qd4-buffered/perf/by_thread.txt), [call chains](principles-container-write-allocating-batch-512-qd4-buffered/perf/callers.txt), source annotations under `principles-container-write-allocating-batch-512-qd4-buffered/perf/annotate-*.stdout`.

**principles-files-write-allocating-each-512-qd4-direct syscall trace:** [summary](principles-files-write-allocating-each-512-qd4-direct/strace/summary.txt); this separate run uses wall-clock syscall time (-w) and includes setup, population, cleanup and ptrace overhead; it is not a timed-phase latency measurement.

**principles-files-write-allocating-each-512-qd4-direct sampled CPU:** other 65.9%, storage I/O/filesystem 22.5%, synchronization/scheduling 9.9%, page pinning/IOMMU 0.5%, memory copies/fills 0.5%, BLAKE3 0.5%
[Flat](principles-files-write-allocating-each-512-qd4-direct/perf/flat.txt), [threads](principles-files-write-allocating-each-512-qd4-direct/perf/by_thread.txt), [call chains](principles-files-write-allocating-each-512-qd4-direct/perf/callers.txt), source annotations under `principles-files-write-allocating-each-512-qd4-direct/perf/annotate-*.stdout`.

**principles-files-write-allocating-batch-512-qd4-direct syscall trace:** [summary](principles-files-write-allocating-batch-512-qd4-direct/strace/summary.txt); this separate run uses wall-clock syscall time (-w) and includes setup, population, cleanup and ptrace overhead; it is not a timed-phase latency measurement.

**principles-files-write-allocating-batch-512-qd4-direct sampled CPU:** other 61.9%, storage I/O/filesystem 18.2%, synchronization/scheduling 10.0%, page pinning/IOMMU 4.5%, BLAKE3 3.6%, memory copies/fills 1.8%
[Flat](principles-files-write-allocating-batch-512-qd4-direct/perf/flat.txt), [threads](principles-files-write-allocating-batch-512-qd4-direct/perf/by_thread.txt), [call chains](principles-files-write-allocating-batch-512-qd4-direct/perf/callers.txt), source annotations under `principles-files-write-allocating-batch-512-qd4-direct/perf/annotate-*.stdout`.

**principles-container-write-allocating-each-512-qd4-direct syscall trace:** [summary](principles-container-write-allocating-each-512-qd4-direct/strace/summary.txt); this separate run uses wall-clock syscall time (-w) and includes setup, population, cleanup and ptrace overhead; it is not a timed-phase latency measurement.

**principles-container-write-allocating-each-512-qd4-direct sampled CPU:** other 70.4%, synchronization/scheduling 19.0%, storage I/O/filesystem 8.4%, memory copies/fills 0.7%, page pinning/IOMMU 0.7%, BLAKE3 0.7%
[Flat](principles-container-write-allocating-each-512-qd4-direct/perf/flat.txt), [threads](principles-container-write-allocating-each-512-qd4-direct/perf/by_thread.txt), [call chains](principles-container-write-allocating-each-512-qd4-direct/perf/callers.txt), source annotations under `principles-container-write-allocating-each-512-qd4-direct/perf/annotate-*.stdout`.

**principles-container-write-allocating-batch-512-qd4-direct syscall trace:** [summary](principles-container-write-allocating-batch-512-qd4-direct/strace/summary.txt); this separate run uses wall-clock syscall time (-w) and includes setup, population, cleanup and ptrace overhead; it is not a timed-phase latency measurement.

**principles-container-write-allocating-batch-512-qd4-direct sampled CPU:** other 80.0%, synchronization/scheduling 8.6%, storage I/O/filesystem 5.7%, BLAKE3 5.7%
[Flat](principles-container-write-allocating-batch-512-qd4-direct/perf/flat.txt), [threads](principles-container-write-allocating-batch-512-qd4-direct/perf/by_thread.txt), [call chains](principles-container-write-allocating-batch-512-qd4-direct/perf/callers.txt), source annotations under `principles-container-write-allocating-batch-512-qd4-direct/perf/annotate-*.stdout`.


Cluster net/store/read/metadata modes were not requested: no seed supplied.

Recorded perf events: `cpu-clock`. Requested events can be narrowed by host permissions.

Raw buffered I/O is a warm bounded working-set measurement, not physical-device throughput. High QD means more synchronous workers, not asynchronous queue slots.
Raw operation-cycle latency includes benchmark loop bookkeeping and scheduling; compare throughput and latency together. Idle workers invalidate a case; max_start_us records release-to-first-operation delay.
Unresolved kernel/vDSO addresses are retained, not guessed. Time/vDSO and unresolved samples are separate categories.
Category totals are heuristic, mutually exclusive self-sample classifications above the 0.3% report threshold; inspect symbols/callers before choosing changes.
BLAKE3 reuses per-worker buffers: this measures warm-buffer CPU throughput, not disk bandwidth. Streaming omits per-buffer finalize overhead.
Raw allocating writes fill the window once then overwrite it: allocation_ops and overwrite_ops distinguish first fill from warm overwrites. They are not an append-only allocation ceiling.
Local data writes wrap within the configured bounded working set after filling it; profiles include create and replacement work. Direct local CLI without --window retains append-only writes.
Storage-principles cases are independent finite one-pass models, not production GC. Delete GiB_s counts payload represented by removed objects, not transferred bytes; p99 for release is per-container. Required syncs are timed for each/batch, and latency for every modified record reaches durable cohort completion. none is acceptance only. Logical content, physical payload, record alignment and directory barrier topology are reported separately; packed tombstones retain space until container reclamation.
Buffered storage reads are warm; compare direct-I/O cases and the separate ceiling probe before inferring device limits.
