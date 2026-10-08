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


## Focused longer comparison

[Eight 180-second cases](../../results/measure/20261008-w90-focused-comparison/README.md)
finished in 27 minutes 35 seconds. Four alternating repeats, 95% occupancy,
four workers, 1 GiB containers and hot-set locality show median punch throughput
22,123 versus files 12,157 ops/s (+82%); median write p99 2.591 versus 6.143 ms.
Every repeat favors punch (1.81–1.83× throughput), with essentially equal final
allocation and 22% lower worker CPU/op. All 24.6 million operations and final
verification pass, dead backlog is zero, and scratch is clean. Rare write stalls
remain in both layouts (worst 533 ms punch, 946 ms files).

This is sufficient repeatability evidence to favor punch for the tested simulator
workload. Broader workload/device coverage and production integration remain
separate; two roots on one NVMe do not demonstrate physical target scaling.
The initially scheduled broad matrix was deliberately stopped in favor of this
focused comparison and is excluded from the performance conclusion.


## Integration branch prepared

`devel-storage-engine` starts from committed `devel` at `6a38d4a8` and merges
`devel-bench` at `5071867c`, preserving simulator history and recorded evidence.
The separate worktree leaves the other developer's checkout and uncommitted work
untouched. Work-item conflict resolution retains both W92 and W90; generated
architecture pages are rebuilt from the reconciled sources. Two inherited
directory links now explicitly reference their existing evidence README files.

Production container storage and an engine selector are not implemented by this
merge. They will be developed on this branch. Incorporate subsequent committed
`devel` changes here, validate, then merge only `devel-storage-engine` into
`devel`; its ancestry already includes the benchmark commits. No push or merge
back into `devel` is performed during branch preparation.

Merged-source validation: warning-free Linux `efsd`/`efs-bench` build; 11 churn
and six shared-range test methods; eight churn-runner and three range-runner
methods; CLI ownership/scratch checks and all five documentation gates pass.
This is integration validation, not production container-backend acceptance.
Remote build/test logs: `/data1/efs/bench/storage-engine-integration-20261008`.
