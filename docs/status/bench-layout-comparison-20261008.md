# W90 · Matched storage simulator comparisons

Plan recorded before implementation, October 8, 2026, on `devel-bench`.

`efs-bench` remains an optimized I/O storage simulator. The goal is to compare
file-per-record and container layouts under configurable workloads resembling
efsd, not implement another production engine. Production recovery, durable
indexes, Raft, FUSE and deployment are outside this milestone.

## Implementation plan

Extend the existing churn simulator and runner rather than create another engine:

- Match payload size, occupancy, worker count, read/replacement mix, warmup,
  operation budget and persistence policy across layouts. Replacements retire
  one old version; independent create/delete lifetime distributions are not modeled.
- Add explicit seed and hot-set locality controls. Fixed-operation runs replay
  the same per-worker logical object/operation sequence for each layout, limited
  to their common live-object count. Timed runs share the generator but can
  execute different-length prefixes.
- Accept multiple private storage roots. Assign workers round-robin, preserving
  worker-local models; report each target's work and measured elapsed time.
  Multiple directories on one device do not establish multi-device scaling.
- Add a comparison mode that alternates layout order across repeats and reports
  sample range, median throughput, read/write p99, allocation and CPU per operation.
  Keep raw commands, source identities and integrity checks with every result.

## Acceptance and limits

Validate CLI bounds, matched ordering/geometry and locality, multi-root cleanup,
full payload/version verification and failure handling. Run a small functional
comparison on xorinox using private directories under `/data1/efs/bench`, then
retain reproducible evidence. Short functional runs are not a layout winner gate.

For performance decisions, repeat longer matched runs on the same devices with
representative sizes, locality, occupancy, persistence and target counts. Compare
throughput together with tail latency, allocation and reclamation backlog. The
conclusion applies to the simulator workload and devices, not automatically to
efsd. `none` measures unsynced acceptance; `each`/`batch` include selected physical
barriers but no durable object map. No global cache drops or host changes.

Status: implemented and validated. Eleven storage test methods and eight runner
test methods pass, including mismatch rejection and multi-target cleanup. Twelve
small fixed-step comparison cases and four short 1 GiB timed cases pass on
xorinox. The two roots share one NVMe; physical multi-device scaling and longer
performance acceptance remain open. See the [evidence ledger](../../results/measure/20261008-w90-layout-comparison/README.md)
and [user invocation](../how-it-works/storage-churn.md#matched-simulator-comparison).

Fixed-step runs require identical trace fingerprints, read/write counts and live
counts across layouts. Live counts now use common container-tail rounding;
historical file-only runs could have a slightly larger live set. CPU is measured
per worker during I/O and final required barriers. Timed hard-cap validation is
performed before final flushing so a slow final barrier cannot mask early cap
exhaustion. `COMPARISON.md` reports sample medians/ranges and target metrics;
`results.json` retains the detailed verification/reclamation counters.
