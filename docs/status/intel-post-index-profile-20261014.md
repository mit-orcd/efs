# Intel post-index live profile — 2026-10-14

**Diagnostic capture PASS; proposed optimization NOTRUN.** Follow-up to the
[LSM candidate index deployment](intel-lsm-candidate-index-20261014.md) and
[earlier live profile](intel-live-performance-20261014.md). Human requested an
independent assessment of the next performance hot path, including the theory
that metadata publication/retries leave bulk transfer underused. No deployment,
production code change, new workload, cache-policy change or retry tuning here.

## Source, environment and workload

Running source `6cbd99e549c8bd9576761556ef6b3b9ca8445e83`, build ID
`6cbd99e549c8`, version `v0.3.2-pre-alpha-455-g6cbd99e54`, build metadata
`2026-10-14T20:10:43Z`. Includes the checksum-tail EIO fix and candidate index.
The linked deployment report records all eight binary hashes and OS versions.
Two Intel Core Ultra7 256V hosts, eight logical CPUs each, four files-engine
nodes,400GiB quota/node, approximately99GiB occupied/node,5Gb/s Ethernet.
Two external physical clients have2.5Gb/s links: AMD Ryzen AI MAX+395 and
xorinox Ryzen9 7940HS. Four Intel main daemons and all four Intel-export FUSE
clients retain TCP, daemon direct I/O, `EFS_COMMANDS=0`,
`EFS_COMMANDS_SERVER=0`, `EFS_EXPIRY=0`;
Independent AMD-local and private Intel processes were excluded from profiling.

Observed existing human jobs, each with four workers:

- AMD PID133638: `python3 ./tiny_files.py --max-depth 8 --max-files-per-folder 1000 --max-files 1000000 --workers 4 /data1/efs/intel-mnt/tiny-files11`.
  Defaults create1–512byte files, write then close; no explicit application fsync.
- xorinox PID661407: `python3 ./tiny_files_md5.py --workers 4 /data1/efs/intel-mnt/`.
  This is a read/MD5 crawl, not a second writer. Both mounts are independent
  client deployments. Intel-local FUSE clients were essentially idle.

Process inventories before/after match exactly (PIDs, arguments, executable
hashes, feature environment), including unrelated clusters. Human jobs remain
running; all bounded perf/strace attachments detached. No production restart,
remount, storage modification or optional-feature activation occurred.

## Capture and results

At approximately20:33UTC:60s user-cycle perf at99Hz with frame-pointer stacks,
per-second thread wait states, process CPU/I/O and host network counters.
Separate12s wall-clock strace summaries per daemon/client followed. Separate
10s application syscall summaries,10s external-client RPC-header traces, and
6s sync-only daemon traces provide attribution. These phases are not one
simultaneous exact workload transaction. Ptrace affects latency; syscall wall
sums overlap workers and must not be divided into an end-to-end time budget.

|Observation|Measured result|
|---|---|
|Intel1 CPU|n1 21.6%, n2 110.7% of one CPU; GETATTR inclusive46.0–50.3% of host sampled user cycles, LSM get36.8–39.2%, prefix scan16.5–18.0% (nested, not additive)|
|Intel1 self CPU|memcmp17.0–18.8%, block binary search7.5–11.1%; reduction read29.3–34.3% inclusive. Hybrid Core/Atom PMUs reported separately|
|Intel2 CPU|n3 35.6%, n4 11.9% of one CPU; Raft/apply and fragment hashing more prominent; BLAKE3 self16.3–21.6%|
|AMD FUSE|6.3% of one CPU; BLAKE3 self29.9% of its sampled user cycles, FLUSH inclusive59.6%|
|xorinox FUSE|9.0% of one CPU; lookup21.1%, read23.5% inclusive; memset self15.6%|
|AMD application|openat1,402 calls averaging15.051ms; close1,402 averaging11.665ms; write1,414 averaging0.035ms|
|xorinox application|openat1,891 averaging10.149ms; path stat1,612 averaging6.906ms; read1,612 averaging5.206ms|
|Host network|Intel1 RX/TX11.73/45.86MiB/s; Intel2 12.01/7.13; AMD10.59/19.56; xorinox42.04/0.37. Far below link bandwidth|
|TCP/NIC counters|Zero RetransSegs delta on every host; zero physical-interface RX/TX error/drop deltas during60–61s window. Host-wide counters, not packet-loss proof for all time|

No application open/read/write/close errors in these syscall summaries.
Futex EAGAIN and expected mkdir EEXIST are not EFS failures. This is neither a
complete tree integrity test nor a reproduction of the other agent's bulk job.
No matched old/new throughput comparison is inferred from changing live jobs.

### RPC evidence: publication cost, but no demonstrated retry storm

[Offline trace summarizer](../../scripts/summarize_intel_rpc_trace.py) pairs
complete TCP request/reply frames and validates reply type. AMD completed:

- 1,597 REPORT replies, all OK; mean3.744ms, p9511.384ms, maximum45.83ms.
  Of these,799 requests carried one chunk (449byte frame),798 zero chunks
  (97byte frame). Do not classify two distinct successful publication shapes
  as retransmissions merely from their opcode count.
- 2,388 PUT replies, all OK; mean6.100ms, maximum76.458ms.
- 1,597 CREATE_RETAINED replies:799 BUSY,798 OK. The source's
  `efs_client_create_resume()` explicitly sends a route/receipt probe before
  admitting and submitting an unknown create. The server returns BUSY for an
  unknown probe. This observed near1:1 pattern is consistent with that normal
  two-phase protocol, not evidence of799 failed creates or timed-out retries.
- 3,076 LOOKUP replies:2,273 OK,803 NOT_FOUND, consistent with creating new
  names. No NOT_PRIMARY or STALE reply statuses in the decoded sample.

xorinox completed2,972 GETATTR,5,818 LOOKUP,2,743 GETCHUNKS,
1,825 GET_CHUNK,1,832 RAFT_MKFS and161 READDIR exchanges, all OK.
No fully captured exchange on either external client exceeded one second.
`RAFT_MKFS` is also the session-command envelope, not evidence of formatting;
First/last boundary exchanges are excluded:20 orphan
receives/client,6 pending AMD and4 xorinox. No reply-type mismatch, partial
send or malformed framing found. EAGAIN receives are ordinary nonblocking I/O.
Only8 bytes/buffer were captured; operation identity/payload replay cannot be
fully reconstructed. Durations exclude pool checkout and include tracer delay.

All four client log deltas were empty over roughly80–89s; daemon I/O counters
in captured logs reported zero errors. Log files neither rotated nor truncated.
Absence of retry diagnostics is supporting evidence, not exhaustive retry
instrumentation. **The current tiny-file workload does not substantiate
seconds-long REPORT/repeated-RPC-retry dominance.** Durable publication and
metadata round trips clearly cost time; a separate bulk-specific reproduction
would be required to validate the quoted bulk-write claim.

Sync-only traces confirm real durable costs, not just poll/futex waits:

|Host pair of daemons /6s|KV WAL fdatasync|Raft log fdatasync|Data-path fsync|
|---|---|---|---|
|Intel1|4,171; mean1.310ms|5,128; mean1.277ms|4,047; mean0.556ms|
|Intel2|1,931; mean1.560ms|4,721; mean1.377ms|4,273; mean0.554ms|

All returned success; no sync exceeded74ms. Data-path includes fragment files
and their directory hierarchy; many directory calls are very short. Counts
are across threads, nodes and replication, not barriers per logical file.

## Recommended next change

**First experiment: combine GETATTR's first lane-value read with its reduction
read, preserving the independent second collect and all freshness checks.**
In `efs_meta_apply_getattr()` (`src/meta/meta_apply.c:6657`), for every active
lane, `efs_txn_reduce_read_ex()` first transactionally reads the lane and scans
pending reductions. GETATTR immediately transactionally reads the same lane
again to obtain sequence/present/mtime-generation fields, then reads it a third
time in the independent sequence validation collect.
`efs_txn_reduce_read_ex()` (`src/meta/txn.c:1196`) already has the full visible
lane bytes. An internal helper returning those bytes/status alongside the
reduction can remove the middle read. In the ordinary no-intent path each
transactional read probes an intent key and then the base key: lane point gets
fall from six to four, with the pending-reduction scan retained. That is a
one-third reduction of these lane reads, **not a promised one-third workflow
speedup**. Current ReadIndex authority reuse already exists; this proposal is
value reuse within one first collect, not a new long-lived attribute cache.

Preserve committed-but-unmaterialized reductions, pending-decision recheck,
second lane sequence collect, inode generation/content/size/mtime fences,
absence/error handling, present counts, and malformed-value refusal. Never
reuse the first value for the second collect or cache an absence/decision
across requests. Proposed qualification: focused GETATTR/reduction tests with
interleaved lane writes, truncate, utimens, pending commit/abort and generation
change; mounted exact EOF/bytes/stat smoke; then identical isolated four-worker
writer and MD5 workloads with untraced throughput/latency plus bounded perf.
Gate on correctness and reduced KV calls before claiming improvement.

Second candidate: investigate durable metadata/session RPC amplification and
safe group commit/coalescing, keeping acknowledgement/replay/retained ownership
semantics. This is more invasive than removing a redundant read. Capture
operation identities before calling successful paired REPORTs duplicates or
removing retained-create probes. Do not widen windows, raise retry budgets,
weaken syncs or change visibility to manufacture throughput.
Client fixed128KiB chunk hashing/encoding is another CPU cost for tiny writes,
but6.3% total writer CPU makes it a lower-priority latency target here. No
checksum removal or expiry feature is proposed.

## Reproduction and retained evidence

Requires same-user ptrace access, perf/strace installed, native frame-pointer
builds, and active human-approved jobs. Resolve current PIDs, checking command
line/export/profile before each attachment; never reuse these stale PIDs blindly.
From the repository, copy `scripts/profile_intel_live.sh` to each host and run:

```sh
bash profile_intel_live.sh NEW_OUTPUT_DIR DAEMON1,DAEMON2,CLIENT 60 # each Intel host
bash profile_intel_live.sh NEW_OUTPUT_DIR INTEL_CLIENT_PID 60     # AMD/xorinox
# Separate phases, output directories already created by helper:
timeout -s INT 10 strace -f -c -w -p APPLICATION_PID -o OUT/application.txt
timeout -s INT 10 strace -f -ttt -T -yy -xx -s 8 \
  -e trace=sendmsg,writev,recvfrom,poll,clock_nanosleep \
  -p INTEL_CLIENT_PID -o OUT/rpc-headers.txt
timeout -s INT 6 strace -f -ttt -T -yy -s 0 -e trace=fsync,fdatasync \
  -p DAEMON1 -p DAEMON2 -o OUT/storage-sync.txt
python3 scripts/summarize_intel_rpc_trace.py LOCAL_RPC_TRACE > OUT/rpc-summary.json
```

Timeout124 is expected for bounded attachments. Compare process inventories
and network counter deltas; check tracers detached. Do not kill human workloads
as cleanup. Summarizer operates offline and prints aggregate results only;
raw trace headers can include payload prefixes and stay outside Git.

Raw evidence local:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-post-index-profile-20261014/`;
remote on each host `/data1/efs/intel-post-index-profile-20261014-2033/`.
Final retrieved archive SHA256:

- AMD: `97e2de6ccf9ef3140b58aede91f29b15fe3d8bedb5038b9124f11e02ec0a50da`
- Intel1: `7e40ccb41ab61ad9fdcfaf747bc74f962afac73a07beb8d2d602ccee09fa6c96`
- Intel2: `c12f7d24d8b5aeee6605d04f6ccc171d3c69b5c92dddbfc077259047457ba643`
- xorinox: `fa4483492f6e21f2d2e8b531d7ebcc97555e0951fe58f26e5afcf29c2ebe8e67`

No suites or new performance implementation run in this observational task.
