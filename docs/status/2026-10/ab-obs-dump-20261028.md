# AB — lock-held diagnostics, October 28–29, 2026

> **Port note (Oct 30).** Evidence ported from `agent/devel-task-ae` onto devel `7514b2145`; see [AE port](ae-product-port-20261030.md) for what was and was not ported. Its K55/K56 were renumbered **K72/K73** (devel already used K55/K56). Commit hashes, "current product" and "isolated branch" wording describe the original branch, not devel.

**IMPLEMENTED; qualification INCOMPLETE**, paired with
[AE](ae-pump-logging-20261028.md), on isolated branch `agent/devel-task-ae`.
Base `714d7e6fc`; current product `65bd82f6f` (prior frozen native gate `bb57ef69b`). Nothing is merged or
pushed. Normal-file OBS=0/1 measurements overlap; no independent throughput
speedup is demonstrated. The blocked-sink availability failure, K72, justifies
moving the periodic dump together with AE rather than adding a second reporter.

The pump captures a fixed-size numeric snapshot under `h->mu` and the outbox
owner lock, then submits after unlocking. The one AE reporter formats the
periodic `raft-obs`, I/O, KV and batch records. Term/role events retain their
synchronous path and identity. Snapshot fields read across thread ownership use
atomic diagnostic accesses. There is no new product switch or wire/store format.

Matched probes in `scripts/evidence/pump-logging/pump_logging_measure.py` measure actual mutex hold
through unlock and active cycle through the persistence/logging tail, excluding
poll sleep, on archived baseline/candidate copies only. Existing
`pump_hold_max` omits part of the old formatting/write interval. The first
controlled workload was four fixed-rate 512-byte O_TRUNC+fsync writers at five
operations/s each, 16 files/writer, plus 10 Hz stat. It is a D4-shaped subset,
not the shared kit's actual mixed-size D4 driver. Its three-run medians and
[min,max] follow; these include formation/recovery in the pump histograms,
while CPU/stat/throughput use the application window.

| Metric | Baseline OBS=0 | Candidate OBS=0 | Baseline OBS=1 | Candidate OBS=1 |
| --- | --- | --- | --- | --- |
| Active-cycle mean, µs | 228.3 [224.9,234.0] | 219.8 [217.5,221.4] | 232.7 [231.8,235.0] | 223.0 [213.2,231.2] |
| Actual hold high-water, µs | 21504 [21310,29231] | 22352 [20612,25317] | 25005 [21011,35190] | 15598 [9129,34019] |
| Mutex-wait high-water, µs | 5402 [5030,5497] | 4917 [4488,5011] | 10079 [4181,16539] | 4230 [3367,4943] |
| Daemon CPU sum, s | 10.75 [10.11,10.79] | 10.46 [10.29,10.47] | 10.34 [10.12,10.73] | 10.46 [10.41,10.82] |
| stat p99, ms | 53.419 [2.231,54.611] | 2.465 [2.150,52.798] | 2.534 [2.206,52.305] | 2.847 [1.950,50.264] |
| Writes/s | 20 [20,20] | 20 [20,20] | 20 [20,20] | 20 [20,20] |

Baseline is `714d7e6fc`, candidate `9d7e93ef2` plus the identical measurement
probes, not the later native/hardware gate binaries. Instrumented source/binary
hashes are retained with each run. Exact reproduction and all failures are in
[AE](ae-pump-logging-20261028.md). This table does not establish AB's standalone
on/off cost outside measurement spread, nor a throughput-capacity gain.

The literal shared-kit follow-up uses committed
`scripts/evidence/pump-logging/pump_logging_d4_profile.py`: unchanged
`d4_progress.Run.writer_fixed`, four writers, 16 files/writer, deterministic
mixed overwrite/truncate/in-place operations, small 1 B–8 KiB and large
1–2 MiB payloads, paced at five operations/s/writer. Every final acknowledged
version is compared exactly after quiescence; errors remain failures. A 64 KiB
fault-build memtable exercises short-run KV maintenance. Three repeats of each
baseline/candidate × OBS=0/1 remain pending. Their queue was held before its
first case when another agent's Task AK full gate appeared on intel2; an
exclusive reservation is needed to avoid contaminated timings.

Existing consumer parsing, ThreadSanitizer, native `test-all`, the retained
K48 listing failure, and real-HCA blocked-sink latency are tracked in AE. No
alpha acceptance is implied by the implementation or by a scoped PASS label.
