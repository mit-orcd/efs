# Historical status snapshot — Oct 7, 2026

Archived during the timed documentation review. Dated implementation and
cluster claims below are historical evidence, not the active work order or
current authorization. Use [the status index](../status/README.md) and
[the current handoff](../status/in-flight.md).

---

# FUSE client memory issues — opened Oct 5 2026

[Status / work queue](../status/README.md) · [Related decisions](../status/decisions.md)

**Status: open; local C fixes implemented and tested, live gates pending.**
See the implementation checkpoints below for the current state. The original
review and proposed mechanisms are retained as evidence.
These are separate defects from D23's clean-body retention fix and D27's
stalled-publication handling. The user requested documenting them after a
code-only review; the proposed mechanisms below are a plan, not a new decision
register entry or a claim that the fixes have landed.

## Evidence and scope

Review baseline: `v0.1.0-pre-alpha`
(`4d2264c166c813f930e4aa52341e87adb7329809`) versus `devel`
(`4e4c10ff0683eca7538e31aad657260d7fdd8d83`). Both defects are present
in both revisions; the core read/write/buffer-pool code is unchanged.

User-supplied kernel log, Oct 5 at 18:49:05 (timestamp as supplied): global
OOM killed `efs-fuse`, PID 43274, with `anon-rss:2599708kB` (about 2.48 GiB)
and `total-vm:6242628kB` (about 5.95 GiB virtual, not resident).
The gateway host has 2.8 GiB RAM and 2.8 GiB swap. The later `free -h`
reading does not establish available RAM/swap at the kill. The workload,
client configuration and allocation breakdown were not captured, so neither
individual defect is established as the cause of that specific OOM.

The default dirty threshold is 2 GiB, with extra inline pressure handling
only above 4 GiB; the separate read cache holds up to 1 GiB at the default
chunk size. That already leaves inadequate headroom on this small host.
Shared buffer slabs retain up to 2 GiB and can fall back to malloc; their
capacity backs cache bodies and must not be counted a second time as active
cache memory. Returning a chunk to the slab pool does not return RSS to the OS.

## Sparse dirty writes bypass reclaim and cache admission has no hard bound

**Priority: P1. Status: open; plan prepared.**

Sources: `src/client/write.c` — `dcache_store` / `dcache_store_owned`,
`dcache_load_sparse`, `dcache_reclaim_main`, `dcache_flush_slot_inner`,
`efs_dcache_maybe_reclaim`; `src/client/bufpool.c`.

- `DCACHE_SLOTS=65536` bounds buckets, not bodies: collisions allocate
  linked nodes. The comment claiming an unconditional 8 GiB cap is false.
- A partial append to a never-published chunk can allocate a full chunk
  with `have_base=0`. Background reclaim skips it; inline pressure uses
  `have_only=0` and skips it too. Many open files receiving small append
  writes can therefore retain dirty buffers until close/fsync, despite
  exceeding the reclaim threshold.
- `dirty_bytes` decreases when a snapshot goes in flight even though
  snapshot/replay storage remains allocated. It cannot enforce a resident
  cache-memory limit by itself.

**Validation so far:** an isolated harness using actual insertion/take
functions admitted 65,537 sparse entries, accounting for 8,590,065,664 bytes.
One-byte placeholder bodies avoided allocating 8 GiB; dependencies were
stubbed. This proves admission behavior, not an end-to-end workload/RSS
measurement. Reclaim exclusion was verified in source. Actual `bufpool.c`
with the RDMA registration hook stubbed retained two slabs (64 MiB capacity)
after 257 allocated/touched buffers were returned.

**Proposed fix:** reserve body capacity before admission; bound collision
metadata and track resident bodies, reservations, in-flight snapshots and
merge scratch. Leave bounded reserved capacity for draining so an exhausted
writer budget cannot deadlock reclaim. Queue sparse pressure work by inode
and use the existing write synchronization / append serialization / data
flush / REPORT ordering. Preserve bytes and ranges until publication commits
or another safe owner takes them. Wait for capacity outside locks the drainer
needs, and before acquiring append reservations where possible; unavailable
servers must lead to bounded admission errors while already accepted data is
retained. Do not simply remove the sparse-skip condition or discard bodies
on PUT success: D23 established that STALE replay needs them until REPORT.

**Gates:** concurrent admission and collisions obey the budget at every
ownership transition; many tiny appends across open files plateau without
requiring close to start draining; concurrent same-inode and peer partial
writes cold-verify with no missing/NUL bytes; PUT/REPORT failures, STALE,
quota errors, truncate/fsync races and shutdown neither lose accepted bytes
nor deadlock. Validate on Linux FUSE and the small host, with RSS plus cache
charges, slab retention, swap activity and workload/configuration recorded.
A cache bound alone is not a whole-process RSS bound.

## Read/readdir reply buffers leak when FUSE workers exit

**Priority: P2. Status: open; independent small fix planned.**

Sources: `src/client/efs_fuse.c` — `t_rd_buf`, `t_rdd_buf`, `ll_read`,
`ll_readdir`. Related working ownership pattern: `decode_frag_scratch`
in `src/client/read.c` uses a pthread-key destructor.

The reply buffers are heap allocations referenced by bare `__thread`
pointers. They are reused between requests but have no thread-exit
destructor. When a worker retires, its pointer disappears without freeing
the allocation. Repeated bursts with worker retirement can grow memory
throughout the mount lifetime. This is distinct from clean dcache-body
retention and cannot explain a write-only in-process benchmark crash.

**Validation so far:** an isolated harness executed actual `ll_read` with
cache misses and filesystem/reply operations stubbed. Thirty-two sequential
workers each read 1 MiB and exited; allocator accounting showed 33,554,432
bytes still allocated after joins. The readdir case has the same ownership
problem from inspection; it was not separately executed. No libfuse
worker-lifecycle integration test has run locally.

**Proposed fix:** a lazily allocated per-thread reply-buffer owner containing
both pointers/capacities, registered through a pthread key with a destructor.
Check key creation and registration failures; use temporary owned storage or
return an allocation error if registration cannot succeed. Preserve reuse,
capacity checks, realloc failure behavior and zero-copy pin/release.

**Gates:** repeated short-lived read and readdir workers leave no owned
allocations after joins; reuse/growth on a persistent worker remains correct;
allocation/TLS-registration failures leak nothing; zero-copy read hits keep
correct pin lifetimes. Run Linux burst/retirement testing with leak detection
or allocation accounting, and verify memory plateaus after warm-up.

## Implementation order and completion rule

1. Independently fix and validate reply-buffer ownership.
2. Add cache admission/accounting with reserved drain capacity.
3. Add safe sparse pressure drain and cold-read/failure gates.

This describes the order within this memory work, not a replacement for the
existing project's correctness queue. Keep the two issues separate in review.
They are complete only after implementation and the relevant gates pass;
small-host OOM prevention also needs measurement of the other resident memory
consumers. Do not claim either fix alone caused or resolves the recorded OOM.


## Implementation checkpoint — Oct 5 2026

C changes are in the working tree; local regression gates pass, Linux load gates remain owed. Worker reply storage now has a pthread-key owner/destructor. The shared chunk allocator enforces a 256 MiB admission budget plus 64 MiB drain reserve by default, tracks live bodies through dirty/read-cache/PUT ownership transfers, and bounds retained slab/heap backing. Dynamic dcache collision metadata has an independent 8 MiB bound. FUSE write requests reserve worst-case body and entry capacity before append locking/reservation; pressure uses the existing inode flush + REPORT coordinator and preserves data on failure. Clean committed spans are released only after the matching object and local snapshot sequence verdict; newer snapshots and re-dirtied ranges stay owned.

Configuration: `EFS_DCACHE_BYTES` is the soft reclaim target (default 128 MiB); `EFS_DCACHE_HARD_BYTES` defaults to 256 MiB; `EFS_DCACHE_DRAIN_BYTES` defaults to 64 MiB. The hard budget includes read-cache bodies and PUT/merge scratch, not only dirty bytes. Oversized-chunk exports may require larger explicitly configured budgets. This is not a total process RSS limit: fixed tables, bounce buffers, reply buffers, TLS scratch, connection/RDMA buffers and other metadata remain outside it.

`make test-client-memory` passes locally. ASan/UBSan tests cover worker retirement and allocation/TLS failures, concurrent admission, exhaustion/recovery of the reserve, reservation cleanup on thread exit, sparse pressure failure/success, failed PUT restoration, repeated-generation/newer-snapshot commit races, and pinned/pending read-cache ownership. The filesystem/RPC stubs do not establish cold-read correctness on Linux or performance.

D27 integration here is its memory-admission/retained-byte/recovery-reserve portion. The later runtime checkpoint adds publication-stall detection, per-open error delivery and controlled teardown. Strict whole-call deadline enforcement and remaining acceptance gates are still open; D27 is not closed. Linux sparse/append/concurrent-write/failure/recovery/cold-remount gates and the 2.8 GiB host RSS measurement are still required before rollout.


## REPORT/write backpressure checkpoint — Oct 6 2026

Implemented locally, uncommitted and undeployed. FUSE writes, direct client writes and creates reserve REPORT capacity before mutation. Live records and detached publication snapshots remain charged until acknowledgement; failed REPORTs restore their marks and original oldest age. Admission kicks the existing drain coordinator outside table/directory/append locks and waits within the caller deadline or `EFS_REPORT_ADMIT_MS` (default 8000 ms). Exhaustion returns BUSY. The write-worker queue also bounds admission waiting; an accepted job retains its buffers and completion state until the worker finishes.

Defaults: `EFS_REPORT_MAX_RECORDS=8192`, `EFS_REPORT_MAX_BYTES=16777216`. Each record receives a conservative 2048-byte workload charge; chunks consume two credits for their companion inode record. This is not exact allocation accounting or a process RSS limit. Other mutation APIs are not all admitted, so this does not establish a global bound on every metadata producer. Pressure counters expose pending/inflight/reserved records, charged bytes, oldest age, waits and timeouts; queue logs include jobs and queued/inflight bytes.

Validation: local REPORT/queue regression harnesses pass normally and under ASan/UBSan. Request-local create errors, client memory, lookup memo and fold observation regressions pass. Isolated Linux GCC runs of REPORT, queue and create-error harnesses pass; all three changed client C files pass strict syntax checks. Live load/failure/recovery measurements remain owed.

## Additional OOM evidence — xefsct1, Oct 6 2026

A 4 KiB random-write `dd` targeting 10 GiB lost its FUSE connection after about 1.4 GB. The full kernel report at 01:19:59 shows PID 2597 with 274364 KiB anonymous RSS and 702432 swapped pages (about 2.68 GiB on this host), with zero free swap. Swap is zram and consumes physical RAM. The installed binary identifies as `v0.1.0-pre-alpha-13-g4e4c10ff-dirty` and contains the hard/drain budget settings. The process environment and allocation breakdown at the kill were not preserved. The installed file alone does not establish the exact executable/configuration used by the killed process. This remains an open memory-accounting investigation; REPORT backpressure is not established as the remedy for this crash. Next gate: capture RSS plus VmSwap, allocator backing/credits, queue and publication pressure throughout a controlled sequential-write run.


## Serving-client deployment gap and FUSE hardening — Oct 6 2026

Read-only inspection proves xefsct2 still serves `/mnt/efs` with PID 9562,
started Oct 5 at 20:43:19, from `/tmp/efs/efs-fuse (deleted)`. Reading that
process's executable identifies `pre-alpha-12-g3d3f17c2-dirty`; its strings
lack `EFS_DCACHE_HARD_BYTES`. The installed replacement is newer. The killed
xefsct1 process belonged to session 16, opened Oct 5 at 20:35, whereas the
replacement executable was installed Oct 6 at 01:11. This strongly suggests
its mount also retained the old client, but its dead process image is no
longer available for direct verification. The earlier installed-file evidence
must not be read as proof that the killed process had the cache fixes.

`ct-up.sh` previously always shipped binaries but skipped an existing mount;
`gw-up.sh` shipped only when the installed binary was missing. Both now ship
current binaries/scripts and compare actual `/proc/<pid>/exe` bytes with the
installed executable before accepting an existing mount. A stale or missing
serving process fails with an explicit `--remount` instruction. Test-client
remount support is added; failed stops cannot silently retain a mount. Tar
pipelines check both endpoints and do not retry consumed streams. Shell syntax
and ten mocked stale/current/fresh/remount/sender-failure lifecycle cases pass.
No live mounts or workloads were changed during this investigation.

FUSE now uses libfuse's explicit active-worker configuration (requires
libfuse >= 3.12), respects `-o max_threads`, clamps idle workers to the active
limit and logs both limits. Unspecified zero fields fall back to 32 active / 8
idle; libfuse command-line defaults may already supply a nonzero active limit.
This is configuration hardening, **not evidence of the OOM cause**: on the
installed current libfuse, the old API's compatibility wrapper already supplies
an active cap of 10. [Upstream implementation](https://github.com/libfuse/libfuse/blob/fuse-3.17.4/lib/fuse_loop_mt.c)
confirms that behavior. Reusable decode scratch now uses the checked
pthread-key buffer owner, handling key creation/registration failure instead
of losing heap ownership. Worker configuration and owner failure/retirement
regressions pass; the full isolated Linux client builds successfully.

The next live memory gate must remount every participating client onto the
matching build first, record the **running executable**, and sample RSS plus
VmSwap and budget/queue/publication counters through the random-write run.
Keep that rollout/load gate separate from D25 unit/build results. The OOM is
not closed until the current serving binary's memory plateaus under load.


### Detached-client stop/remount correction (Oct 6)

The gateway retained PID 965740 and its active safe-stop socket after its
FUSE mount disappeared. The old executable and mount lock were deleted,
but the daemon still held /dev/fuse; a replacement correctly refused the
occupied control endpoint. Mount-only discovery in cluster stop missed it.

Local scripts now discover process mount arguments alongside mountinfo;
stop drains detached clients too, verifies endpoint ownership and waits for
graceful pidfd-based retirement. Cluster stop keeps servers running on any
discovery/drain failure and no longer bypasses refusal with lazy unmount.
Gateway/client --remount always runs controlled stop; ordinary startup
reports a detached client rather than attempting replacement. No socket
unlink or forced kill is used to override a live owner. Missing helper or
ambiguous process identification fails closed. The helper ships with the
EFS scripts; cluster changes live in ~/git/cluster. Cluster stop also copies
the two local stop tools to each reachable VM before discovery, so it works
before deployment to an older installation. Missing local tools and transfer
failures abort before server shutdown; no binaries are replaced by bootstrap.

Client-stop transport mocks cover detached success, drain refusal,
discovery failure and retirement failure. Actual process discovery and
endpoint-owner retirement pass in an isolated Linux synthetic-daemon test;
cluster-script mocks prove no server stop after failed discovery/drain and
remount preflight on an invisible client. Shell syntax checks pass. These
changes are uncommitted and have not been deployed; live gateway retirement
and remount remain owed.

## Read-path ENOMEM storm: prefetch queue can park the whole body budget — xefsct1, Oct 6 2026

**Status: root cause established by controlled A/B reproduction; no fix
implemented.** Filed as
[W60](../backlog/work-items.md#w60--a-concurrent-sequential-readers-prefetch-queue-can-park-the-entire-read-side-body-budget-demand-reads-then-fail-nomem-surfaced-as-eio-queue-row-0q)
(queue row 0q). Distinct from the write-side admission items above: this one
fails READS and surfaces to applications as EIO.

### Evidence

User workload on xefsct1 (2 vCPU test client, current-build mount pid
1096475, `v0.1.0-pre-alpha-62-gb4a75492-dirty`):

```
rg --hidden --no-ignore --stats 'search text' /mnt/efs/
```

rg reported `Input/output error (os error 5)` for thousands of tiny files
(clustered in `tiny_files*/folder_*/level_2/level_3/level_4/`), while the
servers logged zero errors (`iostats ... errors=0` on all three nodes). The
client log (`/mnt/efs-fuse-efs.log`) holds **19,112** `efs-fuse read: out of
memory (efs_rc=-2)` lines between 21:58:16Z and 22:57:01Z — the kernel-side
EIO is `efs_fuse_read_ino` mapping any `efs_client_read` error to -EIO while
the log keeps the real code (`src/client/efs_fuse.c:2166`). Per-10-minute
histogram: 950, 6433, 4879, 2085, 1665, 1704, 1396 — an onset ~40 min into
the scan, then decay as rg moved on and the queue drained. No kernel OOM and
no `staging table over EFS_CLIENT_META_MB` line in the window.

### Root cause (code-backed)

The shared client body pool (`src/client/bufpool.c`) caps demand reads at
`EFS_DCACHE_HARD_BYTES` (default 256 MiB; the 64 MiB drain reserve is
write/drain-only — `efs_buf_alloc` uses `g_hard` unless the caller holds a
full reservation or is draining). Every buffer charges a minimum of one
chunk (`buf_charge`, 128 KiB).

A sequential reader arms prefetch after two in-order reads
(`t_seq_run >= 2`, `src/client/read.c:1725-1732`). `prefetch_ahead`
(`read.c:1508-1553`) allocates one pool buffer per queued chunk
(`efs_buf_alloc` at :1536) and submits to the GET pool
(`get_pool_try_submit`). The GET pool queues up to `GET_POOL_QDEPTH=32`
jobs per shard × `GET_POOL_N=64` shards (`read.c:1194-1204`) = **2048
queued jobs × 128 KiB = 256 MiB — the entire read-side body budget**.
Prefetch allocation failure is silent (`return` at :1537-1539).

Any read whose range is not chunk-aligned (every sub-chunk tiny file, every
tail) needs a pool scratch buffer in the batch path (`read.c:1797`); at
budget exhaustion it returns `EFS_ERR_NOMEM` (:1802) with **no trim, no
wait, no retry** — unlike the write path, which trims the read cache,
drains and retries (`fuse_write_admit`, `efs_fuse.c:2900-2978`, the only
caller of `efs_rdcache_trim`). The error log line therefore blames the
victim (demand read) while the culprit (queued prefetch buffers) is
invisible.

So a scan that mixes one large sequential file with many tiny files — rg
over a tree holding the four ~10 GiB `00*.dat` files plus
`tiny_files2..9` — lets its own prefetch starve its own tiny-file reads.

### Reproduction (controlled A/B, Oct 6 2026, xefsct1)

Throwaway second mount of the same cluster with
`EFS_DCACHE_HARD_BYTES=33554432 EFS_DCACHE_DRAIN_BYTES=33554432` (64 MiB
total = 512 chunk buffers):

- tiny-file loop alone (`head -c 4` × 3000 files of `tiny_files9`):
  **0/3000 failed**, zero NOMEM logged.
- same loop while `cat 001.dat > /dev/null` ran concurrently:
  **2000/2000 failed**, first failure on file #1; 2000 NOMEM lines logged.

A 5-minute rerun of the user's full-root rg on the main mount (256 MiB
budget) produced no new NOMEM — with the full budget, 2 rg threads need
longer to pin the queue at cap; the original storm needed ~40 min.

### Initial fix directions (Oct 6 evidence)

- Read-path backpressure: on scratch-alloc failure, trim clean unpinned
  rdcache bodies and wait briefly (bounded), mirroring `fuse_write_admit`,
  instead of failing the read.
- Prefetch admission watermark: best-effort prefetch must never consume the
  last of the budget — skip when live+reserved exceeds, say, 75% of
  `g_hard`, and/or cap queued prefetch bytes (queue slots × chunk size is
  currently 100% of the read budget).
- Distinct log lines for prefetch-drop vs demand-read failure; the current
  single `read: out of memory` line hides the direction of the pressure.
- Note `buf_charge` makes a 3-byte file cost a full 128 KiB slot; sub-chunk
  bodies would raise the tiny-file ceiling ~4 orders of magnitude.

### Gate when fixed

The 64 MiB A/B above must flip to 0/2000 with the concurrent sequential
read running, and a mixed `rg` over the full xorinox tree (including the
10 GiB `00*.dat` files) must complete with zero `efs_rc=-2` lines.

### Oct 6 implementation checkpoint

W60 now uses atomic speculative admission at half the normal shared body
budget for both queued prefetch and reproducible read-cache copies. Speculation
cannot spend request reservations or drain capacity. Demand scratch first
reclaims unpinned clean bodies, then retries for at most twenty 1 ms waits
without index/cache locks. Pending fetches, reply pins and dirty bytes retain
ownership. This does not promise success under exhaustion by demand/dirty
owners; sub-chunk charging and a separate diagnostic counter remain follow-ups.

Allocator watermark/reservation/drain tests, sixteen concurrent speculative
allocators, and production demand recovery tests pass on the NUC. Existing
cache ownership and publication/session regression tests pass. The live
32 MiB mixed-read gate is pending: the first fixture write encountered a
protocol error against older NUC servers (`79983128`); align builds before
using that run as W60 evidence. Xorinox's full-tree gate remains owed.

### NUC live A/B — Oct 6, PASS

All four servers were aligned to `73ce8aaa-dirty` before this comparison.
A baseline client rebuilt with only `read.c`/`bufpool.c` reverted to pre-W60
code failed **2000/2000** tiny reads while reading a 512 MiB file repeatedly
(1,579,810,816 sequential bytes during the test; 2000 read-NOMEM log lines).
The fixed client on the same cluster/fixture/budget passed **2000/2000**
(1,781,792,768 sequential bytes; zero read-NOMEM lines). Both sequential
readers exited normally without errors. The normal budget was 32 MiB with
a separate 32 MiB drain reserve; no limit was raised.

The reusable [live gate](../../tests/live/read_pressure.py) additionally
checks deterministic sequential data and tiny-file contents. Full-root
Xorinox `rg` acceptance remains owed; this NUC gate does not claim it.

Verified-content NUC rerun: **0/2000 failures**, 27,414,757,376 sequential
bytes verified without error, both worker and harness completed normally.
Full NUC POSIX passed (216 pass / 0 fail / 1 unsupported-mmap skip), W36
passed 20/20 and the full Linux unit suite passed. See the
[ten-round checkpoint](../../results/measure/20261006-d25-w60-ten-rounds/SUMMARY.md).
