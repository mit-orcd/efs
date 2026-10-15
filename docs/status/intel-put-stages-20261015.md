# Intel PUT stage and overlap measurement — 2026-10-15

Follow-up to [post-pressure diagnosis](intel-post-pressure-profile-20261015.md).
**Measurement PASS; three 4 GiB full-content checks PASS. No EFS implementation,
configuration or deployment change.** This narrows the performance investigation,
not a new correctness or full POSIX acceptance gate.

Follow-up: [durability isolation](intel-durable-isolation-20261015.md) now measures
shared-directory concurrency scaling and limits causal metadata attribution.

## Conclusion

The largest measured end-to-end server delay is writer admission on n1/n3.
The underlying service cost is fragment **file fsync**, not an absence of parallel
writers. Each daemon already reaches four overlapping writer jobs and four
overlapping file fsyncs. n1/n3 have much slower durable service than n2/n4, and
requests queue behind their occupied writers. Directory barriers contribute,
but are not the dominant service stage.

The proposed staged PUT pipeline is **not yet justified as the next implementation**.
Extra queues alone do not remove the measured file barriers. Focus the next
controlled experiment on the slow roots' durable service capacity: compare the
same ordinary-file, O_DIRECT, checksum-tail, file-fsync and directory-durability
sequence at bounded concurrency on an isolated scratch directory, with and
without metadata traffic. Determine whether extra I/O concurrency actually
amortizes device flushes before changing the server pipeline or worker count.
Neither concurrency nor queue/retry budgets were changed here. Never ACK a PUT
before its own required file and directory durability succeeds.

## Deployment and workload

Source `2e935cea075dfec45d25385bc8626c49fee67ba3`, build ID `2e935cea075d`,
build timestamp `2026-10-15T03:42:00Z`; analysis starts from devel `6aa2dbe1a`.
Intel1 `.207`: n1 PID 3880449, n2 3880456; Intel2 `.209`: n3 4146981,
n4 4146988. Ports 17432/17433, 400 GiB/root, approximately 160 GiB used/root.
AMD `.205`: independent Intel-export client PID 3989623 at `/data1/efs/intel-mnt`.
Main Intel has two hosts with four daemon nodes; private Intel is untouched.

Verified `--direct-io --storage-engine files` and `EFS_TRANSPORT=tcp`,
`EFS_COMMANDS=0`, `EFS_COMMANDS_SERVER=0`, `EFS_PACKED_WRITES=0`, `EFS_EXPIRY=0`.
Thus .efs client/server, packing, containers and expiry are disabled.
Intel daemon binary SHA-256:

* Intel1: `3b65b51830ff63e21d1271d5f439a9bc688cbd4111435797ca2b0295c760f8ec`
* Intel2: `2833f63756909a1d70f905be6863a3de5b3bf7cdddf8bac2e8320c4b79189ad5`

n1/n3 use /data1 on PCIe SSD (Intel1 nvme0n1, Intel2 nvme1n1), XFS/noatime.
n2/n4 use /data2 symlinks to the home LV on EXT X200E SSD, XFS/relatime.
These are different devices/filesystem layouts; this run does not prove a device
fault or separate hardware latency from filesystem/metadata contention.
Intel links are 5 Gbps; AMD is 2.5 Gbps.

One AMD writer, pre-generated random nonzero 4096 MiB fixture, `bs=1M`, final
fsync included in timing. Full SHA-256 verification and cleanup follow outside
write timing. All three runs matched
`241f47adae6548fff4e889cfe5e51a73271506d8680feea2609dd5e4e3cf5852`.

| Run | Concurrent xorinox crawl | Seconds including fsync | MiB/s |
| --- | --- | ---: | ---: |
| Untraced baseline | Running, 4 workers | 109.075 | 37.55 |
| Stage probes + first 12 seconds AMD strace | Running, 4 workers | 110.076 | 37.21 |
| Untraced comparison | Temporarily paused | 88.169 | 46.46 |

Each is one trial, not a statistically qualified performance gain. Quiet run is
23.7% faster than loaded baseline; ordering/cache/background effects remain.
The root storage pressure persists with the crawl paused. Its original process
727786 was resumed after the quiet timed write/fsync, before completion of read
verification; output was preserved. No human dd was running before these tests.

## Measured stages

Thirty seconds of perf uprobes and syscall tracepoints on all four main daemons,
about 00:21 EDT. Match submitted stack job pointers across caller/writer threads,
filter to diagnostic file inode **3375071**, and include completed jobs only.
All 23,831 matched completed jobs returned success. Capture-boundary incomplete
jobs are counted separately and excluded from completed-job timings. No event
loss was reported; parser diagnostics show no unparsed lines.

Means in milliseconds (columns are distinct intervals except the explicitly
nested file-fsync column):

| Node | Completed PUTs | Admission wait | Dispatch | Write syscall | File fsync | Entire directory stage | Total submit→return |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| n1 | 5,995 | 25.506 | 0.058 | 1.986 | 8.731 | 2.737 | 39.273 |
| n2 | 6,019 | 3.879 | 0.035 | 0.398 | 2.937 | 0.504 | 7.933 |
| n3 | 5,858 | 56.079 | 0.057 | 1.981 | 8.736 | 3.171 | 70.313 |
| n4 | 5,959 | 2.159 | 0.045 | 0.384 | 2.578 | 0.399 | 5.765 |

Admission is entry to `submit_and_wait_inner` through `slot_run_locked`: includes
path selection/locking as well as waiting for an eligible writer. It is not a
measurement of mutex wait alone. Dispatch ends at `run_job` entry. Write syscall
is `write` inside that worker before `finish_fragment_put`; pre-file-barrier
intervals including path/open/preparation are 2.153/0.527/2.181/0.526 ms.
`finish_fragment_put` to `sync_fragment_parents` also includes fstat/possible
truncate/close; its means are 8.773/2.954/8.774/2.596 ms, nearly all file fsync.
Directory stage includes ancestry proof lookup and group waiting, not just the
calling thread's directory fsync syscall. Caller resume averages 0.030–0.047 ms.

Actual writer service means: n1 13.668 ms, n2 3.988, n3 14.131, n4 3.526.
File fsync accounts for approximately 64%/74%/62%/73% of these service means.
Admission p95: 100.03/23.40/164.39/15.03 ms. File fsync p95:
14.61/5.42/14.79/4.56 ms. No larger retry budget is indicated by this evidence.

### Overlap and queueing

| Node | Mean active writers over capture | Peak active writers | Time with all four active | Queued while all four busy | Queued with fewer than four active |
| --- | ---: | ---: | ---: | ---: | ---: |
| n1 | 2.73 | 4 | 15.97 s | 14.43 s | 1.43 s |
| n2 | 0.80 | 4 | 2.79 s | 2.30 s | 0.33 s |
| n3 | 2.77 | 4 | 18.64 s | 17.94 s | 1.25 s |
| n4 | 0.70 | 4 | 2.09 s | 1.64 s | 0.35 s |

Each daemon also reaches **four simultaneous file fsyncs**. Mean concurrent file
fsyncs over their capture spans: 1.74/0.59/1.71/0.51. Writes themselves reach
four overlapping calls; directory fsync calls reach two. Counts are independent
host-local interval sweeps, not sums misrepresented as wall time.

Slot-affine fallback admission in `src/server/writer.c:slot_handoff` can leave a
waiter attached to one slot while another becomes free. This may contribute to
the small queued-with-idle intervals; these intervals also include dispatch,
locking and completion handoff. They are **not proof of a scheduling bug**.
At least four waiting submissions coexist with an idle worker for only
0.335/0.083/0.547/0.062 seconds. Most slow-root queueing coincides with four busy
workers. A queue redesign is not supported as the dominant remedy by this run.

The latter 15 seconds exclude the initial AMD strace period (server probes remain):
n1 admission/file-fsync 28.70/8.49 ms; n3 83.26/9.38 ms. The same slow-root pattern
therefore persists after client syscall tracing detaches. These are still
instrumented measurements, not native latency guarantees.

### AMD and disk evidence

AMD strace matched 9,318 PUT replies, all OK: mean 47.65 ms, median 44.43,
p95 95.87, max 243.52. Five REPORTs all OK: mean 30.53 ms, max 43.37.
269 GETCHUNKS_PACKED mapping queries also succeeded; the message name does not
enable small-file packing. RPC timing includes client ptrace/scheduling and
excludes checkout; do not subtract aggregate server means from these different
sample populations or compare timestamps across hosts. Unmatched exchanges at
capture boundaries are not classified as failures.

Disk snapshots bracket capture **and perf text conversion**, 34 seconds on Intel1
and 35 on Intel2 (not exactly 30). /data1 device busy time is approximately
98%/93%, writing about 25.5/22.6 MiB/s, with 18,078/18,420 flush commands.
A separate 20-second quiet-run sample has /data1 busy about 93%/90%, writing
25.6/32.5 MiB/s, with 7,701/8,103 flush commands. Device counters include all
workloads; NVMe busy percentage alone does not prove its maximum capacity.
Together with directly measured file sync times and queue overlap, this supports
a durable-I/O bottleneck on the slow roots, aggravated by concurrent metadata.
The snapshot does not justify calling their raw bandwidth exhausted.

## Reproduce and validate

Use `scripts/capture_intel_put_stages.sh` on Intel1 and Intel2 as root. The script
pins this deployment/PIDs deliberately; for later reproduction first resolve
current main-daemon PIDs and exact executable, then update its explicit guards
and use a fresh output directory. Never point it at private Intel processes.
It requires existing perf, debug symbols, tracing support and root privileges.
The script verifies its private probe namespace is empty, installs only eight
function probes, waits at most ten minutes for `OUT/start`, records 30 seconds,
converts events and removes its own probes on exit. It changes no EFS setting.
The actual root activation was explicitly performed by the human.

Once both hosts have `OUT/ready`, start on AMD:

```sh
python3 /data1/efs/intel-client/src/tests/measure/intel_dd_sync_batch.py \
  /data1/efs/intel-mnt /data1/efs/intel-client/post-pressure-profile \
  candidate --mib 4096 --repeats 1
```

Touch `/data1/efs/put-stages-20261015/start` on each Intel host. Record the new
output's inode with `stat -c %i PATH`. In parallel, capture only the Intel-export
AMD client for 12 seconds:

```sh
timeout -s INT 12 strace -f -ttt -T -yy -xx -s 8 \
  -e trace=sendmsg,recvfrom -p CLIENT_PID -o rpc.trace
```

Analyze locally:

```sh
python3 scripts/summarize_put_stages.py events.txt --ino WORKLOAD_INODE
python3 scripts/summarize_put_stages.py events.txt --ino WORKLOAD_INODE --skip-seconds 15
python3 scripts/summarize_intel_rpc_trace.py rpc.trace
PYTHONDONTWRITEBYTECODE=1 python3 scripts/test_summarize_put_stages.py
bash -n scripts/capture_intel_put_stages.sh
```

The two deterministic parser tests pass: cross-thread identity, decimal/hex
pointers, boundary exclusions, inode/time filters, disjoint stage durations and
interval overlap. Uprobe field offsets 24/32/36 for inode/chunk/fragment were
verified using the deployed binary's DWARF `ptype /o struct writer_job`.
The measurement script removes only its unique mounted output; the external
random fixture is retained. For quiet comparisons use authorized pause/resume
of the identified crawl with an automatic resume guard, preserving its output.

All probes were removed and the original four daemon PIDs remain running.
AMD strace detached, all three owned workload runs finished, and the existing
xorinox crawl resumed. Full POSIX, injected faults, crash recovery, new pipeline
and alternative writer counts are NOTRUN in this observational task.

## Evidence locations

Local:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-put-stages-20261015/`.
Remote on Intel/AMD: `/data1/efs/put-stages-20261015`.
Raw perf data stays on Intel; retrieved archives contain text events, identities,
record logs and snapshots, not source assets. No raw artifacts enter Git.

* Intel1 archive SHA-256: `e2eff3a9c796fc8c45fc477b5441953b3ab9f91e9f575d0ae487074bf2e03e9a`
* Intel2 archive SHA-256: `8c6ee3301483f00b5f5f7f8201d1774f05c467b0cdaf65717d7ac11c48c56674`
* AMD trace SHA-256: `cb61823d730617e52dc93800081a4a6f1ce6f1382b51d4248615e14846fc0eca`

`SHA256SUMS` also records all three workload-result files. Retained stage summaries
include counts, means, p50/p95/max, overlap distributions and parser diagnostics.
