# October 7 benchmark hot-path review

Reviewed `logs/efs-bench-20261007T001758.671828Z`: 502 cases,
498 PASS, three failed profiled runs and one failed baseline. All perf reports
were generated. Measurements ran on xorinox, Ryzen 9 7940HS (8 cores/16 threads),
with AVX512 BLAKE3 and two XFS locations on different NVMe devices.
Original benchmark source: `06e02a64`; binary SHA256:
`f0ee6aa6eb85bc36297a8371c990ec504d7ce25448c676d24fb9f9bf7847b7af`.

## Findings and changes

1. Cached KV segment gets linearly scanned records, repeatedly comparing keys.
   Each resident block now has validated record offsets and binary search.
   Offset arrays are freed on eviction/close, within the existing 256-slot cache.
   The segment format and durability are unchanged. Malformed/truncated records,
   invalid operations, payload-bearing tombstones, unsorted keys and inconsistent
   block boundaries are rejected rather than yielding false misses.
2. QD256 raw workers contended on one release mutex. Failed cases had idle
   workers, not failed I/O/checksums. Private release gates retain the common
   readiness barrier/deadline and strict idle-worker failure. `max_start_us`
   exposes scheduling delay. Counters distinguish first allocation from repeated
   bounded-window overwrites, including preallocated cases.
3. Invalid profiled workloads were presented beside usable CPU profiles without
   clear qualification. Analysis now labels these diagnostic-only, while retaining
   a valid baseline. All 72 engine-data profiles contained impossible caller
   addresses (including ASCII path bytes interpreted as return addresses).
   CPU-address-width-aware warnings expose this; auto call-graph selection now
   uses DWARF for engine data. Flat self-samples from old recordings remain useful;
   broken caller chains cannot be reconstructed.
4. Metadata benchmarks ignored flush, compact and stats failures; accepted
   incorrect KV values; lost earlier phase errors; and reported recovery success
   without checking the recovered index/term. These now invalidate results and
   accumulate into the exit status. Every successfully begun Raft batch is ended,
   even after append failure; unsuccessful batches do not contribute successful
   batch throughput. Error/zero-operation data and metadata phases print failure.
   An inaccessible scratch root is no longer classified as absent.

## Matched evidence

`meta-ab.json` and `meta-{before,after}-{0,1}.stdout` retain two unprofiled
three-second repeats per binary on the same scratch location:

| Cached KV reads | Before, ops/s | After, ops/s |
|---|---:|---:|
| Repeat 0 | 3,918,617.4 | 4,926,051.4 |
| Repeat 1 | 3,885,651.7 | 4,822,527.2 |

These matched measurements isolate the lookup/gate changes before the final
metadata-validation additions. Two final-binary validation runs also passed
(`meta-final-{0,1}.stdout`), with 6.60 and 6.45 million reads/s; they were not
interleaved with fresh old-binary runs and are not a matched speedup estimate.

Mean improvement in the matched runs is about 25%; this is warm local KV reads, not a claim about
whole-cluster throughput. Separate flat perf samples reduced `memcmp` self share
from 23.78% to 13.63%. Relative CPU shares alone do not establish elapsed speedup.

The original binary reproduced 67 idle workers in one of two QD256 repeats.
Four private-gate repeats had zero idle workers, but variable throughput
(48.1–67.3 GiB/s) and start delays (212–733 ms). No raw I/O speedup is claimed.
The four previously failing configurations passed reruns with actual perf-control
FIFOs and zero idle workers; `io-perf-rechecks.json` and case folders retain
output, commands, flat/thread/caller reports and annotations.

`dwarf-recheck/` retains two engine-data cases using the new automatic DWARF
selection. Baselines, profiled runs and reports pass; caller reports contain no
noncanonical addresses. This fixes the observed problem, not every possible
stack-unwinding limitation. Raw perf recordings and binaries remain in private
remote scratch; only compact text/JSON evidence is committed.

## Other hot paths and interpretation

BLAKE3 profiles already use AVX512 and are dominated by SIMD hashing as expected;
no hash rewrite is justified. Buffered I/O profiles are dominated by kernel
copies over warm bounded windows, not direct device bandwidth. Root2 metadata
sync latency (~4.3 ms) exceeds root1 (~0.5 ms); the roots differ in device and
XFS log geometry. No sync weakening or unmeasured filesystem tuning was applied.

Writer2 performs poorly in some engine-data cases, but their profiles combine
write/read phases and path counts; read hashing dominates CPU samples. These
recordings do not establish the write-side waiting cause, so writer defaults
remain unchanged. Isolated write-side latency/wait measurements are needed
before choosing a new production writer policy.

## Validation

- Full Linux unit suite passes (`unit.log`), including KV LSM and the new segment
  offset/corruption/eviction regression.
- Benchmark CLI/backend smoke passes after final changes (`cli-final.log`).
  Real `fsync`/`fdatasync` failure injection produces a failed metadata run.
- Profiler unit tests: 15 pass (`profile-unit.log`), including automatic unwinding,
  malformed-address warnings and invalid-result labeling; also pass locally.
- Targeted real perf reruns pass; no production cluster deployment/restart.
- Documentation generated and its five checks pass.
