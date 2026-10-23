# Intel post-pressure-index throughput diagnosis — 2026-10-15

Follow-up to [pressure draining](intel-pressure-drain-20261014.md).
**Capture and two full-content checks PASS; throughput improvement NOT ESTABLISHED;
proposed server PUT pipeline NOT IMPLEMENTED/NOT QUALIFIED.** This was diagnosis
only: no restart, deployment, settings change, or change to private Intel.

## Identity and reproduction

Main Intel n1/n2 on intel1 (10.0.0.207), n3/n4 on intel2 (.209), ports
17432–17433, 400 GiB/root, about 159.6 GiB used/root. Daemon PIDs respectively
3880449, 3880456, 4146981, 4146988; AMD (.205) client PID 3989623.
Deployed source `2e935cea075dfec45d25385bc8626c49fee67ba3`, build ID
`2e935cea075d`, build timestamp `2026-10-15T03:42:00Z`; documentation checkout
`6231df267`. TCP, daemon `--direct-io`,
`EFS_COMMANDS=0 EFS_COMMANDS_SERVER=0 EFS_EXPIRY=0`.
Thus .efs client/server and expiry remain disabled.
AMD independent mount `/data1/efs/intel-mnt` uses the same release.
Intel NICs 5 Gbps; AMD 2.5 Gbps. Intel /data1 uses PCIe SSD, XFS/noatime;
second roots use EXT X200E SSD, XFS/relatime on the host home logical volume.

No human dd was running at inventory. Ran the committed measurement script twice
on AMD, one writer, random nonzero 4096 MiB source prepared outside the export:

```sh
python3 /data1/efs/intel-client/src/tests/measure/intel_dd_sync_batch.py \
  /data1/efs/intel-mnt /data1/efs/intel-client/post-pressure-profile \
  candidate --mib 4096 --repeats 1
```

It invokes `dd bs=1M count=4096 conv=fsync,excl status=none`, hashes the whole
result outside write timing, and removes only its uniquely created test output.
Both hashes matched
`241f47adae6548fff4e889cfe5e51a73271506d8680feea2609dd5e4e3cf5852`.
The local source fixture is retained. Xorinox's existing four-worker MD5 crawl
(PID 727786) continued throughout; it was not paused or restarted.

During first write, use `scripts/profile_intel_live.sh OUT PIDS 20` on each host
(AMD PIDS `3989623,4022539`, Intel as above). This collects 20 seconds of
`perf record -e cycles:u -F 99 --call-graph fp`, counters/waits, then bounded
12-second `strace -f -c -w`. Requires perf/strace and permission to trace owned
processes. Resolve fresh PIDs before reproduction; never reuse stale PIDs.

During second write (dd PID 4030368), capture on AMD:

```sh
timeout -s INT 12 strace -f -ttt -T -yy -xx -s 8 \
  -e trace=sendmsg,recvfrom -p 3989623 -o write-rpc.trace
```

On each Intel host, attach its two main daemons for 10 seconds:

```sh
timeout -s INT 10 strace -f -ttt -T -yy \
  -e trace=fsync,fdatasync,pwrite64,pwritev,writev \
  -p PID1 -p PID2 -o write-sync.trace
```

Analyze RPCs with `scripts/summarize_intel_rpc_trace.py write-rpc.trace`.
Its `syscalls()` helper reconstructs unfinished calls for storage analysis.
Separate fragment file paths, containing directories, and `mdraft` WAL paths;
exclude TCP writes and daemon log writes. Sum durations only as overlapping
thread time, never as elapsed-time percentages. The first detailed RPC/sync
capture landed in read verification: excluded from write-path conclusions.
The separately retained `write.trace` files are the valid second-write capture.
All profilers detached; the four original daemons and AMD FUSE mount remain up.

## Findings

The previous quiet comparison was 77.08 MiB/s baseline versus 77.28 MiB/s final
six-trial median: no sustained gain. Current traced/load-bearing runs were
59.24 and 32.94 MiB/s including final fsync, both content PASS. These are diagnostic
runs, **not** an untraced before/after benchmark; the deeper trace strongly perturbs
execution and the crawl is concurrent.

* Client admission is now 7.46% of sampled user cycles versus 66.4% in the earlier
  live profile. Different workload windows prevent a controlled speedup claim,
  but full-cache scans are no longer the dominant sampled stack. AMD client used
  1.82 CPU seconds across the 20-second perf window (about 9.1% of one core).
  memcpy (57.25%), BLAKE3 (26.06%) and memset (8.19%) dominate its remaining
  user-cycle samples, **not elapsed time**. Optimizing these is unlikely to
  explain seconds of waiting.
* In that window dd advanced 846 MiB (42.3 MiB/s, not final durable throughput).
  AMD Ethernet transmitted 77.57 MiB/s, received 4.86 MiB/s, no NIC errors/drops;
  host TCP retransmissions increased by two. Intel host retransmissions did not
  increase. These counters are host-wide, not per-EFS attribution. There is no
  evidence that packet loss or link saturation explains this throughput.
* Intel1 /data1 NVMe was busy 90.13% and Intel2 /data1 92.69%, while writing
  only 38.36 and 39.86 MiB/s. Their flush-command deltas were 16,776 and 20,289
  in 20 seconds. Other NVMe devices were busy 62.13%/55.01%. Disk busy time
  on NVMe does not prove maximum device throughput; this pattern supports
  a latency/barrier-heavy workload rather than streaming bandwidth saturation.
* Valid write trace: 9,208 matched PUT replies, all OK, mean 33.88 ms,
  median 12.76 ms, p95 122.53 ms, maximum 407.00 ms. Five REPORT replies,
  all OK, mean 19.42 ms, maximum 31.97 ms. 375 GETCHUNKS replies all OK.
  9,250 PUT sends were captured; unmatched boundary exchanges are not failures.
  These timings include ptrace and scheduling overhead, exclude connection
  checkout, and do not establish a native latency distribution.
* In approximately 10 seconds, Intel1 performed 4,311 fragment fsyncs totaling
  13.40 overlapping thread-seconds, 4,916 directory fsyncs totaling 4.17 seconds,
  and 7,604 WAL fdatasyncs totaling 11.30 seconds. Intel2: 4,421 fragment fsyncs /
  21.10 seconds, 5,000 directory fsyncs / 7.99 seconds, 3,049 WAL fdatasyncs /
  11.09 seconds. Metadata WAL work includes other traffic; do not attribute all
  of it to dd or equate it to REPORT count.
* Intel2 atom-core user samples show 55.22% under `efs_meta_apply_getattr` via
  `server_raft_host_lookup`; reductions and LSM probes/scans dominate that stack.
  This is mixed server workload including the crawl, not proof that dd spends
  55% of its elapsed time on GETATTR. Hybrid core events are separate populations.

## Source interpretation and next fix

`src/server/handler.c` PUT calls the store and replies only after it returns.
`src/server/store_nvme.c` delegates to `server_write_fragment_with_sum`;
`src/server/writer.c` hands a job to a writer slot and waits. That slot stays
occupied through `run_job`, including the whole durability path.
`src/server/store.c:finish_fragment_put` explicitly fsyncs each fragment and
then fences its containing directory before success. Production opens O_DIRECT,
not O_SYNC; removing an O_SYNC flag is therefore not the next optimization.
Ancestry proofs and directory group barriers already exist. Likewise, the client
already sends fragments in parallel; merely proposing parallel replica sends
would duplicate existing behavior.

**Best-supported bottleneck: waiting for durable PUT completion, with substantial
co-running metadata/flush contention.** Pressure indexing removed CPU overhead
but does not shorten these barriers. This capture does not prove the fraction
spent in writer admission versus individual file sync versus directory waiting;
per-stage native timing is needed to separate them. No retry storm or long REPORT
stall was observed in the matched write RPCs.

The next candidate is a **bounded staged server PUT pipeline**: separate write
submission, completion of each required file barrier, and grouped directory
publication. Allow a bounded cohort of independent fragment writes to reach the
filesystem before waiting on their barriers, so file I/O can overlap and directory
barriers can cover more completed members. Preserve fixed byte/descriptor limits,
fairness and backpressure. This is a code change to scheduling, not widening
retry budgets, or acknowledging volatile data.
Existing concurrent writer slots may already achieve much of this overlap, so
an improvement is a hypothesis, not a promised multiplier.

First add cheap sampled queue/write/file-sync/directory-sync/ACK timers (no
per-operation logging) and compare an untraced baseline with/without the crawl.
Prototype only if queue/barrier serialization remains material. Every ACK must
wait for its own file data, checksum, length, directory durability and errors;
never substitute one file's fsync for another's. Batch completion must use exact
membership, retain buffers until completion, and fail affected members safely.
Keep same-fragment ordering and existing publication/uncertain ownership rules.

Acceptance before rollout: injected write/fsync/directory/close failures,
shutdown/cancellation with outstanding cohorts, bounded-memory and fairness
checks, concurrent overwrites, fsync/restart/full-hash checks, then identical
untraced random 4 GiB dd trials including final fsync. Report repeated medians,
PUT tail latency and barrier counts with fixed crawl state. Crash durability and
this proposed pipeline remain NOTRUN. No such code change was made here.

## Retained evidence

Local root:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-post-pressure-profile-20261014/`.
Remote root on each measured host: `/data1/efs/evidence/post-pressure-profile-20261014`.
Raw traces/perf/data are outside Git. `SHA256SUMS` covers initial host archives,
subsequent write traces and both workload results. Valid write trace SHA-256:

* AMD: `397f41dd8301cb05303cb95d7d9a669dc23f1b153c532d67b9498aebd5be8cd3`
* Intel1: `c1ea21b20f1958053ab349456f9811f7045bc7b311f7dddd4dfa7ba07a73e651`
* Intel2: `9184ad5efa24d0018c75453ac92db22fe3f6822792b8348297b723be08ca0ef7`

The captures date 03:55–04:01 UTC October 15 (October 14 EDT); artifact names
retain the EDT start date. No new full POSIX suite was run for this diagnosis.

**Raw evidence note (2026-10-23):** the bulk data in `post-pressure-profile-20261014` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
