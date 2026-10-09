# W93: NUC batching control with .efs disabled

Source diagnostic commit c227bcbd, build ID c227bcbd-batchobs.
All four servers: EFS_COMMANDS_SERVER=0; both mounts: EFS_COMMANDS=0.
TCP, direct I/O, existing cluster data retained. Eight workers, original
tiny_files.py workload with cooperative SIGUSR1 stop, fresh output directory.
Run duration 300 seconds; samples every 5 seconds and cumulative internal
counter dumps every 5 seconds. Histograms report inclusive upper-bound
quantiles; cumulative fields are independently sampled atomics. Window
endpoints may differ from loader endpoints by up to a dump interval.

The new counters classify tail mismatch behind/ahead/equal-index term,
attribute active batch endings once (rather than count subsequent rechecks),
and count every dropped inbox frame. They do not alter admission, retry,
durability or ACK policy. Enabled instrumentation overhead is uncalibrated.
A restart changes leaders and workload path distribution: this is a control
for catalog work, not a matched randomized performance A/B.

Raft and durable-store tests pass with diagnostics off and on. Existing data
is never formatted or discarded. Startup exceeded node 1's script timeout;
an idempotent retry completed startup after replay.

See analysis.json, rates.json, raw per-node observations, samples.jsonl,
disabled-environment.json, build hashes and final state for exact evidence.

Final state verified: four servers and two mounts retain .efs=0, diagnostics=0,
TCP/direct I/O. Both cold mounts agree on 32 sampled payloads (not a full
namespace verification). Raw node logs are losslessly gzip-compressed;
rollback binaries remain on NUC and are excluded from repository evidence.
Device-wide disk-intervals.json includes all users and queue time; it is not
isolated SSD firmware latency. Boundary counters and histogram fields are
independent cumulative atomics, not transactional snapshots.
