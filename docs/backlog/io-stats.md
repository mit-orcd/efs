# io-stats — data-plane counters

[Queue](../status/README.md) · [Operations](../operations/operations.md)

Source-reviewed Oct 7 (`src/server/iostats.c`, handler/writer paths,
`efs-mgmt io-stats`, cluster portal parser). The
[Oct 4 development-cluster implementation and measurements](../archive/io-stats-acceptance-20261004.md)
are retained separately; they are not acceptance of a later deployment.

`efs-mgmt io-stats <node:port>` reads process-global GET, PUT and disk-write
classes without resetting them. The metadata pump also emits `iostats:` text
approximately every five seconds after activity has begun. Counters are
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
