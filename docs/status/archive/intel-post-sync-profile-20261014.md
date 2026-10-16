# Intel post-batching live profile — 2026-10-14 EDT

**Historical read-only capture PASS.** The recommendation was subsequently
implemented and smoke-qualified in [indexed pressure draining](../intel-pressure-drain-20261014.md).
The analysis below records what was known before that implementation.
Follow-up to [sync batching](../intel-sync-batching-20261014.md). The next promising
throughput target is foreground write-pressure draining, with two expensive cache
walks as a directly measured CPU cost. The capture does not establish the speedup
that changing this path would deliver. No deployment, mount, configuration, or
workload changes were made. This is performance evidence, not filesystem acceptance.

## Identity and workload

Deployed devel source `2256f1a34fc1006e3ecb9ab356a46a0341f0b605`, build ID
`2256f1a34fc1`, version `v0.3.2-pre-alpha-468-g2256f1a34`, timestamp
`2026-10-15T02:57:30Z`. Documentation HEAD before capture: `12e49ff07`.
Four file-storage daemons on Intel1 (`10.0.0.207`, n1/n2) and Intel2
(`10.0.0.209`, n3/n4), ports 17432/17433, 400 GiB quota per daemon.
Daemon arguments retain `--direct-io`; relevant process
environment retains `EFS_TRANSPORT=tcp`, `EFS_COMMANDS=0`,
`EFS_COMMANDS_SERVER=0`, `EFS_EXPIRY=0`.
Thus .efs client/server and expiry remain disabled.

Profiled PIDs: Intel1 3873880/3873887, Intel2 4142560/4142567,
AMD FUSE 3885731, user dd 3907299. Existing Intel mounts remain at
`/data1/efs/mnt`; independent AMD and xorinox clients use
`/data1/efs/intel-mnt`. Private Intel was untouched.

User workload, already running when attached:

```sh
dd if=/dev/urandom of=/data1/efs/intel-mnt/001.dat bs=1M count=10240 status=progress
```

This is a 10 GiB write without `conv=fsync`. Its progress is not a durable-fsync
throughput result. A four-worker xorinox `tiny_files_md5.py` crawl was also active;
its metadata/read traffic contributes to daemon samples. The user dd was live
through profiling; at 03:17:21 UTC its PID was absent and AMD remained mounted.
We did not stop it and cannot recover its exit status from this session.

## Measurements

Perf-only observation was approximately 30–31 seconds, beginning
03:12:33–36 UTC on the three hosts (October 15 UTC, October 14 EDT).
Subsequent syscall tracing was deliberately separate and perturbs latency.

| Observation | Result | Meaning |
| --- | --- | --- |
| AMD dd write-byte delta | 2,190 MiB in approximately 31 seconds, about 71 MiB/s | Logical progress, not a completion/durability benchmark |
| AMD NIC TX | 3,349 MiB, about 108 MiB/s | Well below the 2.5 Gb/s physical link ceiling |
| Client CPU | About 16% of one core; dd about 13% | Client is mostly waiting, not exhausting CPU |
| Daemon CPU | n1 about 32%, n2 41%, n3 86–89%, n4 24% of one core | n3 includes competing metadata crawl work |
| Ethernet error/drop deltas | Zero on all three hosts | No observed interface loss/error signal |
| TCP retransmission deltas | AMD 3 / 2,478,943 outgoing segments; Intel1 0; Intel2 0 | Host-wide counters, not exclusively EFS |
| Matched PUT replies, instrumented | 3,432 OK; mean 29.43 ms, p95 75.93 ms | No captured failing replies |
| Matched REPORT replies, instrumented | 122 OK; mean 20.19 ms, p95 37.25 ms | No captured failing replies |

Eight PUT sends lacked a matched complete reply within the trace window; they
are not classified as failed. Nonblocking EAGAIN and unpaired boundary frames
are not EFS retries. No retry storm is evident in this sample; this does not
prove an absence of retries outside it. Host counters cannot establish zero
end-to-end packet loss.

AMD's 2.5 Gb/s link has a raw ceiling of about 298 MiB/s. With 2+1 erasure-coded
payload expansion, the optimistic logical ceiling is about 199 MiB/s before
protocol overhead. Intel's 5 Gb/s interfaces do not remove this client limit.

## Source-correlated hot path

`perf` attributes **66.40% of sampled AMD client user cycles** to
`fuse_write_admit`:

- `efs_dcache_trim_metadata`: 30.90% inclusive, mostly mutex operations.
  `src/client/write.c` walks all 65,536 slots, locking/unlocking per slot,
  even when metadata usage is far below its 8 MiB budget.
- `efs_dcache_pressure_ino`: 26.69%, walking all slots/chains across 64 shards
  to select the next inode whose data/publication ownership must be retained.
- The admission path also calls `efs_append_flush_report`, whose ordering
  includes queued writes, append serialization, data flush and metadata REPORT.

`src/client/bufpool.c:efs_buf_should_drain` triggers at `g_hard / 8`:
32 MiB with the default 256 MiB hard budget. The log's 320 MiB limit includes
64 MiB drain reserve; it is not the early-drain threshold. Captured pressure
messages show approximately 32–33 MiB live, `failed=high-water`, often about
0.24 seconds apart. Metadata occupancy in that interval was only tens of KiB.

`src/client/efs_fuse.c:fuse_write_admit` first releases a successful reservation
when the high-water threshold is reached, trims, checks the pressure inode,
and may synchronously call `efs_append_flush_report(NULL, ino)` before retrying
admission. This makes an ordinary write wait on a whole-inode ordered drain.
Existing background PUT/reclaim/report machinery already exists; the proposal
is to improve its pressure integration, not add an unrelated second pipeline.

The CPU percentages above are percentages of sampled user cycles, not elapsed
time. Eliminating these walks cannot honestly be advertised as a 66% throughput
gain. The stronger throughput hypothesis is that synchronous admission drains
reduce overlap between accepting writes, durable fragment PUTs and publication.
A timed admission/credit trace is still needed to quantify that contribution.

The 12-second syscall summary corroborates substantial waiting: dd made 695
write calls taking 10.54 aggregate seconds (including progress output), versus
672 reads taking 1.07 seconds. FUSE spent most aggregate thread time in
futex/poll. Thread durations overlap and must not be added as elapsed time.

Storage durability remains material. A separate six-second node3 path trace
recorded 533 completed fragment-file fsyncs averaging 5.76 ms, 531 directory
fsyncs averaging 0.78 ms, 1,165 Raft-log fdatasyncs averaging 2.95 ms and 1,040
KV-WAL fdatasyncs averaging 3.11 ms; no errors in these completed calls.
These are instrumented, concurrent timings, not isolated device benchmarks.
Directory batching does not replace per-file durability. Node3 user CPU also
shows metadata comparisons/LSM probes; concurrent xorinox traversal prevents
attributing all of that cost to dd.

## Recommended next change and acceptance

Implement **bounded background pressure draining with indexed candidates**:

1. Maintain bounded per-inode pressure eligibility and reclaimable-metadata
   candidates instead of repeatedly scanning every slot. Revalidate identities
   under existing locks; preserve dirty, PUT-in-flight, unpublished, failed and
   recovery-pinned ownership. Avoid stale entry pointers and unbounded indexing.
2. At the early threshold, schedule a bounded cohort through the existing
   background write/publication path. Let admission proceed when its existing
   reservation succeeds; wait for the required credit when it cannot. Preserve
   hard body/metadata limits and drain reserve. Do not merely raise thresholds,
   widen RPC windows, or extend retry budgets.
3. Explicit fsync/close must still wait for all their prior writes and successful
   metadata publication. Preserve EOF/truncate ordering and retain uncertain
   outcomes until resolved; failed durability/publication must not release
   ownership or report false success. Retain a deadline-bounded foreground
   progress/error path if the worker cannot reclaim credit.

Start with the indexed-walk change as a separately measurable, lower-risk step.
The background-drain change is the larger throughput hypothesis and requires
correctness design before implementation. Do not remove fragment fsyncs or
introduce new storage formats to obtain a benchmark improvement.

Qualification should include native and sanitizer tests of eligibility/credit
transitions, multiwriter races, delayed/failed PUT and REPORT, cancellation,
uncertain responses, hard-cap pressure, fsync twice, close, shrink/regrow/EOF and
retry ownership. Measure before/after using identical binaries except the
change, same metadata leadership, matched load, pre-generated local input,
`conv=fsync`, unique files and post-write hashes. Record admission wait time,
PUT/report concurrency, sync count, memory peaks and wire rate. Repeat with the
current random-input/crawl workload separately. A throughput gain and correct
memory/durability behavior remain NOTRUN for this proposal.

## Reproduction and evidence

Prerequisites: same-user SSH, perf/strace attachment permission and live Intel
PIDs verified from cmdline/executable/environment. Never reuse historical PIDs
without verifying them. Do not start or stop the human workload for observation.
Run the committed script on each host with a fresh output path:

```sh
bash scripts/profile_intel_live.sh NEW_OUTPUT_DIR AMD_FUSE_PID,DD_PID 30
bash scripts/profile_intel_live.sh NEW_OUTPUT_DIR INTEL1_N1_PID,INTEL1_N2_PID 30
bash scripts/profile_intel_live.sh NEW_OUTPUT_DIR INTEL2_N3_PID,INTEL2_N4_PID 30
```

The script records `perf record -e cycles:u -F 99 --call-graph fp`, counter and
wait snapshots, then separate `timeout -s INT 12 strace -f -c -w` captures.
For bounded protocol and durability inspection, equivalent commands are:

```sh
timeout -s INT 10 strace -f -ttt -T -yy -xx -s 8 -e trace=sendmsg,recvfrom -p AMD_FUSE_PID -o rpc.trace
timeout -s INT 6 strace -f -ttt -T -yy -e trace=fsync,fdatasync -p INTEL2_N3_PID -o sync.trace
python3 scripts/summarize_intel_rpc_trace.py rpc.trace
```

Timeout 124 is expected for bounded attach; verify tracers detached. No test
payload was created or removed by this observation. Keep raw output outside
Git; remove only the newly created capture directory if cleanup is desired.

Local raw root:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-post-sync-profile-20261014/`.
Remote capture directory on each profiled host:
`/data1/efs/post-sync-profile-20261014`.
Local host subdirectories retain perf data/reports, counters and syscall traces;
root files include process inventories, RPC summary and subsequent trace copies.
Archive SHA-256 (archives cover the main perf/syscall phase):

- `amd_efs.tar.gz`: `c27bb826a40cdec662486c9e95cc306fe2132d791a4c5da507266f8b2ba7a0fa`
- `intel1_efs.tar.gz`: `4498f5fcea5cd626cb188577b11ae4e8e7f8e2e359446d216ea7fb268ecdafc6`
- `intel2_efs.tar.gz`: `f047214d3aa6aa9f94a8bb7e07cfc71caa311b7cdb6bc0d69aabedf248ca6c8b`
