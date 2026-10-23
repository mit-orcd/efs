# NUC durability batching rerun with kernel tracepoints — 2026-10-09

## Result

Fresh SSH access can read syscall tracepoints and attach perf to the existing
daemons. No cluster restart, deployment, production code or durability setting
change was required. This five-minute recording uses no strace and confirms
expensive NVMe persistence with modest typical WAL batching.

| Node / backing | Interior successful WAL syncs | Entries/sync mean | Single-entry syncs | Mean / p95 WAL sync | Approximate WAL-call reduction vs one call/entry |
|---|---:|---:|---:|---:|---:|
| n1 / SATA | 12,600 | 1.000 | 100.0% | 0.132 / 0.221 ms | 0% |
| n2 / SATA | 30,846 | 1.269 | 84.4% | 0.153 / 0.278 ms | 21.2% |
| n3 / NVMe | 24,456 | 1.596 | 78.0% | 7.803 / 13.884 ms | 37.3% |
| n4 / NVMe | 18,005 | 1.474 | 68.9% | 7.403 / 13.239 ms | 32.2% |

All nodes have median one entry per sync; n3/n4 p95 is three. n3 occasionally
has larger bursts (maximum 92 entries), so the previous short strace windows
missed part of the tail. This explains why continuous recording improves its
observed mean; it does not establish a tracing-overhead effect. All figures
include foreground and background traffic, not only newly created files.

| 100-second interval | Files/s | n3 entries/sync | n3 mean sync | n4 entries/sync | n4 mean sync | NVMe busy | NVMe flush await |
|---|---:|---:|---:|---:|---:|---:|---:|
| 0–100 | 33.18 | 1.756 | 8.082 ms | 1.470 | 7.479 ms | 89.53% | 4.14 ms |
| 100–200 | 22.26 | 1.414 | 7.413 ms | 1.491 | 7.277 ms | 92.15% | 4.03 ms |
| 200–300 | 27.10 | 1.588 | 7.855 ms | 1.461 | 7.462 ms | 90.86% | 4.11 ms |

NVMe write await is 4.54–5.20 ms with only 6–8 MiB/s writes; SATA write await
is about 0.22 ms. No progressively worsening sync latency appears within this
already-warm capture. The lower-overhead rerun supports the earlier persistence
limiter finding, not an explanation for the startup-to-steady transition or
a hardware-ceiling assertion.

## Capture and validity

Unchanged deployed build `800122c5e86d`, daemon PIDs 2312683/2314230/2315840/
2317774, TCP/direct I/O. UTC 06:01:53–06:06:53; 61 counter samples, 300 seconds,
1,699,265 recorded events. Zero lost events reported by perf record and
`perf script --show-lost-events`; no completed traced syscall errors, short WAL
writes or record-header verification failures. Recorder overhead near the end
is 4.81% of one core (`recorder-overhead.json`). Recording writes approximately
146 MiB to SATA over five minutes, so this is lower overhead, not zero overhead.

`command.json` records the exact perf invocation: enter/exit pwrite64,
fdatasync and fsync on all four daemons, with pwrite entry FD filtering. Exit
pwrite events are deliberately unfiltered and include unrelated fragment/KV
writes; 173,775 unmatched exits are ignored, not lost events. Three unmatched
fdatasync exits, one fsync exit and four incomplete entries are capture
boundaries. `script.log` contains perf's generic warnings that the trace field
does not apply to other event classes; this recording contains syscall events.

`capture.py` records process/disk counters and retains read-only handles to the
original WAL inodes. `extend_headers.py` verifies the same inode and first 13
header bytes, then reads up to 35 bytes to identify command tags. End identities
match initial descriptors/inodes: no observed WAL rotation. The parser verifies
framing length and successful full writes; it does not claim CRC or committed
state validation. No command paths or user file payloads are read into the
header evidence.

`analyze.py` pairs kernel entry/exit events per TGID/TID and reconstructs
successful WAL-write cohorts before each sync start. It excludes the first
barrier per node (unknown pre-attach history) and incomplete end calls. Writes
completed during a sync are assigned conservatively to the next sync, consistent
with the code's captured-byte watermark. These remain syscall-boundary estimates,
not direct telemetry of `d->bytes`/`cover`. Tiny capture-to-entry races can shift
adjacent cohorts; n2's 41 zero-assigned-entry barriers are not proof of unnecessary
syncs. Physical-device flushes can coalesce across writers, so WAL calls are not
physical flush counts. Approximate call reduction is arithmetic, not an A/B gain.

Raw original `/data1/efs/evidence/durability-tracepoints-20261009/perf.data` is retained
on NUC (153,143,171 bytes), SHA256
`839c83a6152c1f7360f8b49ccfc7a49b276cd8b7ac4ebc104b76d9ff8850f01c`.
The repository retains the full compressed decoded event stream
`perf-script.txt.gz`, verified `wal-headers.json`, all counters and exact identity,
plus `cohorts.jsonl.gz`, distributions in `analysis.json` and disk/rate windows
in `windows.json`. Reproduce with `python3 analyze.py`.

## Workload, command mix and correction

Reuse generator 2519952, `/data1/efs/mnt/tiny-batching-20261009`, eight workers,
depth up to 8, up to 1000 files/folder, 1–512-byte payloads, bounded target
50,000 files. During capture it advances from 26,274 to 34,528 reported files
(generator elapsed 716.1–1016.2 seconds). Batched progress counters are approximate
at interval boundaries. No second loader or ptrace tracer runs concurrently.
This is a later segment of the same workload, not an identical startup A/B.

Verified n2 WAL command counts include CREATE 8,351, PUBLISH 8,335, LEASE_OPEN
8,334 and LEASE_CLOSE 8,337. GC/reap commands LANE_SWEEP_BATCH, REAP_DONE and
GC_ACK account for 5,657 entries (14.45% of n2 traffic), with similar n3 share.
Different groups/replication placements contribute different counts; commands
are not uniquely identifiable foreground requests. The traffic shows multiple
metadata stages plus background reclamation. It is not valid to equate one
entry, one barrier, or all recorded WAL traffic with one newly created file.

The preceding collector's SIGINT did **not** stop scheduling: nohup/background
launch inherited SIGINT ignored. Earlier statements that it was merely draining
were incorrect and are corrected in that report. The rerun preserves the
existing loader, which continues naturally to its bounded target; no forced
kill, restart or data removal. The archived prior collector is corrected to
restore SIGINT before spawning future generators. Its previous raw measurement
is unchanged. Actual completion is recorded remotely in the original collector's
`loader-exit.json`; do not infer completion from sending a signal.

## Interpretation and next gate

Existing coalescing helps, especially for occasional queued follower bursts,
but typical cohorts remain small. Existing limits are not demonstrated to be
the constraint. Eight synchronous create/close workers and several dependent
metadata/data stages limit simultaneously eligible work. This is a plausible
mechanism, not a measured eligibility breakdown.

Before changing policy, instrument internal durable-watermark cohorts,
hold duration, waiter/queue depth and follower flush reasons; compare controlled
bounded coalescing policies with identical repeated fixtures, idle/mixed p99,
bounded queues/RSS and cold/fault durability gates. Keep fragment persistence
as a separate measurement. Device placement, cache/FUA and XFS log settings
also need controlled comparisons before attributing the NVMe cost to hardware,
firmware, thermal behavior or filesystem configuration. W93 correctness gates
remain intact; this observational run does not close new optimization acceptance.
