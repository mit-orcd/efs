# AMD sequential read profile — 2026-10-15

**Observed/reproduced: 10 GiB dd read completes at109 MB/s (104.28 MiB/s).**
Read-only investigation of the running eight-writer deployment
`bb22e9559337a836856dce1720b739743f0ef2d7`; no source, settings, mount or daemon
changes. TCP/direct, ordinary files; .efs client/server and
expiry disabled. See [deployment identity](intel-default-eight-writers-20261015.md).

## Workload and observation

User AMD PID1161320 was running:
`dd of=/dev/null if=/data1/efs/intel-mnt/001.dat bs=1M count=10240 status=progress`.
The file is10737418240 bytes. Initial observation found the process already64s
old. Captured its final~0.5s of read/write calls and successful exit; the initial
client/daemon profile spans its tail and subsequent idle time and is not used
for attribution. No claim to know the user's original whole-run completion time.

Repeated the exact command read-only with logs separate from the user's terminal.
The repeat completed10240 full records,10737418240 bytes in98.1953s, exit0.
This is a repeated/warm workload, not cold-cache performance or content integrity
qualification: dd to /dev/null checks successful transfer, not a known checksum.
No drop_caches, mounts, restarts or file changes. Profiles perturb its throughput.

Existing `scripts/profile_intel_live.sh` captured20s cycles:u at99Hz with FP
stacks, CPU/network/disk snapshots and one-second wait-channel samples, followed
by12s strace -f -c -w. Targets: AMD client1127604; Intel1 daemons3908856/3908863;
Intel2 daemons4173451/4173458. A separate12s `strace -ttt -T -s0 -e trace=read,write`
on the repeated dd captured1280 complete1MiB reads: mean9.319ms, p9512.448ms.
/dev/null writes take microseconds. Expected tracer timeout exit124; dd exit0.

## Findings

* No fsync or fdatasync calls appear in any of the four daemon strace summaries
  during the repeat. Intel device flush counter deltas are only1–3 each/20s,
  and host writes less than0.4MiB/device. **Per-PUT durability barriers do not
  explain this read workload's limit.** They can still matter for writes.
* Intel NVMe busy time was0.47–2.14s per20s (roughly2–11%). This is not a device
  bandwidth ceiling measurement, but does not show the write-side saturation.
  Server pread64 means70–140us; other read calls103–328us under tracing.
* AMD client used3.10 CPU-seconds/20s (~15.5% of one core). Individual daemons
  used2.79,4.10,6.61,4.00 CPU-seconds (~14–33% of one core). No aggregate CPU
  saturation shown. Single-stage serialization remains possible despite this.
* AMD received3,388,808,183 NIC bytes/20s (~161.6MiB/s), below its2.5Gb/s link
  ceiling. Intel sent~1.35/1.96GB. No new NIC drops/errors. Host-wide TCP
  retransmissions AMD2, Intel0/0. Do not equate these with an EFS retry count,
  or infer read amplification from whole-run dd versus this different window.
* AMD sampled user cycles: mutex unlock27.85%, memcpy7.88%, efs_recv_all6.71%,
  mutex lock5.18%, fetch_published_once4.32%. Inclusive stacks attribute~24.84%
  of all sampled user cycles to unlock under `efs_client_node_is_down`, reached
  through `get_two_parallel_run`. Demand work also executes inline in the FUSE
  read thread (~25% of sampled user cycles through the fetch path), alongside
  GET-pool worker activity. These are CPU shares, not proven blocking duration.
* AMD strace records60790 polls,117742 recvfrom calls and23133 futex calls in12s.
  Summed futex wall time is dominated by many sleeping threads and must not be
  interpreted as a measured active-read lock bottleneck. Likewise recvfrom
  errors may be nonblocking EAGAIN; they are not automatically failed EFS RPCs.
* Server user CPU samples are dominated by BLAKE3 verification (e.g.73.73% of
  Intel1 atom-PMU samples), but total daemon CPU is low. Removing checksums is
  neither justified nor proposed. Hybrid PMU populations are separate.

## Source-informed hypothesis and next measurement

Reads already have concurrency. `src/client/read.c:get_two_parallel_run` sends
two preferred fragment requests and waits for their logical replies. The GET
pool and `prefetch_ahead` supply chunk concurrency; default EFS_READ_PREFETCH=16.
`get_pool_run_budgeted` handles a single demand chunk inline. `maybe_prefetch`
also synchronously requests a missing future layout window before submitting
prefetch work. These are opportunities for gaps despite available workers.

`src/client/node_cache.c:efs_client_node_is_down` locks the same per-node
`conn_lock` used by connection-pool operations for a health lookup. The read
pair path performs multiple such checks. This is a concrete CPU hot path and
candidate for a cheap coherent health snapshot, **not proof that changing it
will recover the missing throughput**. Any future change must retain failure,
recovery and publication ordering rather than bypassing health decisions.

Next isolate demand-cache misses/prefetch hits, time waiting on the per-node
connection lock versus network replies, and synchronous layout misses. Compare
one sequential stream with multiple independent streams and a bounded prefetch
comparison on a separate test mount. That distinguishes per-stream pipeline
limits from shared service capacity. Do not widen limits or remove barriers
based only on perf percentages. No implementation attempted in this task.

## Reproduction and raw evidence

Run the exact dd command above against an existing stable10GiB file. Capture
`bash scripts/profile_intel_live.sh NEW_OUTPUT_DIR MAIN_PID_LIST 20` on each
host while dd is active, and `timeout -s INT 12 strace -ttt -T -s0
-e trace=read,write -p DD_PID -o DD_TRACE` on AMD. Paths/PIDs must be re-inventoried.
Do not include unrelated AMD/private Intel processes. Native profile scripts
are unchanged at the deployed revision; build/binary hashes in linked report.

Local raw summaries, counter parser, dd trace and command logs:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-read-profile-20261015/`.
Remote repeat captures including raw perf.data:
`/data1/efs/evidence/read-repeat-profile-20261015` on Intel and
`/data1/efs/intel-client/read-repeat-profile-20261015` on AMD. Initial partial
captures use `read-profile-20261015`. AMD dd stderr/trace files are
`/data1/efs/intel-client/dd-read-repeat-20261015.log` and `.trace`.
All captures/workloads ended; no cluster or user-file cleanup was required.
