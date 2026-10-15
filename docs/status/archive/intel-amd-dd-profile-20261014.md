# AMD dd to Intel — 2026-10-14 EDT

**Historical diagnostic capture PASS.** Subsequent implementation and smoke
measurements are in the [batching deployment report](../intel-sync-batching-20261014.md).
At this capture, performance fix/A-B qualification was NOTRUN.
Investigate the reported approximately 50 MiB/s single-file write. No source,
configuration, mount, workload or service changes were made. This is performance
evidence, not a filesystem acceptance verdict.

## Deployment and workload

Deployed source `c775fb15e2530399e052cf48432d6e495a039fb0`, build ID
`c775fb15e253`, build timestamp `2026-10-15T02:09:33Z`; see the
[deployment report](../intel-block-hash-index-20261014.md). Four files-engine
daemons on two Intel hosts, two 400 GiB quota nodes per host; TCP/direct-I/O,
`.efs` client/server, packing, containers and expiry disabled. Independent
AMD-local and private Intel clusters were not targeted.

Existing human command, observed without restarting it:

```sh
dd if=/dev/urandom of=/data1/efs/intel-mnt/big-files/001.dat bs=1M status=progress
```

AMD dd PID3672678; Intel-export FUSE PID3587317. Concurrent xorinox four-worker
`tiny_files_md5.py` crawl PID713960 remained active. This is a loaded observation,
not an isolated disk or network benchmark. The file advanced from about 10.4 to
11.4 GiB in the main 30-second sample; the command has no count/fsync option.
Application write progress is not a completed fsync-throughput measurement.

## Findings

AMD routes 10.0.0.207 through `enxc84d4435029d`, source 10.0.0.205;
reported link speed is **2,500 Mb/s**, not 10 Gb/s. Intel Ethernet is 5,000 Mb/s.
AMD's raw ceiling is 298.0 MiB/s (0.291 GiB/s). Default 128 KiB data chunks
encode into three 64 KiB fragments, sent by the client: 1.5 times the logical
payload, giving an optimistic logical ceiling of **198.7 MiB/s** before headers,
metadata and other traffic. **Greater than 1 GiB/s cannot fit this path.**

Main low-overhead sample, October15 02:27:18–02:27:55 UTC (host starts staggered):

|Observation|Result|
|AMD dd progress, 30 seconds|1,027 MiB; about 34.2 MiB/s|
|AMD Ethernet TX / RX|52.6 / 4.15 MiB/s|
|AMD client CPU / dd CPU|about 8% / 7% of one CPU|
|Intel daemon CPU n1/n2/n3/n4|about 25% / 90–93% / 37% / 31% of one CPU|
|TCP retransmissions, AMD / Intel1 / Intel2|0 / 2 / 0, host-wide|
|Ethernet RX/TX errors and drops|zero increments on all three hosts|

The 1.54 wire/logical ratio is consistent with erasure-code payload overhead.
The link was far below capacity. Two Intel1 retransmissions among approximately
1.39 million outgoing TCP segments do not support packet loss as the principal
explanation; counters are host-wide and do not identify those flows.

In the separate 12-second syscall summary dd spent 10.12 aggregate seconds in
402 writes versus 0.668 seconds in 381 reads. Reads averaged 1.75 ms; writes
25.2 ms (includes progress writes). Thus random generation was not the dominant
observed delay. Thread sleep/futex totals overlap and must not be added into
an elapsed-time percentage. The client was mostly waiting, not CPU-saturated.

Separate ten-second header trace, all paired complete replies:

|RPC|Count|Mean|p95|Status|
|PUT_CHUNK|8,272|16.89 ms|48.36 ms|all OK|
|REPORT_CHUNKS|1,340|5.34 ms|14.88 ms|all OK|
|GETCHUNKS_PACKED|299|1.83 ms|4.40 ms|all OK|

No captured exchange exceeded one second. This does not establish an EFS retry
storm: no paired error replies were seen. 86 receives lacked a captured send;
33,541 nonblocking EAGAIN returns are not failed RPCs. Tracing adds latency.
The GETCHUNKS_PACKED opcode name does not mean file packing is enabled.
606 REPORTs carried zero chunk records (inode metadata), 160 one, 149 two,
135 three; larger batches were uncommon. These are not proven duplicate RPCs.

A separate five-second n1 path trace completed 1,099 fragment-file fsyncs
(mean7.21 ms), 1,101 directory fsyncs (mean2.49 ms), 408 Raft-log fdatasyncs
(mean4.42 ms), and 376 KV-WAL fdatasyncs (mean4.28 ms). Aggregates overlap across
threads. The code explains these barriers: `src/server/store.c`
`finish_fragment_put()` explicitly syncs the fragment, then its parents before
success, including direct-I/O production writes. Direct I/O alone is not a
persistence barrier. `src/client/write.c` fans each chunk to three peers and
waits for completion/quorum; publication goes through REPORT/Raft.

Client logs repeatedly show high-water pressure near 32–38 MiB live bodies,
while the hard body limit is 320 MiB. `src/client/efs_fuse.c` pressure handling
can synchronously drain an inode through queued writes, PUT and REPORT before
admitting more data. This is evidence of backpressure, not exhaustion of RAM.
The exact contribution of pressure drains versus normal background publication
has not been isolated by a controlled experiment.

Intel1 user-cycle samples still include metadata comparisons/probes/scans;
its busier n2 is near one CPU. The concurrent MD5 crawl also generates metadata
work, so the profile cannot attribute all server metadata cost to dd.

## Recommendation and remaining proof

The strongest supported explanation is **durable small-fragment latency and
frequent publication/backpressure**, with shared metadata load, rather than
bandwidth saturation, random generation, or an observed retry storm.

Investigate bounded batching/coalescing of sequential-file publication and
shared durability barriers. Preserve ownership, bounded memory, fsync/close
semantics, data-before-metadata ordering, and durable acknowledgements. A shared
directory barrier may only acknowledge operations proven covered by that
barrier; do not simply remove syncs or enlarge retry budgets. Do not infer
that the current fragment-per-file design can share distinct file fsyncs
without an additional valid persistence mechanism.

Before selecting a fix, compare the same nonzero-data sequential workload with
and without the competing crawl, recording PUT/REPORT batch sizes, queue/pressure
waits, sync latency and completed fsync throughput. Then measure a bounded
batching candidate against the exact baseline, including crash/fault tests for
its acknowledgement boundary. Expected speedup is unproven. `/dev/zero` is not
a valid network-write substitute here: the client elides all-zero chunk PUTs.

## Reproduce the observation

Requires same-user SSH, perf and strace permissions; confirm current PIDs and
commands first. Use a new output directory, without restarting the human writer:

```sh
ssh amd_efs 'bash -s -- /data1/efs/NEW-dd-profile CLIENT_PID,DD_PID 30' < scripts/profile_intel_live.sh
ssh intel1_efs 'bash -s -- /data1/efs/NEW-dd-profile N1_PID,N2_PID 30' < scripts/profile_intel_live.sh
ssh intel2_efs 'bash -s -- /data1/efs/NEW-dd-profile N3_PID,N4_PID 30' < scripts/profile_intel_live.sh
```

The committed helper captures 30-second perf/counters, then 12-second strace
summaries and detaches. For RPC headers:

```sh
timeout -s INT 10 strace -f -ttt -T -yy -xx -s 8 -e trace=sendmsg,writev,recvfrom,poll,clock_nanosleep -p CLIENT_PID -o rpc.trace
python3 scripts/summarize_intel_rpc_trace.py rpc.trace
timeout -s INT 5 strace -f -ttt -T -yy -e trace=fsync,fdatasync -p N1_PID -o sync.trace
```

Timeout exit124 is expected. Retrieve only completed captures; detach all
profilers. No generated workload files need cleanup; leave the user's file/job
untouched. Raw evidence is external to Git at
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-amd-dd-20261014/`;
remote profile directories `/data1/efs/intel-amd-dd-profile-20261014` on the
three observed hosts. SHA256:

- AMD archive: `d6f4b6fe5e187736870bd9f49c54c89f8e8ea8c5ac5be78bdcf78f0a998d9be3`
- Intel1 archive: `2442e6220466ff7719fe3c0c1f0d93d02d531b7a0c20c15c3b6c9772d338e8f6`
- Intel2 archive: `c2831ca510ae0210d28897334d3f3a11e9a2b27110bddc58fdc03627c04d7aa8`
- RPC trace: `ad22bf4cb6e13585fe9526e645498da91f59983c3efc4f999e6712070fc755bb`
- Sync trace: `96219a710558da11819a68517017bee5f79063711f80f606bbb059417703bda9`
