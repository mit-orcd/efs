# Intel/AMD read coordination — 2026-10-15

**Measurement PASS; optimization and historical regression attribution NOTRUN.**
Follow-up to [the read profile](intel-amd-read-profile-20261015.md), not a new
correctness acceptance gate. No EFS code, configuration, mounts or deployment
changed. TCP/direct/files; .efs client/server, packing, containers and expiry
OFF. Tested source `bb22e9559337a836856dce1720b739743f0ef2d7`, build
`bb22e9559337`, timestamp `2026-10-15T12:10:00Z`. Binary hashes, Intel storage
and deployment details: [eight-writer deployment](intel-default-eight-writers-20261015.md).
Intel main daemons 3908856/3908863 and 4173451/4173458 remain on ports
17432–17433, 400 GiB/node. AMD client 1127604, libfuse 3.18.2, mounts
`/data1/efs/intel-mnt`; AMD NIC 2.5 Gb/s, Intel NICs 5 Gb/s. Private Intel and
AMD-local deployments untouched.

## Untraced scaling

Stable existing `001.dat` is 10 GiB. Read its first 4 GiB to /dev/null, split
into disjoint ranges across independent dd processes. Order 1,2,4,4,2,1;
fixed total bytes per trial, no tracing, no cache dropping. All 14 processes
exit zero (24 GiB total). This checks successful reads, not content hashes.

| Streams | Trials, MiB/s | Median MiB/s | AMD NIC receive MiB/s |
|---|---|---|---|
| 1 | 110.53, 111.09 | 110.81 | 163.88–164.71 |
| 2 | 160.87, 160.49 | 160.68 | 237.96–238.52 |
| 4 | 194.86, 195.14 | 195.00 | 288.91–289.33 |

Four streams achieve 1.76x single-stream throughput. This supports a per-stream
coordination cost. At four streams, AMD approaches its raw 298 MiB/s link
ceiling; this is no longer evidence of abundant network headroom. Whole-host
NIC RX/logical bytes is consistently 1.4827; zero new Ethernet errors/drops.
Counters are host-wide, not a packet capture or a retry count.

## Function and syscall measurements

Separate 12-second root BPF capture during a 4 GiB single-stream read:

| Function/category | Completed calls | Total ms, across threads | Mean us | Max us |
|---|---:|---:|---:|---:|
| `efs_client_pull_chunks_range` | 4038 | 4927.956 | 1220.40 | 17317 |
| `efs_client_node_is_down` | 42478 | 117.103 | 2.76 | 368 |
| `efs_client_conn_get` | 29587 | 80.148 | 2.71 | 40 |
| per-node `pthread_mutex_lock` | 117515 | 80.246 | 0.68 | 366 |
| `maybe_prefetch` | 2724 | 9.136 | 3.35 | 23 |
| `efs_rdcache_invalidate` | 13302 | 20.196 | 1.52 | 29 |
| `efs_client_read_refs` | 2630 | 1.662 | 0.63 | 15 |

No completed `efs_client_pull_layout_miss` or decoded-cache
`pthread_cond_timedwait` calls were recorded. Warm future mappings can explain
absence of layout-miss calls; mandatory range refreshes still occur. Absence
of pending waits does not establish prefetch hits. Totals include nested work
and probe overhead and must not be added or called critical-path percentages.
Integer-microsecond rounding affects short calls. Entire probed dd completed
4 GiB in 37.4234 seconds, exit zero.

Independent 12-second futex trace: per-node connection locks have 221 wait
syscalls, 4.303 ms total, of which 211 immediately return EAGAIN and only ten
return success. No connection-pool-capacity or decoded-cache CV waits.
GET-worker queue waits are mostly idle workers; their aggregate time is not
active read latency. These findings weaken the prior lock-contention theory.

Independent eight-second RPC trace: 2473 complete chunk-map exchanges average
1.149 ms (p95 1.397 ms); 14953 complete fragment GET exchanges average 2.964 ms
(p95 4.108 ms, max 128.312 ms). All these replies have status zero. No exchange
exceeds one second. Nonblocking recv EAGAIN is not an EFS failure. Timings include
ptrace overhead. `GETCHUNKS_PACKED` is the metadata wire opcode and does not
mean small-file packing is enabled.

## Concrete split and redundant traffic

A second eight-second trace retaining 48 payload bytes shows 819 pairs of FUSE
replies: **1,048,560 data bytes followed by 16 data bytes**, plus a 16-byte FUSE
header on each reply. A separate three-second /dev/fuse read trace confirms
kernel READ request sizes of 1,048,560 and 16 bytes. Thus the application 1 MiB
read is split before EFS processes it. Neither size is aligned to the 128 KiB
chunk, explaining the immediate rejection by `efs_client_read_refs`. The
suspected double range lookup through that zero-copy path is **not supported
by this capture**.

The detailed RPC capture has 14,862 GETs for 13,164 distinct
(export,inode,chunk,fragment,generation) identities: 1,698 identities fetched
twice. Repeats include chunks 7,15,23,31 (both data fragments), consistent with
refetching the final chunk for the 16-byte tail. Duplicate requests alone do
not identify retries, prefetch ownership or all causes; no failure/retry storm
is established. Partial capture boundaries are retained as a limitation.

Received syscall bytes in that window: 974,429,640 fragment reply bytes and
241,741,842 chunk-map reply bytes. Metadata is **19.88% of those two reply
categories**, about 98 KiB per map request. They exclude link/TCP headers;
do not equate these byte totals with the separate untraced NIC windows.
This traffic, plus repeated data fetches, helps explain why useful throughput
is substantially lower than link throughput.

## Recommendation and historical lead

First qualify a chunk-aligned FUSE READ limit/negotiation so a 1 MiB application
read does not produce a 16-byte tail. Trace the kernel/libfuse limit before
choosing the exact setting; the limiting layer is not yet established. Merely
increasing workers or removing fsync is not supported by these read results.

In parallel, design safe reuse of decoded/prefetched data after a metadata
refresh proves the complete read identity unchanged. `ops.c:apply_chunk_recs`
unconditionally calls `efs_rdcache_invalidate`; that function clears both
identity and pending state. The measured 13,302 invalidations establish frequent
execution, not that each discarded a valid entry. Demand `read.c` deliberately
refreshes every batch because peer spans can change without a base-generation
change. A sub-chunk read can refresh once in its fast path and again in its
fallback batch. Avoiding redundant refreshes within one validated read and
preserving an unchanged decoded image are concrete candidates.

Commit `033a842ab6056c42ae47e93207c4543dfcc30d5f` (October 6) introduced the
unconditional invalidation. This is a plausible lead for the reported older
speed, **not a proven regression culprit**: no historical binary A/B was run,
and that commit contains correctness fixes. Do not revert it or remove metadata
refreshes. Equality of base generation alone is insufficient: span/read-view,
fence/publication state, placement and checksum identity must be covered;
in-flight fetch completion and invalidation races must remain safe.

Before accepting a future fix: paired one/two/four-stream runs with identical
fixtures and cache treatment; native and mounted peer overwrite/span/fold,
truncate/regrow, partial/chunk-boundary reads, retained-open/unlink reads,
publication failure and cancellation tests. Compare fragment identities and
metadata bytes, not throughput alone. No speedup from a proposed fix is claimed.

## Reproduce and retained evidence

For each N in 1,2,4,4,2,1, concurrently run i=0..N-1:
`dd if=/data1/efs/intel-mnt/001.dat of=/dev/null bs=1M skip=$((i*4096/N))
count=$((4096/N)) iflag=fullblock status=none`. Measure monotonic wall time
around launching all processes and waiting for all; throughput=4096/seconds.
Inventory processes and stable fixture first. Never substitute an output file.

During separate bounded reads, attach to the current Intel-export client PID:
`timeout -s INT 12 strace -f -ttt -T -e trace=futex -p PID -o TRACE`;
`timeout -s INT 8 strace -f -ttt -T -yy -xx -s48
-e trace=sendmsg,writev,recvfrom -p PID -o TRACE`;
`timeout -s INT 3 strace -f -ttt -T -xx -s96 -e trace=read -p PID -o TRACE`.
Use `scripts/summarize_intel_rpc_trace.py` for exchanges. Decode GET request
payload as little-endian `I4xQIIQ`; FUSE READ opcode 15, offset at byte 48,
size at byte 56 after its input header. Restrict decoding to complete syscalls.

For BPF timing, root attaches entry/return uprobes to the named functions and
libc mutex/condition calls, filtering client PID. Store nsecs keyed by TID,
accumulate count, sum and max of (return-entry)/1000; exit after 12 seconds.
Resolve PIE symbols and filter conn_lock and rdcache.cv address ranges from
the actual binary/process; never reuse this run's addresses after restart.
Numeric aggregation keys avoid the initial string-key probe's BPF load error
-28. The corrected root preflight and capture succeeded and detached. All
owned workloads and probes ended; no service changes or payload cleanup needed.

Raw artifacts (not committed): local
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-read-coordination-20261015/`;
remote AMD `/data1/efs/intel-client/read-coordination-20261015/`.
Includes measurement scripts, failed preflight, final probe log, stream results,
syscall traces and parsers. SHA-256:

- `streams.jsonl`: `babd8f1dce7e79086472d72750ae71e230ebb32e66a7a0d1b8c1573d36e71ea4`
- `probe-results.log`: `4d39e6ba1e31978fd62550d8c64af26bb1ffe621ec6d12aab9bbd96ad137844f`
- `rpc-detail.trace`: `c01e002e6c18902d7747da0acf779f41d5bb95a4ec0a9e09c79d05fd883b2d41`
- `fuse-read.trace`: `020bb658b52c301e714acb4fc203daf8646ba4def1efbb42a5f100611d74c185`
