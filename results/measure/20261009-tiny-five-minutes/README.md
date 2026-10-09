# NUC tiny-file five-minute profile — 2026-10-09

## Result

The startup burst falls to a much lower sustained rate, but the five-minute
capture does not show a monotonic decline. The displayed rate is cumulative
created/elapsed, rather than an interval rate. The strongest evidence is
durability latency on the NVMe shared by n3/n4, not Python or client CPU saturation.
This does not establish a hardware ceiling or explain the underlying increase
in drive latency. No production code, durability settings, or deployment changed.

| Capture interval | Interval files/s | Displayed cumulative files/s | NVMe busy | NVMe write await | NVMe flush await |
|---|---:|---:|---:|---:|---:|
| 0–100 s | 39.64 | 95 | 92.8% | 3.26 ms | 3.10 ms |
| 100–200 s | 47.48 | 76 | 91.0% | 4.59 ms | 3.84 ms |
| 200–300 s | 61.15 | 72 | 88.4% | 4.77 ms | 4.06 ms |

Startup reached 215 files/s cumulative at 30 seconds. The capture starts at
generator elapsed 50 seconds (10,344 reported files) and ends at 350.1 seconds
(25,171). Progress counters publish batches of 64 per worker; use long intervals
instead of interpreting short stalls as exact completion rates.

## Method and identity

Production build: `800122c5e86d`, TCP, direct I/O, `.efs` enabled, catalog cap
1024 MiB unchanged. UTC recording: 2026-10-09 05:26:17–05:31:17.
All four daemons (2312683, 2314230, 2315840, 2317774), main FUSE client
2319366 and generator 2369220 were recorded together at 99 Hz with call chains.
`perf.data` has 14,954 samples, zero lost. `identity.txt` records command lines.
`samples.jsonl` contains 31 ten-second process/host/disk observations;
`capture.py` retains the collection/report procedure. Perf percentages describe
on-CPU samples, not blocked time; inclusive rows overlap.

No generator was active initially. A detached `tiny_files` screen was created
with `./tiny_files.py --max-depth 8 --max-files-per-folder 1000 --max-files 50000
--workers 8 /data1/efs/mnt/tiny-profile-five-minutes`. Payload defaults to 1–512
bytes. The existing delete screen was idle and untouched. After capture, one
SIGINT stopped scheduling; queued worker activity continued before process exit.
The final log ends with KeyboardInterrupt; no SIGKILL, cluster restart, or data
deletion was used. The dataset is retained. Final reported count is 32,497,
not a verified filesystem count.

Instrumentation notes: an initial collector exited because pressure files were
unavailable; its partial perf was stopped and is excluded from this analysis.
The full 300-second recording reused the running generator. Window reports were
regenerated using absolute perf timestamps starting at 268962.051489; initial
relative filters returned no samples. The included reports are corrected.

## Durability evidence

n1/n2 use `/data1` XFS on SATA `/dev/sda`; n3/n4 use `/home` XFS on
`rl-home`, backed by Samsung SSD 970 EVO 1TB `nvme0n1`. The latter was busy
89–93% of the capture at only 9–11 MiB/s writes. SATA busy was 8–11%, with
0.18–0.28 ms write await. NVMe write await rose to 4–5 ms. Busy percentage
alone does not establish capacity exhaustion on a parallel device.

A separate six-second `strace -f -tt -T -yy` of n3, after the perf recording
while queued work drained, captured:

| Completed syscall/path | Calls | Mean elapsed | Maximum |
|---|---:|---:|---:|
| Raft WAL fdatasync (`mdraft/log/raft.log`) | 735 | 7.79 ms | 29.79 ms |
| Fragment-file fsync (`data/exports/.../0.0.*`) | 280 | 19.59 ms | 43.22 ms |
| Fragment directory fsync | 1,974 | approximately 0.007 ms | 0.20 ms |
| Other ancestor fsync | 1,974 | approximately 0.005 ms | 0.04 ms |

Waits overlap across threads and must not be summed as wall time. Strace adds
overhead and this is a follow-up observation, not an untraced latency benchmark.
Thread snapshots also show `xlog_wait_on_iclog` and `folio_wait_bit_common` waits.
The existing Raft log already coalesces durable syncs; further work should measure
its batching effectiveness rather than assume group commit is absent. Fragment
ACKs must continue protecting durable contents and pathname.

## CPU and catalog findings

Python used 1.3–1.4% of one core; FUSE 3.2–3.6%. Combined attached process CPU
was about 41–44% of one core. FUSE's principal on-CPU stack is close/flush through
dcache PUT and append flush reporting, consistent with persistence dependencies.
Python stack unwinding is incomplete and unknown frames are not interpreted.

Catalog builder inclusive cycle share fell 19.65% → 15.86% → 8.71% across the
three windows. Synchronous fragment-write share rose 7.31% → 8.90% → 10.47%;
merge scan remained around 2.2–2.7%. There is no growing dominant CPU hot path
in this steady capture. Compactions occur (maximum logged durations 202–322 ms),
but their contribution to device latency is not isolated by this recording.

At 05:29:26.234, n2 last published generation 723: 1,252,820 files, 5,276
directories, accounted bytes 1,068,704,992, build 65 ms. At 05:29:27.739 it
reported NOMEM at the 1024 MiB cap. n3 also reports cap failures. Approximately
16 GiB available RAM and ample disk space rule out simple host-space exhaustion;
the explicit catalog accounting cap is the consistent explanation, including
temporary build budget. Allocation-specific failure tracing was not collected.
This leaves stale `.efs` snapshots and requires separate repair. It occurs about
191 seconds into capture, long after the initial slowdown, so cannot explain
that initial transition. Retry backoff may reduce background load later; its
causal contribution to the recovering rate is not measured.

## Next controlled investigation

Measure existing WAL coalescing and fragment durability batching, preserving
ACK guarantees. Compare identical workloads with n3/n4 on separate backing
devices to isolate shared-device contention. Gather device health/temperature
and longer latency history before attributing the transition to thermal
throttling, cache exhaustion, or controller garbage collection. OS block writes
are not NAND writes and do not establish SSD write amplification.
