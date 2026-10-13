# W114: storage scaling and overlapping ranges — October 8, 2026

**Scope:** independent benchmark work on `devel-bench`, xorinox_efs private
`/data1/efs/bench/w90-20261008` fixtures. No production EFS backend, services,
network transport, global cache/mount/device policy or shared `devel` source
was changed. This is not an RDMA/GPU or cluster measurement.

**Implementation:** timed churn with hard work bounds, parallel preparation,
post-run FIEMAP accounting, and bounded deferred file-body batching. New
`--bench ranges` / `--storage-ranges` tests one logical 1 GiB file under
separate-range ownership, shared immutable reads, a coordinated hot range,
and explicitly unsafe overlap diagnostics. Raw read/write/flush maxima help
separate lock waits from I/O delays. Source identities and earlier measurement
epochs are retained, rather than applying final code to historical numbers.

**Evidence:** [complete measurements and validation](../../results/measure/20261008-w90-scaling-ranges/README.md).
Earlier [35-case measurements](../../results/measure/20261008-w90-xorinox/README.md)
used immediate file-body flushing and remain historical reference evidence.

**Interpretation:** file flush scheduling materially changes the baseline.
Compare the final deferred-body file measurements before attributing a gap to
containers. Longer 64 KiB churn remains a bounded local model with volatile
index and worker-local reclamation, not endurance or recovery acceptance.
Filestreams are a measured variation, not a global recommendation.

**Same range:** safe variants verify expected versions and checksums. Unlocked
split/punch updates expose mixed records; locked variants prevent them. Ordinary
single-write diagnostic samples do not establish or contradict an atomic-write
guarantee. The unsafe checker cannot detect a whole stale version or lost update.
Same-range direct mixed reads/writes also show a strong persistence-policy
interaction; read syscall maxima expose the delay, but kernel/device root cause
is unresolved. Do not turn this observation into a global sync-policy change.

**Next production gate:** retain immutable published locations, conditional map
publication, reader pins through I/O completion, allocation-generation fencing
and authoritative retirement before punch/reuse. The
[candidate recovery sketch](../backlog/storage-engine-v2.md#candidate-range-ownership-and-index-recovery-plan)
is a proposal, not implemented distributed authority. A durable index, bounded
replay and crash/fault/restart gates are required before backend integration.
W114 evidence does not close W44, W71, W75 or W87.

**Operator documentation:** [churn](../how-it-works/storage-churn.md),
[shared ranges](../how-it-works/storage-ranges.md). W114 remains open for broader
lifetime/workload distributions and production acceptance; this laboratory
follow-up does not promote a storage format.

Numbering correction (Oct 12): this benchmark is W114. Historical `w90-*`
fixture and evidence paths retain their names; W90 denotes scratch expiry.
