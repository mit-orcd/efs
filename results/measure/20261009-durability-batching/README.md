# Existing durability batching — NUC, 2026-10-09

## Outcome

The deployed batching works, but under this eight-worker tiny-file workload it
only amortizes a small number of Raft entries per WAL sync. Most calls still
cover one entry. No code, deployment, batching limit, filesystem option or
durability contract changed. This is an observational measurement, not an
enabled/disabled batching A/B or a hardware-ceiling proof.

| Node / backing device | Completed interior syncs | Entry writes assigned | Entries/sync | Single-entry syncs | Mean sync | Approximate call reduction vs one sync/entry |
|---|---:|---:|---:|---:|---:|---:|
| n1 / SATA | 654 | 654 | 1.00 | 100.0% | 0.130 ms | 0% |
| n2 / SATA | 1,976 | 2,875 | 1.45 | 74.5% | 0.113 ms | 31.3% |
| n3 / NVMe | 1,554 | 2,075 | 1.34 | 76.4% | 7.325 ms | 25.1% |
| n4 / NVMe | 1,158 | 1,615 | 1.39 | 73.1% | 8.006 ms | 28.3% |

Different nodes were traced sequentially, not simultaneously, and host different
group traffic: n1 observed group 0; n2/n3 groups 0 and 2; n4 group 2. These are
all WAL entry records, not file counts, exclusively foreground operations, or
proof that every observed entry was committed. One Raft command may contain
multiple metadata operations. Call reduction is an arithmetic counterfactual,
not a measured throughput improvement or physical-device flush reduction.

| Window / trace start offsets | n3 entries/sync | n3 mean / p95 sync ms | n4 entries/sync | n4 mean / p95 sync ms |
|---|---:|---:|---:|---:|
| Early / n3 46 s, n4 54 s | 1.371 | 6.080 / 10.708 | 1.394 | 7.878 / 12.228 |
| Middle / n3 146 s, n4 154 s | 1.324 | 8.288 / 13.317 | 1.338 | 8.007 / 13.171 |
| Late / n3 246 s, n4 254 s | 1.301 | 7.999 / 13.713 | 1.455 | 8.143 / 13.374 |

Batch sizes do not grow enough to offset the NVMe sync cost. n3/n4 usually
write only about 163–175 WAL bytes per barrier. Largest observed batches are
7 and 6 entries, respectively. Only eight observed n2 barriers span groups;
none on n1/n3/n4 do. This does not diagnose why otherwise eligible work failed
to coalesce: internal eligibility/queue/hold counters were not instrumented.

## Method, raw evidence and limitations

Build `800122c5e86d`, TCP/direct I/O, `.efs` enabled with existing 1024 MiB cap.
`identity.json` records unchanged daemon PIDs, commands and WAL descriptors.
Dedicated generator PID 2519952:

```
python3 /home/efs/tiny_files.py --max-depth 8 --max-files-per-folder 1000 \
  --max-files 50000 --workers 8 /data1/efs/mnt/tiny-batching-20261009
```

Collector samples host/disk/process counters every five seconds for 300 seconds.
Each wave traces n1/n2/n3/n4 sequentially for six seconds each, starting at
30/38/46/54, 130/138/146/154, and 230/238/246/254 seconds. `capture.py` is the
reproducible collector; `events.jsonl`, `samples.jsonl`, `progress.log`, daemon
logs and twelve `w*-n*.trace` files retain raw evidence. The full interval starts
2026-10-09 05:49:55 UTC and ends 05:54:55 UTC. Kernel tracepoints are not readable
by this account; no sudo permission or device settings were changed.

`strace -f -ttt -T -yy -xx -s 16 --trace-fds=WALFD` observes successful
`pwrite64` record framing and `fdatasync`, plus WAL allocation/truncation calls.
Sixteen bytes include record type and group without command payloads.
`analyze.py` reconstructs syscall entry/completion times, identifies complete
record writes and assigns writes completed before a sync starts to that sync.
Writes completed during a sync are conservatively assigned to the next one,
consistent with the captured-byte watermark contract. Drop the first sync
(unknown pre-attach history), incomplete/detached end calls, and failed writes.
All included writes are full, correctly framed entry records; no completed
traced syscall errors were seen. Expected detach lines remain in raw traces.
`analysis.json` retains individual cohorts, distributions and parser diagnostics;
`windows.json` contains rates and whole-device diskstat deltas.

These are syscall-boundary estimates, not instrumented internal `cover` values.
Tiny races between capture of `d->bytes` and syscall entry, and timestamp
resolution, can shift adjacent cohorts. n2 has a few zero-assigned-entry syncs;
do not conclude they are unnecessary barriers. Strace perturbs scheduling;
single-entry frequency and exact latency need low-overhead counters for final
optimization acceptance. fdatasync count does not equal block flush count:
XFS and the device can combine persistence across concurrent writers.

## Rate/device correlation

Before tracing (5–30 seconds), interval throughput is 96 files/s. Untraced
60–130, 160–230 and 260–300 second intervals are 21.6, 23.1 and 24.0 files/s.
Thus slowdown persists with strace detached. Random folder/depth placement,
batched progress counters and changing namespace load mean this is not a
matched overhead control or comparable run-to-run throughput benchmark. It is
slower than the preceding five-minute run; do not attribute that difference
solely to tracing or batching without controlled repeated fixtures.

NVMe untraced write await is 3.71–4.20 ms, flush await about 4.05 ms, busy about
93%, with only 6–7 MiB/s writes. SATA write await remains approximately 0.18–0.19
ms. File creation is latency-heavy, rather than approaching sequential bandwidth.
Queue configuration read during collection:

- SATA: `write through`, FUA 0, mq-deadline, wbt 2000 us, nr_requests 64.
- NVMe: `write back`, FUA 1, none scheduler, wbt 2000 us, nr_requests 1023.
- `/data1` XFS: noatime, logbufs=8, logbsize=256k.
- `/home` XFS: relatime, logbufs=8, logbsize=32k, backed by LVM on NVMe.

These differences are possible comparison variables, not identified causes.
NVMe health/controller access is root-restricted; thermal/cache/controller-GC
explanations remain unproven. The prior fragment fsync measurement remains a
separate persistence cost: improving WAL batching alone does not eliminate it.

## Code interpretation and next experiment

`host_propose_impl` shares holds and lets already-entered proposers append before
release (bounded sched_yield loop). `log_sync_locked` skips an already-durable
tail and covers every record written at its captured watermark. Follower inbox
batching preserves order, combines contiguous same-group messages (at most 16,
1 MiB), and only defers eligible stable-term strictly extending requests.
Boundaries flush; no timed follower accumulation is added. ACKs retain the
durable-log requirement. An eight-worker closed-loop create/close workload has
limited outstanding work; persistence dependencies can prevent large cohorts.
The traces support modest coalescing, but do not isolate the exact boundary.

Next: add low-overhead counts for entries/bytes per durable watermark, hold
duration, waiter depth, follower batch eligibility and flush reasons. Compare
the existing policy against bounded coalescing in identical repeated fixtures,
including idle p99, mixed traffic, RSS/queues and crash/sync-failure gates.
Separately compare n3/n4 device placement and filesystem configuration with the
same workload. Do not disable barriers, weaken ACK durability or assume a larger
batch cap helps when current batches are far below the existing limits.

At 300 seconds the collector sends one SIGINT to stop generator scheduling;
the executor drains already-queued tasks before exit. The recording is complete;
no forced kill or data removal is used. Test data is retained. Consult remote
`loader-exit.json` for eventual drain completion rather than treating the final
capture count as a verified filesystem count.
