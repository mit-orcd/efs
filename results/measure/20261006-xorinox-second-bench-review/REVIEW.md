# Xorinox benchmark/report review — 2026-10-06

Run: `logs/efs-bench-20261006T230611.472138Z`, binary
`7d2de1e0812b5ebde873df29e01e709078dade929c4878150d3f8dd79b020ad3`,
commit `0c4114579dbda1a3458ce08a7e25a6c74f20fc57`. Raw I/O, I/O+BLAKE3 and
CPU BLAKE3 only; no storage-engine or daemon throughput conclusion follows.

## Reports did run

128 perf.data files; 127 cases have flat.stdout, by-thread.stdout, callers.stdout,
command JSON and stderr. These are perf report outputs, under each case's perf
subdirectory. Their unfamiliar names and lack of progress made them hard to find.
One failed workload had recorded data but no reports because the harness skipped
report generation on nonzero workload exit. Those profiles remain useful for
diagnosing the failure, even though they cannot establish representative speed.

The harness now writes flat.txt/by_thread.txt/callers.txt as well, moves reporting
after the workload matrix, runs bounded parallel case jobs (--report-jobs, default
4), shows progress, and supports --reports-only DIR without workload execution.
Source annotations share the same concurrency bound. Missing/empty/failed report
output fails validation; reports do not turn a failed workload into a pass.
Reporting permissions are checked before launching workers. Older analysis links
fall back to .stdout files. Kernel copying and page pinning/IOMMU are no longer
lumped into the heuristic other category.

## Hot paths and performance implications

127/128 cases pass. The remaining io-blake3-write-65536-qd256-buffered perf rerun
has 21 idle workers out of 256, despite all-ready sleeping startup; its baseline
has zero errors/idle workers and is separately valid. High oversubscription can
still leave workers unscheduled before a short deadline. Keep this rerun invalid;
do not fabricate participation or hide it by accepting the reported rate.

64 KiB QD1 buffered read: _copy_to_iter 77.19%, copy_page_to_iter 4.22%.
Buffered write: copy_page_from_iter_atomic 74.34%. Kernel/user copying dominates
these hot-cache workloads. Their 18.389/15.609 GiB/s rates are not NVMe bandwidth.
Changes to metadata or daemon locks will not improve this raw-copy workload.

| 1 MiB direct QD | Write GiB/s | Read GiB/s | Write avg ms | Read avg ms |
|---|---:|---:|---:|---:|
|16|6.289|6.741|2.483|2.317|
|64|6.274|6.738|9.940|9.253|
|256|6.293|6.721|39.405|36.826|

More 1 MiB workers mostly add latency after QD16. Smaller direct I/O has a
larger concurrency benefit; this is not a universal queue-depth setting.
Direct small-I/O CPU includes scheduling, page pinning and IOMMU mapping rather
than one dominant user-space function. An asynchronous registered-buffer engine
would be a separate experiment, not a justified daemon change from these results.

CPU-only 1 MiB/16 threads: 60.631 GiB/s, blake3_hash_many_avx512 98.13% sampled
CPU. Hashing is already SIMD optimized. I/O+BLAKE3 1 MiB read QD16 direct:
6.739 GiB/s, essentially the pure-I/O rate, despite hashing occupying 77.71% of
CPU samples. High CPU sample share alone does not mean hashing limits throughput.

## Verification and next measurement

Python report-harness regressions pass, including bounded concurrency, filename
aliases, report failures/empty profiles, preserving failed-workload status and
report-only execution without workload planning. Real perf report + annotations
ran successfully on three private copies on xorinox: buffered read, CPU BLAKE3,
and the previously skipped failed workload. The latter shows AVX-512 hashing
74.11% and kernel copying 14.72%, diagnostic only. Original measurements remain
unchanged. The original directory is owned by maik; the SSH account efs cannot
rewrite it. Use the recording account to regenerate all 128 report sets.

No workload was rerun. Current profiles support reporting fixes rather than
another speculative production optimization. Next run engine data/meta modes
(with the same report collection), then compare actual fragment-store/writer
and KV/fsync hot paths with raw direct-I/O/hash ceilings before porting changes
into efsd. Preserve durable/cold-read semantics and compare throughput with tail
latency, not just sample percentages.
