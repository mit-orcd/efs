# io-stats — data-plane counters

[Queue](../status/README.md) · [Operations](../operations/operations.md)

Source-reviewed Oct 7 (`src/server/iostats.c`, handler/writer paths,
`efs-mgmt io-stats`, cluster portal parser). The
[Oct 4 development-cluster implementation and measurements](../archive/io-stats-acceptance-20261004.md)
are retained separately; they are not acceptance of a later deployment.

`efs-mgmt io-stats <node:port>` reads process-global GET, PUT, disk-write, disk-read and GC-delete
classes without resetting them. The metadata pump also emits `iostats:` text
for the original GET/PUT/disk-write classes approximately every five seconds
after activity has begun. Counters are
relaxed-atomic diagnostic snapshots, not a coherent transaction or audit log.

- `ops`, `bytes`, `errors`, `us_sum` and maximum latency accumulate until
  daemon restart. Handler bytes count successful payloads; handler errors
  reflect operation status, not complete coverage of malformed requests or
  reply-send failures. Disk-write timing covers the writer job, not just media
  service or a verified persistence barrier.
- `avg_us` is cumulative `us_sum / ops`. `p50_us` estimates the last 64
  samples; it is neither a lifetime median nor an exact interval percentile.
- `uptime_s` begins at the first instrumented operation, not process launch.
  Treat it and counter deltas as diagnostics, not a unique process identity.
- The cluster portal diffs counts/bytes/latency sums for rates and interval
  averages; negative deltas are skipped. This does not prove every fast
  restart is detected if new counters already exceed the prior sample.

The implementation uses no hot-path allocation or mutex in the counter helper.
Clock calls/atomic updates still cost work. Oct 4 VM repeats showed substantial
variance; they do not establish a universal <0.1% overhead or isolate the
cause of a slower repeat. Use matched untraced repetitions for acceptance.

There is no per-export/path split, durable time-series history or client-side
coverage here. GC diagnostics and `EFS_RPC_PROF` are separate mechanisms.

Oct 7 follow-up adds filesystem-backed fragment reads and GC delete RPCs. A
GC request may find an already absent fragment or return bounded-progress BUSY;
it is not a count of physical removals. The portal labels these `rdsk` and `del`,
with `ddsk` derived from GC's actual removed-fragment/payload counters. These
application operations include page-cache service; they are not device IOPS.
The extended I/O reply keeps the original three classes as a prefix; new mgmt
accepts either length, while old mgmt needs upgrading for the five-class reply.
A private NUC RPC test (`tests/live/io_stats_smoke.py`) checks successful/missing
reads, duplicate GC requests and exactly one actual removal. Full NUC unit tests
pass. This has not yet been rolled into the running xorinox daemons.

`efs-mgmt status` now refreshes each reachable node's usage/quota from its STATUS
reply before calculating logical capacity, matching FUSE statfs rather than
cached membership usage. The portal independently samples the mounted export's
statfs through `df`, so this chart fix also works with the existing daemons.
