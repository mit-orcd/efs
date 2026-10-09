# Fresh four-node profile, 04:49 UTC October 9

Build 64cdf183d265, direct I/O/TCP, .efs enabled. Existing user loader active:
tiny-clock-64cads, 80,000-file target, eight workers, depth eight, 1,000 files
per folder. No code, service, generator or configuration changes for capture.
Simultaneous 30-second 199 Hz perf across all production daemons: 6,761 samples,
zero lost. CPU counters bracket 31.317 seconds including capture overhead.
Filtered reports use per-PID relative percentages; inclusive paths overlap.

| Node | CPU, one core=100% | Main inclusive paths |
| --- | ---: | --- |
| n1 | 10.3% | TCP message send 23.59%, request handling 21.55% |
| n2 | 33.8% | catalog 38.44%, add_node 17.67%, page-fault entry 14.37% |
| n3 | 22.0% | catalog 50.54%, add_node 21.99%, page-fault entry 18.16% |
| n4 | 8.0% | fragment write 24.86%, Raft pump 22.17%, fsync 10.34% |

Clock reads are below the 0.5% inclusive report threshold on n2/n3, so the
previous batching improvement persists. No new speedup inferred from the
lower total CPU: publishers now encounter NOMEM and retry with backoff.

Last node-2 successful publication: generation 140, 04:48:57 UTC, 1,138,552
files, 1,069,076,168 accounted bytes, 267 ms build. This leaves only about
4.45 MiB below the 1 GiB cap. Subsequent refreshes return rc=-2 (EFS_ERR_NOMEM),
including a 322 ms failed attempt at 04:49:48. NOMEM can indicate either the
explicit accounting limit or allocator failure; proximity to the unchanged
cap under continuing growth strongly supports cap pressure. No allocation-site
trace was captured. Live stats truthfully reports generation 140, an older
snapshot and refresh error -2. Disk free space remains ample.

Priority: reduce immutable catalog memory per inode and repeated global
inode reconstruction, preserving lane invalidation, snapshot lifetimes,
bindings, rollups, permissions and accounting. Sharing partition-local decoded
rows/stats across generations addresses unchanged-row work and allocation
churn, but per-catalog cap accounting must remain truthful; sharing alone does
not necessarily lower the logical memory required for one full catalog.
Compact representation may also be needed. Avoid treating a larger cap as the
performance fix. Retry admission/backoff may avoid repeated predictably doomed
work, but cannot make snapshots current by itself.

For CREATE throughput, TCP/replication and synchronous fragment writes remain
visible; this on-CPU profile cannot establish blocked I/O latency or physical
storage saturation. Raw perf, all reports, counters, identity, query logs and
live stats retained here. No fix/deployment made in this run.
