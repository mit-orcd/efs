# W97 corrected-build sustained comparisons

Source ec5b53b3; uniform ec5b53b3-flow binaries. Same preserved NUC stores,
TCP/direct I/O, commands disabled, eight seeded tiny-file workers, fresh
output directory for every trial. Three 15-minute feature-off/on pairs,
ordered b1/c1/c2/b2/b3/c3; EFS_BATCH_OBS=1 throughout. No build, profile or
functional suite overlaps the timed workloads.

Per-file open/create/write/close latency uses worker-local histograms; folder
creation and work-queue delay are excluded. Bin quantiles are upper bounds,
not exact percentiles. Progress counters lag worker flushes. A five-second
observer records disk/process data and a 100-second Raft observer records
leadership/lag. Both modes incur the same observer costs.

SIGUSR1 cooperatively drains each loader. The controller requires all hosted
replicas to apply the sampled leader commit cut before the next restart.
This is not an atomic multi-group snapshot. Raw evidence and results are
written remotely to /data1/efs/w97-flow-repeats-v2-20261009.

No performance acceptance result is claimed until these trials finish.
The earlier repeats directory belongs to an interrupted baseline, excluded
because its build lacked the corrected probe timestamp fencing.
