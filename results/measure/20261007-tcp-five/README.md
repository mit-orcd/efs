# Five TCP package acceptance — NUC, Oct 7, 2026

Private four-node loopback TCP fixtures, SATA `/data1/efs` and NVMe
`/data2/efs` (the latter resolves through home). Shared cluster stores unchanged.
Baseline commit `cd0cc4c5`; source and binary hashes are in each manifest.

`tcp-five-main.log`: root rename/mixed reads, namespace and GC/restart controls.
`tcp-five-depth.log`: depth143 acceptance, depth144 bounded refusal, 64-lane
rmdir/replacement and cold restart. `tcp-five-shutdown-{direct,buffered}-{1,2,3}`:
writer-active-at-signal, acknowledged bytes after cold restart, physical GC,
late PUT fencing and zero final quota. First weaker shutdown attempt is
superseded by these six repeats and is not used as active-at-signal proof.

Perf and strace are separate runs against the owned main fixture. Perf captures
59344 samples with zero loss. Flat, by-thread and caller reports are retained;
full `perf.data` remains `/data1/efs/tcp-five-gc.data` on NUC. The trace includes
fsync/fdatasync from all four private daemons, with profiling overhead; it is
not used as an untraced latency benchmark. Detailed reap timings separate scan,
lane sweep, all-member death fence/inventory and REAP_DONE.

Initial reaping capture overlaps startup recovery and is retained separately;
settled serial/candidate captures wait120 seconds on four-node fixtures.
The compile-only transaction barrier is used solely by the coordinator-death
fixture; normal service builds do not include it.

See [the status ledger](../../../docs/status/tcp-five-gates-20261007.md).
