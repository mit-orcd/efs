# W90: five profile-guided container rounds

October 8, 2026. Benchmark-only work on `devel-bench`; xorinox_efs private root `/data1/efs/bench/w90-perf5-20261008`. Production storage, cluster services, mounts and device settings were not changed. No push.

## Comparison

All rows use the 1 GiB punch/reuse container approach: 64 KiB payload plus 4 KiB checksum tail, 95% experiment occupancy, direct I/O, 50/50 reads/replacements, four workers, batch size 32, 2 GiB data-slot capacity plus 1 GiB spare per worker. Each stage has a separate 10-second measured-phase CPU profile and two unprofiled 20-second measurements. Setup/warmup and complete final verification are excluded from timing. Required body flushes and final reclamation remain timed.

| Round | Change | Mean ops/s | vs baseline | Write p99 ms, two samples | Write max ms | Approx sampled CPU µs/op | Decision |
|---|---|---:|---:|---:|---:|---:|---|
| Baseline | Original container path | 16,165 | +0.0% | 2.479–2.495 | 501.2 | 93.1 | Reference |
| 1 | Generate 64-bit words | 16,146 | -0.1% | 2.431–2.431 | 792.5 | 59.0 | Keep: CPU reduction |
| 2 | Bounded punch batches + grouped allocation accounting | 22,216 | +37.4% | 2.527–2.575 | 302.3 | 56.7 | Keep |
| 3 | Flush directory only on namespace mutation | 22,145 | +37.0% | 2.607–2.655 | 532.0 | 57.0 | Keep: fewer redundant syscalls |
| 4 | Sort/coalesce retired ranges | 21,282 | +31.7% | 2.639–2.703 | 331.7 | 56.9 | Reject: no demonstrated speedup |
| 5 | Four-lane fill; discard Round 4; strace audit | 21,504 | +33.0% | 2.639–2.655 | 628.7 | 54.4 | Final candidate |

Means use unprofiled samples. Approximate CPU cost uses the CPU-clock profile event period divided by that profile’s completed operations; it is not a hardware-cycle count and is not obtained from the unprofiled run. CPU reductions and throughput changes must be interpreted separately. The short sequential samples are not randomized/isolated confidence intervals or endurance gates. Rare pauses may be under-sampled.

## Profile decisions

- Baseline `write_object` self CPU share was 43.90%; assembly samples identified the byte-at-a-time PRNG chain. Round 1 reduced it to 9.74% by generating full words. Changing synthetic generation bytes preserves shape and checking but is benchmark overhead optimization, not an EFS storage-engine improvement.
- Round 2 retains at most one bounded retirement batch per worker, marks slots reusable only after successful punching, and drains early for free-space progress. Per-container allocation accounting is an actual fstat pair around the batch, not inferred released bytes. Final punch work is timed even in `none`; each/batch persistence remains distinct.
- Round 3 skips directory barriers only while names remain unchanged. Container creation/recreation marks the namespace dirty. Required dirty-body flushes and file-mode directory barriers remain present. Its throughput was effectively unchanged from Round 2; this is a syscall reduction, not an independently established speedup.
- Round 4 sorted and coalesced physically adjacent retirements but did not demonstrate a gain (about 3.9% lower mean than Round 3). It was removed from the final implementation. Its profiles/results remain archived.
- Round 5 uses four independent PRNG lanes; `write_object` self CPU share is 3.98%. The dominant remaining work is BLAKE3 and XFS extent metadata. Checksums were not removed.

## Syscall audit

Separate baseline/final 5-second measured-phase traces capture all setup/verification too, but accounting uses only the begin/end markers. Traced throughput is diagnostic and excluded from the optimization table. Full compressed traces remain remote; summaries and audit scripts are retained here.

| Measured trace | Reads | Writes/retirements | Allocation-stat calls | Stat calls/retirement | Directory fsyncs | Total fsyncs | Errors |
|---|---:|---:|---:|---:|---:|---:|---:|
| baseline | 29,833 | 29,856 | 59,712 | 2.000 | 937 | 2,803 | 0 |
| round5-final | 43,856 | 44,008 | 5,508 | 0.125 | 0 | 2,754 | 0 |

Audit assertions: complete 69,632-byte reads/writes; one read/write per measured operation; physically punched bytes equal retired records × record length; record-aligned offsets and lengths inside each logical 1 GiB file; final retired backlog zero; fsync counts match benchmark counters; no measured openat/close/ioctl; all live records verify. Accounting/stat calls fell about 94%; directory fsyncs fell to zero during the unchanged-name final workload. A required body flush is not a redundant directory flush.

Trace-parser whitespace/unfinished-call handling was repaired while replaying the saved trace; this did not change the benchmark or its results. The first intended Round 1 edit was a no-op; `round1-word-fill` is retained as an identical-source control and is not counted as an optimization. `round5-vector-fill` retains the pre-rollback sort trial; `round5-final` is the chosen candidate.

## Longer eight-worker confirmation

The final check uses separate 60-second baseline/candidate intervals with perf stat hardware counters enabled only for the measured phase. Total process elapsed time printed by perf includes setup and verification; use benchmark wall_s for rates. Capacity scales to 16 GiB data plus 8 GiB spare.

| Version | Ops/s | Write p99 ms | Write max ms |
|---|---:|---:|---:|
| baseline | 31,127 | 2.927 | 137.3 |
| round5-final | 35,803 | 3.423 | 233.6 |

## Hardware counters and validation

| Version | CPU µs/op | Cycles/op | Instructions/op |
|---|---:|---:|---:|
| baseline | 94.0 | 444,256 | 709,461 |
| round5-final | 56.3 | 255,033 | 275,201 |

Counters were controlled around the measured phase and normalized by actual completed operations. CPU time/op fell about 40%, cycles/op about 42%, and instructions/op about 61%. CPU frequency differs between samples; CPU-clock and cycle reductions are not interchangeable.

Final benchmark/daemon builds completed without compiler warnings. All 47 targeted unittest methods plus the benchmark CLI gate passed, including bounded-retirement backpressure, physical punch failure and perf-control failure. Every profile reports zero lost samples. Archived source hashes match every profile manifest; the final archive matches the committed source. Scratch is empty. Documentation validation is recorded separately.

Allocation-accounting granularity changed: released_allocated_bytes sums positive per-container batch block differences rather than individual-punch differences. Metadata allocation fluctuations can make these counters differ. Final allocated_before/after snapshots remain actual st_blocks measurements; neither counter is NAND write amplification.

## Scope and remaining work

The final candidate keeps the simpler unordered punch batch, optimized synthetic fill and namespace dirty tracking. It does not implement a durable index, replay, distributed ownership, reader pins or production storage migration. All workers own separate models/containers; same-range access conclusions remain in the earlier [range ledger](../20261008-w90-scaling-ranges/README.md). Do not combine ratios from different campaigns into a claimed production speedup. No GPU/RDMA/network throughput was measured.

Exact commands, raw output, source and executable identities, profile reports and source archives accompany every round. Raw perf.data, executable snapshots and full traces remain on xorinox under the benchmark root. Successful fixtures are cleaned; final validation logs are retained alongside the evidence.
