# W93 persistence-efficiency evidence — 2026-10-09

NUC only; preserved stores, direct I/O/TCP, `.efs` disabled, receiver flow control
enabled in all cells. Frozen production build `39be3b38`; `24865a2b` adds only
broader scheduler fixture coverage after that freeze. See `versions.json` for
exact executable SHA-256 values. Each cell records process identity and flags.

Four sequential 240-second exploratory cells: baseline, no-op continuation,
both experiments, bounded group scheduling. Eight workers and seed 970001,
fresh output directories, actual 1–512-byte payloads. SIGUSR1 drains the loader;
all hosted replicas must apply a sampled leader commit cut before restart.
There is one run per cell: ordering, background GC, placement, growing retained
data and drive state are confounders. These are not performance acceptance runs.

`rates.json` has three 80-second rates. `latency-summary.json` aggregates worker
histograms in 100-second bins, including final drain; percentile values are bin
upper bounds. `analysis.json` measures node counters and phase timings.
`efficiency-summary.py` derives command-tag deltas, sync cohort sizes and phase
sums. Append counters count physical new-entry requests on each replica,
excluding rotation copies; they are not unique committed client operations.
Do not sum replicated nodes to count logical operations. Phase samples are pump
cycles, not counts of physical KV/marker fsyncs.

`cold-verification.json` checks 32 independent seeded exact payloads in every
cell through both cold mounts: 256 checks after a full service restart.
`gates.log`, `posix*`, `peer*` retain full service correctness gate evidence.
`fault-live.log` and `fault-exit.txt` retain private sync/append/recovery gates;
`fault-restored/start.log` records preserved-service restoration.

`barrier-census.py`, if executed after those phases, captures actual Raft/KV/
applied-marker sync syscalls for 30 seconds during a separate fresh tiny-file
workload. FD identities are checked before/after, lost events reject the census,
and loader writes drain normally. Its rates include profiling overhead and must
not be compared against untraced factor cases. Raw perf data remains on NUC;
the compressed decoded events and exact command are retained here.

Earlier smoke/partition and injected KV EIO positive/negative-control evidence
is retained in sibling `../20261009-w93-batch-hour/`. That initial performance
controller was stopped before population when scheduling eligibility was
tightened; its intended factor method is superseded by this directory.
