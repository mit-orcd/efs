# Intel live profile after serving-route deployment — 2026-10-14

**Historical observational capture PASS.** The recommendation was subsequently
[implemented and smoke-qualified](../intel-block-hash-index-20261014.md). Follow-up to
[the mounted-stat routing report](../intel-mounted-stat-routing-20261014.md).
Goal: identify the next performance change with the human tiny-file workloads
running. No code/deployment/configuration change, restart, remount or workload
pause occurred. All main and unrelated process inventories remain identical.

## Source, environment and workload

Deployed source `f6b980a6e5f511e5acef74ae3910291cd73b1730`, build
`f6b980a6e5f5`, version `v0.3.2-pre-alpha-461-gf6b980a6e`, build metadata
`2026-10-14T21:54:40Z`. Binary hashes are recorded in the linked deployment
report and reverified by process inventory here. Four files-engine daemons,
two per Intel Core Ultra7 256V host (eight logical CPUs/host), 400 GiB/node,
about 116 GiB used/node at the preceding deployment. Intel links 5 Gb/s;
physical AMD and xorinox clients use 2.5 Gb/s Ethernet. Rocky10.2/Linux6.12
on Intel/xorinox, Ubuntu/Linux7 on AMD. TCP/direct I/O;
EFS_COMMANDS=0, EFS_COMMANDS_SERVER=0, EFS_EXPIRY=0.
 Private Intel and AMD-local clusters excluded from capture.

Existing jobs, four workers each:

- AMD PID1042572: `python3 ./tiny_files.py --resume --max-depth 8 --max-files-per-folder 1000 --max-files 1000000 --workers 4 /data1/efs/intel-mnt/tiny-files11`.
  Writes/closes 1–512-byte files; no explicit application fsync.
- xorinox PID676802: `python3 ./tiny_files_md5.py --workers 4 --output /data1/efs/intel-client/logs/md5-route-final-20261014.jsonl /data1/efs/intel-mnt/`.
  Read/hash crawl, not a second writer. Intel-local mounts essentially idle.

Node2 led both metadata groups throughout: group0 term2204, group2 term2985.
Final status healthy 4/4 matching daemons. After capture writer reached
413,854 files; crawl 50,923 hashes with changed=0/errors=0. Progress is evidence
of running jobs, not a matched throughput comparison or complete integrity gate.

## Measured results

Around 18:05 EDT (22:05 UTC), 30-second 99 Hz userspace cycle profiles and
per-second wait states on all eight main processes. Separate 12-second
wall-clock syscall summaries followed, then separate 10-second RPC header
traces and 10-second application syscall summaries on the external hosts.
Hybrid Core/Atom PMUs report separately. Inclusive CPU shares are nested,
not additive; wall-clock syscall sums overlap threads. Tracing perturbs latency.

|Observation|Result|
|---|---|
|Intel1 CPU during perf|n1 21.3%, n2 105.3% of one CPU; local FUSE 0.1%|
|Intel2 CPU|n3 33.3%, n4 11.1%; local FUSE approximately 0%|
|External client CPU|AMD 6.1%, xorinox 9.0% of one CPU|
|Intel1 inclusive user cycles|LOOKUP 46.9–51.9%; GETATTR beneath it 42.1–47.0%; LSM get 35.1–38.9%; merge scan 16.1–17.4%|
|Intel1 self cycles|memcmp 15.1–16.6%; cached block search 8.3–12.3%; segment probe 4.9–6.9%; memtable position 5.0–6.4%|
|AMD application|1,294 openat at 15.496 ms mean; 1,295 close at 13.537 ms; 1,305 write at 0.038 ms|
|xorinox application|1,728 openat at 10.186 ms; 1,468 read at 7.592 ms; 1,468 path stat at 6.970 ms|
|Ethernet RX/TX MiB/s|Intel1 11.44/43.18; Intel2 11.54/6.59; AMD 10.44/19.34; xorinox 39.69/0.35|

No application open/read/write/close errors in these bounded summaries.
The metadata leader is busy around one CPU, not saturating the entire host.
Low client CPU and slow open/close/stat support metadata/control-path waiting;
these observations do not prove one exclusive end-to-end bottleneck.

Host-wide TCP RetransSegs increased by 1 on AMD, 0 elsewhere over 30–31 seconds.
Used Ethernet interfaces had zero RX/TX error/drop increments. Unused Wi-Fi
interfaces had 51–52 RX drops on Intel/AMD, so “all interfaces had zero drops”
would be incorrect. This is no evidence of an Ethernet loss bottleneck and
not proof of loss absence over all time or for every flow.

### RPC observations

AMD: 2,529 REPORT replies all OK, mean 2.642 ms, p95 6.922 ms, max 79.692 ms.
There were 1,264 one-chunk reports (449-byte frames) and 1,265 zero-chunk
reports (97-byte frames): distinct publication shapes, not duplicate retries.
All 3,792 PUT replies succeeded, mean 4.361 ms. CREATE_RETAINED had 1,265 BUSY
and 1,265 OK: consistent with the explicit unknown-receipt probe followed by
admission/submission in `efs_client_create_resume()`, not 1,265 failed creates.
LOOKUP had 4,259 OK and 1,282 NOT_FOUND, consistent with new names; all 1,265
GETATTR replies succeeded. No decoded REPORT BUSY, NOT_PRIMARY or STALE storm.

xorinox: 8,646 LOOKUPs all OK, mean 2.255 ms; 4,448 GETATTRs all OK,
mean 0.675 ms; 2,722 GET_CHUNK replies all OK, mean 2.091 ms. All other
completed decoded replies succeeded. No fully captured RPC on either client
exceeded one second. Partial exchanges are excluded by the summarizer.
`RAFT_MKFS` is also the session-command envelope; its appearance is not a format
operation.

## Recommended next change

Prioritize **a bounded exact-key hash index inside already-cached immutable
LSM blocks**, extending the existing record-offset index in
`src/kv/kv_seg.c:index_block/search_block`.

The current candidate index skips irrelevant segments and narrows L1, but
surviving cached point gets still binary-search record offsets and compare
variable-length keys repeatedly while holding the LSM mutex. The profile
places substantial self CPU in this exact path, including metadata reduction
and its independent validation read. Existing memtable prefix acceleration
does not cover these segment-block searches.

Build the optional index once after full block validation. Hash the complete
key, resolve collisions with full key/length comparison, and include tombstones.
Bind ownership/lifetime to the cached block and impose an explicit byte budget;
allocation refusal or a missing index must fall back to the current binary
search. Keep source precedence, private snapshot paths, error handling and
fresh Raft/transaction/sequence validation unchanged. No attribute TTL, disk
format change or optional feature is required. Full-key hashing avoids assuming
that short prefixes discriminate inode/lane keys, which share leading bytes.

This is a hypothesis, not a promised percentage. Search self CPU alone is
8–12% of Intel1 samples; eliminating half of only that component saves roughly
4–6% of sampled Intel1 CPU, not 4–6% guaranteed throughput. Some key-comparison
cost may also shrink, but the entire 35–39% LSM share is not removable. Measure
index build cost, extra memory and eviction churn as well as warmed lookup cost.

Qualification before rollout:

1. Differential point-lookup results against binary search: hits/misses,
   collision-heavy keys, mixed lengths, embedded zeros, PUT/DEL precedence,
   repeated versions and malformed/truncated blocks. Corruption remains error.
2. Allocation refusal, eviction/reload, compaction/pinned segment lifetime,
   private snapshots and concurrent readers/writers; sanitizer checks on the
   affected native paths. Keep a strict memory bound.
3. Matched actual-LSM microbenchmark for warm hits/misses and cold churn;
   then same-source/configuration mounted A/B with stable leadership/catalog,
   untraced timing, followed by separate profiles and byte/EOF/fsync smoke.
   Include mixed tiny-file create/read load and report tails/RSS/CPU per file.

Second candidate: remove repeated whole-block copies for short prefix scans
(`kv_seg_iter` currently copies a cached block into its private buffer).
One PMU attributes 6.7% of Intel1 samples to memmove beneath scan advancement.
This needs explicit block pinning because callbacks and nested reads can evict
cache slots; naive borrowed pointers are unsafe. It is a separate follow-up.

Publication/session RPCs and durable syncs remain real costs, but this capture
does not justify retry-budget tuning or bypassing durability/authority. Client
hashing/allocation is lower priority given 6–9% absolute client CPU.

## Reproduction and evidence

As the efs account, with verified current PIDs and a new output directory:

```sh
bash scripts/profile_intel_live.sh NEW_OUTPUT PIDLIST 30
# PIDLIST in this capture:
# Intel1: 3855183,3855190,3855890
# Intel2: 4127354,4127361,4128153
# AMD: 1010027; xorinox: 675989
# After perf/syscall summary finishes, separately:
timeout -s INT 10 strace -f -ttt -T -yy -xx -s 8 \
  -e trace=sendmsg,writev,recvfrom,poll,clock_nanosleep \
  -p CLIENT_PID -o RPC_TRACE
python3 scripts/summarize_intel_rpc_trace.py RPC_TRACE
timeout -s INT 10 strace -f -c -w -p APPLICATION_PID -o APPLICATION_SUMMARY
```

Timeout exit124 is expected; verify successful attachment and detach. Require
same-user ptrace/perf permissions and native symbols/frame pointers. Preserve
both human jobs and exclude unrelated local/private clusters. Compare process
CPU ticks and host counters between before/after-perf snapshots; percentages
here use Linux USER_HZ=100 and snapshot elapsed time. No raw capture is needed
to reproduce the procedure. All attachments finished; no profiler left running.

Raw evidence outside Git:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-post-route-profile-20261014/`.
Remote evidence `/data1/efs/intel-post-route-profile-20261014/` on each host.
Complete perf/summary archive SHA256:

- Intel1 `82c0143fb2ddf8d0a89533e552503d689a31e5cc42bb6f326e4010341d19d42e`
- Intel2 `99dd28035155ce7802ec6c9576724de7e69237d3e5ec88faa3e6a1ac055d19a5`
- AMD `a06477500e2edfa2c43f58fe478aa363c7d441d52cdb613c71bf7e5c7f76d5ac`
- xorinox `9546391edc50d35493aa54e742cde8f3390fe6c1f22d83c93505a5c964591938`

Later RPC/application captures and decoded summaries are separate files beside
those archives. No new optimization implemented or deployed; no correctness or
acceptance suites run for this observational task.
