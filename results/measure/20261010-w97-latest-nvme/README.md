# W97 latest merged NUC slowdown / NVMe observation — Oct 10

Captured all 16 branch tips; all are ancestors of frozen engine
`6b8f05acb484b52284a7ffdd6c1da7b0476f35f6` (`6b8f05acb484-w97`).
Other checkouts and branch pointers are preserved. The new devel/query commits
add retained CREATE routing, durable command policy and virtual FORGET recovery;
this experiment does not change production persistence or SSD settings.

Full native build/tests and private direct/TCP POSIX/peer/sync/APPEND/dirty-stop/
shutdown/WAL/cold-GC gates precede deployment to preserved NUC stores.
The first private gate refuses before execution because its new mount paths do
not exist. Its failure is retained; create empty directories
`/data1/efs/w97-latest-test-mnt` and `...-mnt2` before rerunning.
No service binary is replaced until final native/live exit records are zero.

Continuous measurement uses one fresh tree and eight balanced workers, actual
1–512-byte payloads, original CREATE/write/CLOSE semantics, 128k-file cap per
worker, 80 GiB volume and 3 GiB available-RAM guards. Same source/configuration:
FLOW=1, 10 ms, TCP/direct IO, commands off, group drain / early retirement /
overlap / no-op off. Batch diagnostics are on during measurement.
Twenty minutes writing, ten minutes paused, twenty minutes resumed **without
restart or replacement directories**. SIGUSR2 pauses before new operations;
in-flight operations finish. Progress snapshots serialize count/active updates.
The controller requires zero active workers and unchanged completed counts
through the drained pause, within a 180-second drain allowance. A pause can
therefore include a small measured drain tail; exact boundaries are retained.

Retain five-minute rates per group, per-window durable cohort/sync/marker and
replication-pressure counters/histograms, host block read/write/flush deltas,
RSS/PSI/CPU/hwmon, immutable read probes, final errors/caps, replica catch-up and
128 exact first/last payload checks through both cold mounts. Perf tracepoint
counts retain explicit enable/disable acknowledgements. Loader/probes and
observer volume are bounded; diagnostics affect measurements and are not an
uninstrumented production capacity claim.

NVMe is Samsung 970 EVO 1 TB firmware 2B2QEXE7, XFS over LVM shared with root /
home / swap. n3/n4 use that volume; n1/n2 use XFS on the SATA SSD. Linux reports
NVMe write-back versus SATA write-through. Do not confuse controller volatile
write cache, NAND TurboWrite buffering or Linux page cache. Samsung documents
42 GB maximum TurboWrite buffering for this model family/capacity and also
lower post-buffer performance; those bulk specifications do not establish
our small synchronous-write latency or actual buffer occupancy.
[Samsung product announcement](https://semiconductor.samsung.com/news-events/news/samsung-electronics-sets-new-performance-standards-for-nvme-ssds-with-970-pro-and-evo/)
[Samsung 970 EVO datasheet](https://image.semiconductor.samsung.com/resources/data-sheet/Samsung-NVMe-SSD-970-EVO-Data-Sheet_Rev.1.0.pdf).

SMART/id-ctrl access is denied to efs and no NVMe hwmon sensor is exposed.
Optional root script `capture-nvme-health.sh` reads identity, current cache/power/
APST features and SMART every 15 seconds; it changes no settings and exits when
`STOP-NVME` appears, or after two hours. If unavailable, no SSD temperature /
thermal-throttle or cache-occupancy conclusion is supported. A rate rebound
after rest would be consistent with several explanations, including cleared
replication/GC/compaction queues; it cannot alone prove NAND cache exhaustion.
Host write bytes are not NAND write amplification or direct cache occupancy.

Remote source: `/data1/efs/w97-latest-build`; evidence:
`/data1/efs/w97-latest-20261010`. After cooperative loader drain and all-replica
leader-cut catch-up, restore normal diagnostics-off defaults and both mounts,
check exact sampled cold bytes and active executable identities. Do not wipe
existing stores or force-discard pending writes. Acceptance/results pending.
