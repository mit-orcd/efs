# W93 private quota-pressure observation

The first 4,096-file mixed-load fixture used the normal 1 GiB/node gate quota
and failed ENOSPC while overwrites outpaced bounded GC. This is a failed run,
excluded from latency comparisons. Preserved private stores were observed for
70 seconds without mounting or changing installed service. Pending work drained
to zero, reported usable used bytes fell to 261.29 MiB, and GC delete/sweep/reap/
ACK errors remained zero. The attached samples retain the exact observation.
This does not establish that production backpressure is sufficient. Subsequent
long timing runs explicitly use 4 GiB/node and include physical reclamation.
