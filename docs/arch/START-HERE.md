# Start here — what to work on, and what to read first

[Architecture](../architecture.md) · [Roadmap](../scaling-roadmap.md) ·
[Development](development.md) · [Verification](verification.md) ·
[Product gaps](../product-gaps.md)

This page exists because of the bar in [development.md](development.md): **a
less advanced model must be able to contribute a correct change.** That is
only true if finding *the next task* and *the exact pages that govern it* is
mechanical. Read this page, then read at most the two or three files it
sends you to — not the whole spec.

---

## 1. The task right now

**Where the project is (Oct 1 2026).** [architecture.md §10](../architecture.md)
steps 0–12 are landed and gated: simulator, KV, Raft, cross-shard txns,
sessions, directory spread, delete-2PC, FUSE A–D. The Raft+KV engine is
the only metadata engine (Step 11, Sep 11). Snapshots are by log bytes
(512 MiB, W22); L1 compaction is a background thread (W13); InstallSnapshot
is chunked (W11). There is no next §10 step. What remains is the work
queue in [§1a](#1a-the-work-queue): measured gaps, in order.

**How to pick work.** Take the lowest-numbered open item; correctness
before performance. Each item names what to change, how to measure it,
what proves it, and what is forbidden. If an item needs a decision the spec
does not contain, **stop and ask** (§4). Decisions already taken (D1–D13,
D18–D20, D24) are listed under "Decisions — taken and pending" in §1a and
are to be implemented, not re-asked. Two plans are not queue items until
asked: `efsd --bench` and `efs-fuse --bench` (§1a, "plan only"). The
encryption idea (§1a) is not a decision. **Asked Oct 1:** W26, the FUSE
`fallocate` handler.

**Before touching the cluster** run `tests/preflight.sh` (the deploy
rule's pre-flight as one command). Stop/start is
`tests/cluster.sh stop|start|restart [--clients] [--perf] [--strace]`;
a fresh table after a wipe is `cluster.sh start --fresh`. Runbooks for
the open measurement items are in [runbooks.md](runbooks.md)
(`tests/measure/*.sh`).

**The one live cluster is port 19810** on fcstor003–006 (`/data1/01–06/efs`,
`--quota 36T --direct-io`, RDMA), clients fcstor003–015 at `/tmp/efs-mount`,
the user's own client on fstor007 via `scripts/client.sh`. 19820 is retired.
Do not `wipe_cluster.sh`, `pkill -x efsd`, or `raft-mkfs` without being
asked. The live state (build, leaders, what is deployed where) is the
cluster fact in `.cursor/rules/efs-project-state.mdc`, not this page.

### 1b. In flight — finish this before taking a queue item

Whoever picks the project up next does **this first**. Update or delete
this block when done — an "in flight" block older than the last commit
is a bug in this page.

**Oct 1 18:00Z: committed `dc6b0af1` and rolled — `cluster.sh restart --clients --perf` (`~/efs-runs/restart705.log`, CLUSTER_OK): servers `dc6b0af19832-dirty` on fcstor003–006 with `--perf` (`/tmp/efs-perf/efsd.data`), clients fcstor003–015 remounted RDMA, fstor007 untouched. Post-check (`post706`): PREFLIGHT_OK, g0 term 17 leader fcstor003, g2 term 12 leader fcstor006, commit==applied, both idle; fcstor007 2 GiB write 1.4 GB/s, cold read 3.2 GB/s, tail cmp OK; server log stamped and `store: put O_DIRECT zero-copy` active; client `read reply zero-copy active`. The user does the heavy testing on this build.** Earlier that afternoon (15:55Z): W39 (RDMA zero-copy send), read path R1–R5, 64 get workers, the server's PUT bounce copy and the W37 mkfs guard were put in the tree, clients deployed first, servers later. The user asked for the performance items from the roadmap, implemented and smoke-tested, heavy testing his.
- **Client, deployed to fcstor003–015 (`dep680`) and fstor007's `/tmp/efs` (the user's running efs-fuse is untouched):** (1) **W39** — `efs_rdma_zc_region_add` registers the bufpool slabs lazily per HCA (`zc_lkey`, `ibv_reg_mr` once per slab per device, append-only region table), `efs_rdma_send_frame` posts a two-SGE send (pool header, slab payload ≥ `EFS_RDMA_ZC_MIN` 4 KiB) when the QP reports `max_send_sge ≥ 2`; the payload stays referenced until the send completes, which every caller already guarantees (a conn ends replied or `efs_conn_destroy`ed; `efs_rdma_send_quiesce` reaps for the destroy path). Log once `efs: RDMA zero-copy send active`. (2) **Read path R1–R5:** a prefetched chunk's buffer is handed to the rdcache instead of copied (`efs_rdcache_put_owned`); a whole-chunk demand read decodes into the caller's buffer (`chunk_get_job.ext`); the two data fragments are received directly into the chunk and `efs_decode_chunk` skips the self-copy; a chunk-aligned READ whose chunks are all in the rdcache is answered with `fuse_reply_iov` over pinned images (`efs_client_read_refs`, `efs_rdcache_pin/unpin`, `rdcache_ent.pins` excludes a pinned way from victim selection and from `rdcache_put`'s overwrite; `fuse_reply_data` with a multi-buffer bufvec copies in libfuse 3.10, `fuse_reply_iov` does not). Log once `efs: read reply zero-copy active`. (3) The get pool is 64 workers (`GET_POOL_N`), no longer tied to `EFS_WRITE_PIPELINE` — one synchronous fetch per worker is the client's in-flight read cap. (4) **`rmdir` no longer fails ENOTEMPTY from the client's local table** (`efs_client_unlink`): the local child list can hold a child whose removal committed elsewhere, and the refusal was the recurring one-host `dir_deep_nesting` 199/201 (fcstor012 Sep 30 `d50`, fcstor009 Oct 1 15:55Z `d25`, `results/posix/20261001-155455`; the server's row was empty both times). The client logs `efs: rmdir ino=… local table lists child '…' — asking the server` (first 16) and the server (`EFS_INODE_RPC_NOT_EMPTY`) decides. After it (`dep691`, all 13 clients + fstor007's tree): 9-host **200/201 on all nine** (`results/posix/20261001-160049`), jobs=1 200/201 (`-155932`).
- **Measured on fcstor007 (16 GiB random file, cold remount before every read, `agent-rd-20261001-145047` → `-154007-refs`):** write 1.4 → **1.5 GB/s** (client CPU 21.4 s), single cold read 2.5 → **3.6 GB/s** (client CPU 21.6 → **10.8 s**), four readers 3.7 → **6.5 GB/s** (14.9 s CPU); `EFS_READ_PREFETCH=32` is slower than the default 16 (2.9 GB/s) — leave it. cmp at 0/5/15 GiB, whole-file md5, unaligned and tail windows cold and warm all OK (`gate671`; gate670's `CMP_TAIL_BAD` was `dd skip=` on a pipe without `iflag=fullblock` — the harness, not the client). posix jobs=1 fcstor007 **200/201** (`results/posix/20261001-154333`); the D24 wedge gate again: 4 × 8 GiB dd+fsync 8.2 s, storm p50 5.6 / p99 70 / max 112 ms, 32 `report-landed`. The remaining user-space copies in the read profile (`agent-rd-20261001-153356-prof`) are RDMA recv buffer → chunk (zero-copy *receive* needs per-request posted receives — a transport change, **ask**) and the kernel's own copy out of the iov. Single-stream is in-flight bound (~1 ms per chunk, 8 demand + 16 prefetch), not CPU.
- **Server, rolled at 18:00Z (`dc6b0af1`):** `store.c` writes an aligned PUT with a digest as one `writev` (payload + the 4 KiB D20 tail page) — the D20 bounce memcpy per fragment is gone (`store: put O_DIRECT zero-copy` once-log); **W37** — `server_raft_host_mkfs` replicates the salt the committed MKFS recorded (read back with `efs_meta_apply_export_salt`), never its own; a node that holds no salt record answers BUSY; SALT verdicts ride the ring (a mismatch is PROTO, not an apply halt). The mkfs apply itself is unchanged (no-op on an existing root). Compiled with `test_sim` and `test_meta_apply` passing (`bld701`, 17:56Z). Plus the 14:00Z log timestamps. A fresh-table start after the next wipe is W37's gate.
- **Asks, unchanged:** W41 (`report_mu`), D23 (clean-image cache), W40 (FUSE write copy), the fragment-layout change (needs a wipe), RDMA zero-copy receive, D17, W36, W38.

Older handoff blocks (Sep 28 – Oct 1 14:00Z) were moved verbatim to
[project-history.md](../project-history.md) under "START-HERE handoff
archive" on Oct 1 2026. A `§1b block` reference inside the queue below
points there.

### 1a. The work queue

Rules for this queue: **take the lowest-numbered open item.** Do not start a
later item to avoid a harder earlier one — the order encodes a dependency and
a severity: **correctness items come before performance items**, because a
parallel filesystem that returns wrong bytes has no throughput number worth
reporting. Do not batch two items into one change. Every item ends with
`Forbidden`, which is binding.

**What the hardware allows.** Every performance item is measured against
this, not against last week's number. Cluster traffic rides `ibs1f0`
(200 Gb/s IPoIB, `ip route get 172.16.223.57` on a client); servers have six
NVMe each.

| ceiling | value | derivation |
| --- | --- | --- |
| one client, logical write | **~16.7 GB/s** | 25 GB/s line rate ÷ 1.5 (2+1 EC sends 3 fragments per 2 data) |
| one client, logical read | ~25 GB/s | line rate; a read fetches k=2 fragments |
| cluster, logical write | **~44–57 GB/s** | 4 hosts × 16.7–21.4 GB/s NVMe (`results/nvme/`) ÷ 1.5 |
| 1-client honest write today (Oct 1) | ~1.3–1.5 GB/s | **~8–9 %** of the client's ceiling (8 GiB dd+fsync 1.3 GB/s; 16 GiB 1.5 GB/s) |
| 9-client honest write today (Sep 28) | 2551–2810 MiB/s aggregate | **~6 %** of 44 GB/s (8 GiB dd+fsync, 9 own files) |
| 1-client cold read today (Oct 1) | 3.6 GB/s; 4 readers 6.5 GB/s | ~14 % of the client's ceiling (16 GiB, remount before read) |

[architecture.md §1](../architecture.md) says: if a benchmark stops at a
mutex, one leader, one thread, FUSE serialization, one WAL or one
coordinator before a physical resource, *that is by definition an EFS bug*.
By that rule the write path is still a bug, not a tuning task.

Baselines, all honest (flush in the clock, reads after remount, `findmnt`
verified `fuse.efs-fuse`). The full history of these numbers is in
`.cursor/rules/efs-fio-honest.mdc`.

| measurement | value | where |
| --- | --- | --- |
| 1-client 8 GiB `dd bs=1M conv=fsync`, fcstor007 | **1.3 GB/s** (6.80 s) | §1b 13:20Z block (`dd539`), Oct 1 |
| 1-client 16 GiB write / cold read / 4 readers | **1.5 / 3.6 / 6.5 GB/s** | §1b 18:00Z block (`agent-rd-20261001-*`), Oct 1 |
| 9-client 8 GiB dd+fsync, aggregate | **2551** MiB/s (best 2810) | `results/measure/20260928-134637-dd-prof-r5b`, `-131651-dd-prof-r2b` |
| per-host local NVMe ceiling | 16.7–21.4 GB/s | `results/nvme/` |
| IO-500 9×4 debug, fresh table | easy-write **5.173 GiB/s**, hard-write 0.519, mdtest-easy-write 6.185 kIOPS, hard-read 1 error (W38) | `results/io500/20261001-074905-rdma/` |
| IO-500 9×4 debug, 0 errors | easy-write 4.563, hard-write 0.640, hard-read 1.034, cold hardscan bad=0 | `results/io500/20260930-183504-rdma/` |

**Never quote intra-job fio write samples or `dd` progress lines** — those are
pre-flush and read several GiB/s. The number is bytes ÷ wall with the flush
inside. `dd if=/dev/zero` is also invalid here: all-zero payloads skip PUTs.
Use a non-zero source file.

#### Single-node storage bench — plan only (Oct 1). Do not implement until asked.

**Why this goes before the client tests (Oct 1 04:50Z).** The cluster write wall is ~2.5–2.8 GB/s logical with nine clients (`results/measure/20260928-134637-dd-prof-r5b`): ×1.5 for 2+1 EC ≈ 4.2 GB/s of 64 KiB fragments ≈ **16 K fragment writes/s per server**, each a file create plus an `O_DIRECT` write on XFS. That is an ordinary filesystem-metadata rate, not the 16.7–21.4 GB/s the `results/nvme/` fio shape measures, and only a bench that calls the fragment store the way the PUT handler does can say whether it is the number. Same for metadata: 4.6 kIOPS mdtest-easy-write across two groups against a 0.4 ms Raft-log `fsync` is an inference that the disk is not the limit; `--bench-meta` makes it a measurement. The bench needs no cluster, no clients, no quorum, no network, no wipe, and none of the things that have confused every cluster measurement (strace on the daemon, elections, a dead mount). **What it will not find:** the ~1 GB/s single-client wall — every client profile shows the server near idle; that one needs the client-side ladder (04:30Z §1b block: one dd, Little's law on one reclaim thread, then a `/dev/shm` private cluster), which comes after this.

**What.** One server, its six NVMe, the code `efsd` already uses, driven until the disk is the limit. Two workloads: data-fragment read and write, and metadata read and write (KV segments, the KV WAL, the Raft log — metadata is not an EC fragment). The same run records the hot path, so the next edit is the symbol that is on CPU while the disk is not full.

**What exists.** `efsd --bench <path>` (`src/server/bench_local.c`) `pwrite`s zeroed `EFS_FRAGMENT_SIZE` buffers into one `.efs-bench` file, rotating 1024 offsets, then deletes the dir. No `fsync`, no read, one path, and it never calls the fragment store, the writer pool, the KV, or the Raft log. That number is not a storage-engine result. **Replace it; do not keep it as a second line next to the real one.** `results/nvme/` is the per-host ceiling (16.7–21.4 GB/s) and is the bar.

**What to add.**

1. **Ceiling, same host, first.** The `results/nvme/` fio numbers for that host are the ceiling; re-run only if the host is not in that dir. Four numbers: sequential write with the flush in the clock, sequential read, and the metadata shape (small synchronous writes, random reads of a segment block). Print them in the bench output. The engine run is judged against them.
2. **Data, through the engine.** `efsd --bench <kind> --storage <p1>[,<p2>,…]` — the same comma list `efsd` takes, so the bench runs on **1, 2, 3, … 6 paths from one invocation set** and the output shows how the engine scales from one drive to many (one drive at the fio ceiling and six at 1.3× is a serialization point in the writer pool or the store, not the disks). Kind `data`: call the fragment store and the writer pool exactly as the PUT handler does — fragment name, the probe/`path_hint` logic as shipped, create, `O_DIRECT` write of a **non-zero** payload, whatever `fsync` policy PUT applies, and the publish/rename step if the store has one — then read those fragments back the GET handler's way (`O_DIRECT`, so it is not the page cache). No cleaner path than production: the gap to fio is only meaningful if it is the engine's own.
3. **Report latency at fixed queue depth, not only GiB/s.** For each path count and for QD 1, 16, 64, 256 per path: fragment ops/s, p50/p99 latency per op, and aggregate GiB/s. A client has 16–32 chunks in flight; what it sees is the latency at that depth, and `in-flight × 64 KiB ÷ latency` on the server side either matches the 2.8 GB/s cluster number or does not. A GiB/s-only number at unlimited depth hides the problem the system actually has.
4. **Metadata, through the engine.** Kind `meta`, on `--meta-storage`. Drive `efs_kv_lsm_*` and `efs_raft_disk_*` with sync on (the simulator's `EFS_KV_LSM_NOSYNC` / `EFS_RAFT_DISK_NOSYNC` is the wrong tool): puts that cross the memtable and compact, Raft appends that `fsync` at QD 1 and batched, then point gets and a log reopen. Report MB/s, ops/s and p50/p99 next to the small-IO fio lines; include the compactor segment `fsync` time (the 100 ms mode of D11). Flush is in the clock; bytes must show up in `du`.
5. **Hot path, same process.** `--perf` on that run (attach or the existing efsd flag; do not start `efsd` under strace). One line in the bench output per run: the top symbols and whether the wall was disk (`iostat` util per device) or CPU. A change that moves neither the GiB/s nor that line is not a result.
6. **Where.** One fcstor, no quorum, no clients, no port 19810, no wipe. Bench dirs under the `/data1/0N/efs`-style paths of a node that is not serving 19810 are off limits while it serves; use an unused directory on the same devices or a node the user names. Screen on that host (`EFS_RUNNER=fcstor0NN.ib`). Build in `/tmp/efs` on the node.

**Done when.** One log has the four fio ceilings, then for 1 and 6 paths (and the steps between) the engine's data write and read ops/s, p50/p99 at QD 1/16/64/256, aggregate GiB/s, and the meta read/write lines, each beside its fio ceiling, with the top symbols and `iostat` util. The 1-path → 6-path curve is part of the result. Anything short of the ceiling names the symbol to change next. Do not tune from the current `--bench` line, and do not add an IO path the server does not already have.

#### Client bench `efs-fuse --bench` — plan only (Oct 1). Do not implement until asked; after `efsd --bench`.

**Why.** The single-client write wall (~1 GB/s whatever the transport, host, stream count or build; §1b 04:30Z) has never been located: no measurement separates the kernel FUSE path, the client pipeline, the per-fragment round trip and the client's own arithmetic. `ll_write` is a shim around `efs_fuse_write(NULL, buf, size, off, fi)`, and open/flush/fsync/release are the same kind of shim, so the whole client write path runs in-process with no kernel and no `/dev/fuse`. Four levels, one variable apart, replace the dd ladder:

| kind | what runs | what it isolates |
| --- | --- | --- |
| `cpu` | no network, no server: synthetic non-zero 128 KiB chunks → dcache store → blake3 + XOR parity + the RDMA bounce copy into a registered buffer, then drop | the client's arithmetic ceiling per thread and its scaling with threads (`memmove` + `blake3` + `xor_into` are ~11 % of today's profile; this says what they cost at full speed) |
| `put` | `efs_client_put_fragments_parallel` straight to the servers at a fixed queue depth; no FUSE, no dcache, no REPORT | the per-fragment round trip the client sees: fragments/s, p50/p99 at QD 1/16/64/256, GiB/s — the client side of Little's law. Against a `/dev/shm` private cluster it is network + handler + poller only; against NVMe it adds the store, which `efsd --bench` measured alone, so the difference is attributable |
| `write` | `efs_fuse_create` → `efs_fuse_write` 1 MiB at a time → `efs_fuse_fsync` → release, in-process, N files in parallel — the `ll_*` handlers minus the kernel | the full client pipeline (dcache, reclaim/put pool, dirty-byte throttle, REPORT) with the flush in the clock; the gap to `put` is the pipeline's own cost |
| dd through the mount | the number we have (fcstor007 1000 MiB/s, 32 GiB + fsync) | the gap to `write` is the kernel FUSE path: request dispatch, the `fuse_buf_copy`s, `max_write`, libfuse worker count — W15's splice/`max_write` questions become measurable |

Reading: `cpu` ≫ `put` ⇒ latency-bound (more in flight or a shorter RTT; not more poller threads); `put` ≈ `write` ≈ dd ⇒ the RTT is everything; `write` ≫ dd ⇒ the kernel FUSE path is the wall; `put` on `/dev/shm` ≈ `put` on NVMe ⇒ the store is not in the client's RTT.

**Rules, or it is another `--bench` that lies.**

1. **Production path only.** `efs_fuse_write` / `efs_fuse_fsync` and `efs_client_put_fragments_parallel` as they are: no bench-only shortcut, no skipped hash, no skipped REPORT in `write`. A hook the handlers do not have means the bench is measuring something else.
2. **Honest clock.** `write` counts bytes after fsync/REPORT returns; `put` counts a fragment on its reply. Non-zero payload (all-zero chunks skip the PUT).
3. **Fixed queue depth, latency reported** — ops/s, p50/p99, GiB/s per QD (1/16/64/256) and per thread count, like the server bench. The dd ceiling is `in-flight × 128 KiB ÷ latency`; print both factors.
4. **Never against 19810's export.** `put` makes fragments no row references; `write` makes real files. Target the private 3-node cluster on fcstor007 (`tests/rdma_first_inode.sh` layout, ports 19950–19952, storage on `/dev/shm` or a scratch dir) or a bench export the user names. No wipe, no roll.
5. **Untraced.** `perf record -g` on the bench process for the top symbols; never strace.
6. **One host, then two.** The same `put` / `write` from two hosts against the same servers separates the per-client wall from the cluster wall (4 clients = 578 MiB/s each, 9 = ~290 each today).

**Where in code.** A `--bench` entry in `efs_fuse.c`'s `main` before `fuse_session_new`; a driver loop per kind; a latency histogram; a non-zero chunk generator. The heavy lifting is existing functions.

**Done when.** One log per host count has the four levels side by side (dd from the honest-fio rule's run dir), each with ops/s, p50/p99 at the four depths, GiB/s, thread count, and the top symbols; the two largest gaps between adjacent levels name the next item. Forbidden: a cleaner code path than the handlers use, a GiB/s number without its queue depth and latency, zero payload, running against port 19810's export, tracing the bench process with strace.

#### Decisions — taken and pending (Sep 28 2026)

Each row is a choice the spec did not make. A row marked **decided**
was accepted by the user and is now part of the design; implement it
per the item it points at. A row marked **done** is history. Nothing
in an open row is implemented until the user asks. W13 is done; its
pinned segment view is the primitive W11's steps use.

**Decided Sep 28 2026 (user accepted the recommendations):**

| item | question | decision (Sep 28) | why, in one line |
| --- | --- | --- | --- |
| **D1 · W6.1 / W17** | what do N-1 shared-file writes do under conflict? | **A span publish commutes; no lock; no CAS on the base generation for spans.** A sub-range writer always publishes a span (even when it holds the base). `expected_gen` applies to full-image publishes only. The trailer keeps the folded spans' `candidate_gen`s (≤ `EFS_CHUNK_DELTA_MAX`) so a replay of a folded span is a no-op. The fold is done by the publisher that fills the last slot, or by a reader — never on another rank's `fsync`. | the code already has commuting non-overlapping spans; what collapses at nine ranks is the base-gen check *before* the span path (one fold STALEs every in-flight span) and the `have_base → full CAS` branch. This is §7.2's commutative reduction applied to the chunk map, and P1 (no serialization point). A distributed chunk lock stays forbidden. Gate in W17 |
| **D2 · W6.2** | may `open()` return before the chunk map is known; how big is the map window? | **Yes. `open()` adopts the inode row only.** Chunk maps are pulled per lane group, in parallel, by `pull_layout_miss` in a metadata window that runs one data window ahead of the prefetcher. Window size is internal and adaptive (start at the prefetch depth, grow while the read stays sequential). No mount option, no environment variable. | `performance.md` already says reads fetch chunk maps in per-lane windows; the code pulls the whole map sequentially at open and the server serializes 36 openers to 14.6 ms per GETCHUNKS. A row without a map is already the evictor's steady state (W9); a miss is pulled and an error is an error, never a zero-fill (I9) |
| **D3 · idle gate** | may the 5/s REAP_DONE gate be raised to remeasure ior-hard 1/9/36? | **No.** Measure after the reaper drains. If it does not drain, that is a bug to file. | the tail is the reaper clearing deleted IOR data, and a measurement started during it measures the reaper |

**Decided Sep 29 2026 (user accepted D4–D8 as written; source
`results/io500/20260929-023447-iorperf2/ana`). Implement per the item
each row points at; do not re-ask. D8 is a measurement whose result
comes back to the user before any budget changes.**

| item | question | decision (Sep 29) | why, in one line |
| --- | --- | --- | --- |
| **D4 · W22 step 1** | when does a group take a snapshot and truncate its log? | **By log bytes and follower reach, not every 256 entries.** Take a snapshot only when the log since the last one exceeds a byte budget (recommend 512 MiB — the export itself is 2.67 GB and takes 5–12 s, so anything smaller makes export the steady state), and keep a trailing window of log entries after the snapshot point so `send_ae` serves a follower that is behind by less than that window from the log. Send InstallSnapshot only when `next_index` is below the retained window | `HOST_SNAP_MIN` = 256 applied entries triggers a 2.67 GB export back to back on every node; `raft.c:672` sends the file to any peer more than 256 entries behind; three imports of 12–16 s happened in one 4-minute IOR and each one is a follower that answers nothing while its pump applies the diff |
| **D5 · W22 step 2** | may a follower answer heartbeats while its pump applies an InstallSnapshot diff? | **Yes — slice the diff.** Apply the import diff in bounded slices between pump cycles (one `HOST_TICK_US` worth of keys, then drain the inbox and answer the heartbeat), instead of one 12 s apply. The follower stays a follower; the leader keeps its term | this is W14 step 2's "heartbeat answered from a thread that is not importing", with the evidence: terms moved +45 / +105 across the run with `drop=0` on every outbox, so the outbox was not the trigger; the import-diff apply is the only multi-second pump hold left (`apply_max` 1.17 s on fcstor006, `pump_hold_max` 4–5 s on fcstor005 in the earlier run) |
| **D6 · W22 step 3** | may `mdraft/` (Raft log, KV WAL, segments) live on a root the fragment writers do not use? | **Yes, and measure first.** One `dd` on a quiet node: `fsync` of a 4 KiB file on `/data1/01` while the writer pool creates fragments there, versus on a root with no writers. If the shared root shows the 100 ms mode, give `efsd` a `--meta-storage <root>` (default: first root, today's layout) and point 19810 at a root the six `--storage` paths do not include (or a seventh partition) | the compactor's segment `fsync` was a flat 100 ms 97 times and `persist_max` reached 254 ms while twenty writer threads did `openat`/`write`/`close` on the same XFS; the Raft commit path and the data path share one journal |
| **D7 · W14 step 4** | may the client tell the server a PUT is the first write of `(ino, ci, fi, gen)` so the server skips the six-root probe? | **Yes, with a sentinel, not a guess.** `path_hint = 0xffffffff` means "the client has never PUT this fragment generation"; the server then creates on the writer's least-queue root with no `access()`. Any retry of the same generation (REPORT STALE replay, failed reply) sends the real hint or 0. A fragment name includes its generation, so a first write of a new generation cannot collide with an existing file | the hint from a previous PUT can only help a re-PUT; IOR-easy and a fresh `dd` write every chunk once, so 1.59M `access()` (192 s across six handler threads on fcstor004) survived W14.4 unchanged. The step-4 gate ("under 1 % on a single-client dd") cannot be met by the hint as specified |
| **D8 · W16 steps 2–3** | how does a 86 234-record REPORT reach the log? | **Measure the per-batch commit latency first, then bring the number.** `report-split` shows `push_ms=9869` for 337 publish batches — ~29 ms per batch through the one in-flight batch per peer — and `finish_ms=9227` waiting on the last apply. Add `pub_batch_ms` (p50/max) to `raft-obs`; if the per-batch commit is the ~2–6 ms a Raft round costs here, the 29 ms is queueing behind other clients' batches and the answer is fewer, larger entries per REPORT; if it is the 100 ms `fsync` mode, D6 is the fix. Pipelining past one in-flight batch stays forbidden | the client's fsync waited 19.4 s in one TCP `recvfrom` for a server that was still pushing; nine clients × ~80K records at the observed rate is minutes of leader time per stonewall |

**Pending Sep 29 2026 (from the 04:07–04:27Z trace, `results/measure/20260929-040800-idle-trace/ana`; §1b has the numbers). Recommendations, not decisions: bring them to the user. D9 and D10 are the two halves of one problem — the KV cannot absorb writes as fast as the Raft log commits them, and the wait lands on the pump.**

| item | question | recommended (D9–D11, Sep 29) | why, in one line |
| --- | --- | --- | --- |
| **D9 · W13 step 2, W23** | may the apply path (the pump) block on L0 back-pressure? | **No. The pump never waits for the compactor.** When L0 is within `KV_LSM_RANGE_MAX` of the cap, the apply keeps writing the memtable and lets it grow past `memtable_max` (memory, bounded by what the Raft window can commit — 512 MiB of log is the ceiling since W22.1); back-pressure moves to admission on the leader: `host_propose` for REPORT/publish batches returns BUSY while the local L0 is over the cap, so the client retries with its existing budget and the follower's pump is never the one that stalls. Heartbeats and AppendEntries replies do not depend on the KV | the pump waited 4.1 / 1.7 / **24.2** / 2.7 / 3.6 s in `kv_maybe_flush_locked` on fcstor004 in one 7-minute write, each wait one compaction long; every wait produced `apply-sleep` 400 ms timeouts → REPORT `rc=-13` → client fsync EIO, and the two term changes of the run. Same class as W13 (a lock hold the apply does not need) and W22.2 (a follower must answer while it imports) |
| **D10 · W23** | how does L0 reach L1 so that a 7-minute write does not rewrite the table thirty times? | **Compact by bytes, not by file count, and split range 0.** (a) Merge a range's L0 into its L1 only when that range's pending L0 bytes are at least a fraction of its L1 bytes (recommend 1/8: a 250 MB range waits for ~30 MB of L0, a 1.8 GB range for ~220 MB), otherwise let L0 files accumulate — the cap that matters is bytes in L0, not 64 files; reads already probe every L0 (`kv_seg_probe`), so make the L0 cap a byte budget (recommend 1 GiB) and drop the 64-file cap. (b) Partition on more than `key[0]` where a range is large: range 0 is 1.8 GB against 190–330 MB for the other fifteen; split it by the next key byte at flush and compaction time so no range exceeds ~256 MB. (c) The merge reads input blocks one `pread` at a time (3.7M of ~8 KB): read each input segment through a 1 MiB sequential buffer | 433 compactions wrote 159 GB to keep a 5.3 GB table current through one 7-minute write; the compactor is 48–55 % of two servers' samples; range 0 alone is 61 GB of the 159 and the 24 s pump hold. `inputs=` 5–11 files of ≤256 KB each per rewrite of 200–1800 MB is write amplification of several hundred. W13 step 5's partitioned flush bounded the rewrite to one range; it did not bound how often a range is rewritten |
| **D11 · W22 step 3 / D6** | is `mdraft/` sharing an XFS with the fragment writers the 100 ms `fsync` mode? | **No — close D6 as "not the sharing", skip the `dd` measurement, do not move `mdraft/`.** The pump's Raft-log `fsync` averaged 0.38–0.40 ms on all three servers measured (61 228 of 61 668 under 2 ms on 004) while twenty writers created 408K fragments; the 90–120 ms `fsync`s are the compactor's own 200–300 MB segments (299 of 695 on 004, 407 of 733 on 005). `--meta-storage` stays as an option; the compaction fix (D10) is what removes the 100 ms mode and the 130 MB/s the compactor puts on `/data1/01` | per-thread `fsync` histograms in `idle-detail2-fcstor00{3,4,5}.txt`; a segment written at ~2–3 GB/s and then `fsync`'d is ~100 ms by arithmetic, no journal needed |
| **D8 · answered** | is the per-batch commit ~3 ms or ~100 ms? | **~3 ms** (`pub_p50=3109us` in 17 of 23 samples with traffic); the 46 ms samples and `pub_max` 0.8–2.4 s are the pump holds above. No "fewer, larger entries per REPORT" wire change is indicated. Remeasure `pub_p50`/`pub_max` after D9/D10 in one 9-client IOR | the number the D8 row asked for, from the `raft-obs` line W16.2 added |

D9, D10, and D11 were rolled 05:31Z (`a53b253f2455-dirty`). The 12:02Z trace (§1b) is the measurement: D9 held (no pump wait, no `backpressure` line), D11 held (`fsync` max 91 ms), and D10's byte rule held (0.25 GB compacted, not 159 GB). D10's "drop the file cap" did not. That correction is D12.

**D12 was implemented and rolled 12:29Z** (`a53b253f2455-dirty`, no perf, no strace). On the 12:36Z IOR, fcstor004's compactor brought L0 from 7 files down to 3 (L1 stayed 397). The recommendation below is what was built. The 400 ms apply wait still returned BUSY (73 of 99 `report-split` lines on fcstor004).

| item | question | recommended (D12, Sep 29) | why, in one line |
| --- | --- | --- | --- |
| **D12 · W23 step 4** | may L0's file count grow without a bound while L0 bytes stay under 1 GiB? | **No. Keep the 1/8 byte rule, and put the file cap back as a backstop.** When `n_l0` is over `KV_LSM_L0_DEFAULT` (4), compact the range with the most L0 files even if its bytes are under 1/8 of its L1, and repeat until `n_l0` is under the cap. The 1/8 rule still applies when the count is already under the cap, so a fat range is not rewritten for a few KB. The pump still does not wait. D9's 1 GiB admission stays; it did not fire here | fcstor004 went 26 → 310 L0 files (L1 353) in one IOR while 34 compactions wrote 0.254 GB at 10–14 ms each; `lookup` then probes every file, the publish apply falls behind, and `host_wait_applied` returns BUSY at 400 ms (`pack_ms=0 rc=-13`, 54 of 60 reports). D10 said to drop the file cap because "reads already probe every L0" — that probe is the abort. **The backstop as built rewrote L1 for the whole of the 13:17Z copy (D13)** |

**D13 · W23 step 5 (from the 13:08Z trace, `results/measure/20260929-130800-ddposix/ana`). In tree 14:20Z, not rolled, not gated.** The shape below is what was built. A file-cap compact writes one L0 file and does not open L1. A compact that already meets 1/8, or the 1 GiB byte cap, still rewrites L1.

| item | question | recommended (D13, Sep 29) | why, in one line |
| --- | --- | --- | --- |
| **D13 · W23 step 5** | when the file cap forces a compact, may that compact read the range's L1? | **No. Merge that range's L0 files with each other into one L0 file, and do not read its L1.** Repeat until `n_l0` is under the cap. The 1/8 rule stays the only merge that rewrites L1. The pump still does not wait. D9's 1 GiB admission stays | D12 ran a full L0+L1 merge whenever the count was over 4, which was the entire 100 GiB copy: fcstor004 wrote 67.5 GB in 425 compacts (max 17 s, L0 peak 379) and `lookup` still binary-searched them (`kv_seg_probe` 8.3 % self). `pack_ms=0` on 172 of 184 BUSY reports; one `push_ms` was 206 s |

**Pending Sep 30 2026 04:20Z (from the post-fix ecopy, `results/measure/20260930-040600-postfix-review/SUMMARY.txt`). The two mechanical fixes (F1, F2 there) are in tree and rolled 05:06Z; these two still need the user. Ask; do not write either.**

| item | question | recommended (D15–D16, Sep 30) | why, in one line |
| --- | --- | --- | --- |
| **D15 · W14 step 2 / raft.c `on_vote_req`** | may a voter that heard from the current leader within the last election timeout refuse a higher-term VOTE_REQ and keep its leader? | **Yes — leader stickiness (Raft §4.2.3 / §9.6; Pre-Vote or CheckQuorum are the two standard shapes).** Recommend Pre-Vote: a candidate first asks "would you vote for me at term+1?" without bumping anyone's term; a voter answers no while its leader heartbeat is younger than the election timeout; only a majority of yes starts the real election. `maybe_step_down` then never fires from a peer whose only problem is that its own lane to the leader is stalled | 426 group-2 terms in 13 minutes: fcstor006, unable to get a reply from fcstor004/005 for 30 s at a time, campaigned every 0.5–0.9 s and each VOTE_REQ deposed the live leader (`LEADER->FOLLOWER leader=-1`, re-elected 0.6 s later); every cycle is 400 ms of BUSY reads for every client. F1 shortens each stall from 30 s to 250 ms; it does not stop the deposition |
| **D16 · peer transport** | should Raft peer frames share the RDMA device's one shared recv CQ / poller and the per-conn 36-credit scheme with ~400 client conns, or get their own class with reserved credits and their own poller? | **Measure first (stack-sample the sender and peer-conn threads in a storm), then decide.** If the lost RAFT_REPLY is the shared `recv_poller` behind client completions, peer conns need their own CQ/poller; if it is the peer's reply send waiting on a credit, reserved credits. Do not pick before the sample | the frames of a blocked batch reach the peer and are processed (fcstor006 won votes through a frozen lane); only the reply is late by up to 30 s, and only while ecopy drives hundreds of client conns per server. **Sep 30 06:17Z: the 30 s late reply was the sender reading the wrong channel (`85f5b31c`), not the transport; D15/D16 keep no motivating case** |

**Pending Sep 30 2026 07:10Z (from the user's perf dir, `results/measure/20260930-063500-perf-dir-review/SUMMARY.txt`). Ask; do not write either.**

| item | question | recommended (D17–D18, Sep 30) | why, in one line |
| --- | --- | --- | --- |
| **D17 · `st_blocks`** | where does a client get the allocated-block count for a file it did not write? `inode_allocated_bytes` counts the client-local present-chunk table (W20), empty since D2 for unopened files, so `du` sums 0 for every regular file (52K for 3.8 TB) and ecrawl calls 21150 of 21503 files sparse. `efs_meta_row` has no count; W20 forbids `st_blocks` from size alone | **A per-lane present-chunk count in the lane stamp** (each publish adds the chunks it made present, a truncate subtracts), reduced at getattr like `max_end`/`max_mtime` and returned in the row image; the client sums it. One more u64 per lane read, no new RPC. Alternative: define `st_blocks = 0` as "unknown" for files this client did not write and tell tools so | sparse detection, `du`, quota tooling and ecrawl all read `st_blocks`; only the server sees every lane's publishes |
| **D18 · client staging-table floor — DECIDED Oct 1 2026 (user): evict whole cold tabs; implemented `f073e136`, see §1b** | the staging estimate is dominated by the per-shard-tab floor (one 256-row slab + 16 × 1232 B chunk entries + 512-slot indexes ≈ 90–170 KB per tab, ×4096 tabs ≈ 360–700 MB) and exceeds `EFS_CLIENT_META_MB` (256 MB) with a few thousand rows staged (412 MB at 10780 rows, RSS 330 MB). The evictor then drops 64 hot rows a second forever and never reaches the cap. Which bound do you want? | **Evict whole cold tabs** (`shard_tick` already tracks tab age; a tab with no dirty/pinned/open ino is freed and rebuilt on demand), so eviction frees the floor it cannot otherwise reach; keep the row LRU for the rest. Alternatives: a smaller first slab and lazily sized indexes (the floor shrinks ~10×), or apply the cap above the floor (RSS then ≈ floor + cap) | the 22 ms/s stall from this churn is fixed mechanically (targeted `efs_export_evict_ino`), but the cache still cannot hold a du's rows, and the RSS bound the cap promises is not the one delivered. Sep 30 21:10Z: with the floor above the cap the evictor's LRU scan is **31.6 % of a 62 min client profile and 47.6 % (≈ 0.6 core) during an ecopy**, re-armed by every staging op (`results/measure/20260930-205300-ecopy-perf-review`) |

**Order for continued implementation (decided Sep 28 2026).** Correctness
and truthfulness first because they are small and they are what "easiest
to use" means; then the CPU items that need no decision and are most of
both profiles; then the two design items above; W12 whenever a run dir is
cited.

| # | item | what (order Sep 28) | why now |
| --- | --- | --- | --- |
| 1 | **W17 step 1** | `fsync`/`release` return in bounded time; a killed writer never leaves a D-state FUSE request | seven clients wedged for 20 min after `kill -9` |
| 2 | **W16 step 1** | RPC BUSY/NET/IO surface as EBUSY/EIO, never ENOENT | a busy filesystem must not claim a file is missing |
| 3 | **W18** | dcache reclaim from a dirty list; init once | 73 % of the client's CPU walks a table looking for dirty chunks |
| 4 | **W19** | encode each AppendEntries once; `try_commit` from the replying peer's match | 35 % of the leader is copying AEs on the pump |
| 5 | **W14 step 4** | fragment `path_index` hint on PUT | six path walks per PUT |
| 6 | **W15 step 3** | one `fuse_buf_copy` per whole chunk, for multi-chunk writes too | in tree only for `size == chunk`; `dd bs=1M` (eight chunks) still copies twice: 16 % + ~19 % in the 22:11 profile |
| 7' | **W14 step 2 (a)–(b)** | outbox coalesces replies/heartbeats instead of dropping them | `drop=72114 > sent=50695` to one peer; terms +59 / +26 in two hours; every client BUSY is `no-hint` |
| 7'' | **W15 step 4** | post the RDMA SEND and return; reap CQEs in `send_buf_pick` and the reply wait | 6.9 % of the client waits synchronously for its own send CQE; more send buffers did not change it |
| 6a | **W20** | `st_blocks` from a per-inode counter; `ll_setattr` reads `size` from the row | a `stat` walks every chunk under two locks; 8.9 % of the client under `ll_setattr` |
| 6b | **W21** | a kick does not wake a scan that cannot evict (1 s idle after a no-evict wake, cleared on unpin); cross-tab running total | in-tree cursor did not help: 12.8 % of the client on the 22:48 profile because every write over cap kicks a wake |
| 6c | **W15 step 5** | `FUSE_CAP_SPLICE_READ` so the write payload is read from the pipe straight into the dcache buffer | after step 3 one user copy per byte remains (~22 % `fuse_buf_copy`) plus the kernel copy into libfuse's buffer (~9.5 %) |
| 6d | **W23** | probe a pooled connection's liveness once per idle period, not per checkout; pool generation instead of `fstat` per send | 1.17M `fstat` + 1.17M `getsockopt` + 0.6M `MSG_PEEK` in 230 s (~15K syscalls/s) for 586K checkouts; ~2.5 % of the untraced client |
| 6e | **W24** | `open(O_TRUNC)` / `truncate` of a large file returns in bounded time; SETATTR under BUSY is EBUSY, not a hang | a `dd` onto an existing 1.7 GB file never got past `openat`; SETATTR is the most-exhausted RPC (136 of 424) |
| 2' | **W25** | `futimens` fences cross-group lanes (today EINVAL on every multi-lane file); atime keeps nanoseconds; BUSY → EBUSY everywhere | `ecopy --verify` fails `futimens` on every large file **and then deletes its copy**; the atime check fails on every small one; correctness, goes with row 2 |
| 7 | **W14 steps 2, 3, 5; W15 step 4** | election trigger under import; reap `snap-*.kvx.tmp`; `send_buf_pick` | smaller residuals from the same profiles |
| 8 | **D1 → W17 steps 2–3, W6.1** | commuting span publish | shared-file write rate must rise with ranks; the headline N-1 number |
| 9 | **D2 → W6.2** | open adopts the row; maps in windows | 1 GiB open 1.6–2.5 s under 36 openers |
| 10 | **W16 steps 2–3 (D8)** | `pub_batch_ms` in `raft-obs`, one 9-client IOR, bring the number; no budget change without the user | `push_ms=9869` for 337 batches is either queueing or the 100 ms `fsync` mode, and the two have different fixes |
| 11 | **W8, W10** | 9-host posix 201/201; RDMA faster than TCP | gates re-run once 3–9 land |

**Revised order (Sep 29 2026, after D4–D8).** The items below go
**before 9 and 10**: they are the cause of the two IOR aborts and
nothing downstream can be measured until an ior-easy-write completes.
9 (D2 windows) and 10 (D8) follow; 11 last.

| # | item | what (order Sep 29) | why now |
| --- | --- | --- | --- |
| 8a | **W22 step 1 (D4)** | snapshot by log bytes (512 MiB) with a retained log window; InstallSnapshot only below the window | a 2.67 GB export every 256 entries is the top user symbol on all four servers and turns any 257-entry lag into a 12–16 s import |
| 8b | **W22 step 2 (D5)** | the follower applies an import diff in per-tick slices and keeps answering heartbeats | terms +45 / +105 across one IOR with `drop=0`: the import is the election trigger left |
| 8c | **W14 step 4 (b) (D7)** | `path_hint = 0xffffffff` = first write of this fragment generation; server creates on the least-queue root with no probe | 1.59M `access()` / 192 s on one server per IOR; the re-PUT hint cannot reach a fresh chunk |
| 8d | **W22 step 3 (D6)** | measure `fsync` on the shared vs a quiet root; if the 100 ms mode is the sharing, `--meta-storage <root>` and redeploy 19810 with `mdraft/` off the fragment roots | compactor `fsync` flat 100 ms ×97, `persist_max` 254 ms; a redeploy is a cluster change, so it is last of the four and its measurement decides it. **Superseded by D11 (Sep 29 04:27Z trace): the Raft-log `fsync` is 0.4 ms; the 100 ms is the compactor's own segment. Flag is in tree; no move** |
| 8e | **W23 step 1 (D9)** | the pump never waits for the compactor; back-pressure is BUSY at `host_propose` on the leader while L0 bytes are over 1 GiB | **rolled 05:31Z, held on the 12:02Z trace:** no pump wait, no `backpressure` line. The BUSY in that IOR is not this admission |
| 8f | **W23 step 2 (D10)** | compact a range when its L0 bytes reach 1/8 of its L1, L0 cap in bytes, split range 0, 1 MiB merge reads | **rolled 05:31Z. Byte half held** (0.25 GB compacted, not 159 GB; `pread` over 1 MiB is in use). **File-cap half did not** — see 8i |
| 8g | **rolled 05:31Z** | `disk_log_new_bytes` O(1); `kv_seg_w_open` `setvbuf` 1 MiB | in the same roll as 8e |
| 8h | **W23 step 3** | `lookup` skips a segment whose first/last key span does not contain the key, before `block_for` | **rolled 12:29Z.** Last key is cached in `kv_seg_open`; `kv_seg_probe` returns NOT_FOUND when the key is before `idx[0]` or past the cached last key. Not separately profiled (this IOR had no perf). L1 was still 397 files and `report-split` was still mostly `rc=-13` |
| 8i | **W23 step 4 (D12)** | when `n_l0` is over the file cap, compact the range with the most L0 files even under the 1/8 ratio, and repeat until the count is under the cap | **rolled 12:29Z.** On the short IOR, fcstor004 ended at L0=3. On the 100 GiB copy the same rule rewrote L1 the whole time (67 GB, L0 peak 379). That shape is D13 |
| 8j | **W23 step 5 (D13)** | over the file cap, merge the range's L0 files with each other and do not read its L1; the 1/8 rule stays the only L1 rewrite | **in tree 14:20Z, not rolled, not gated.** The probe walks every in-span L0 file, and the full-L1 backstop both failed to keep the count down during the copy and put 67 GB on the same disk as the apply |

**Added Oct 1 2026 (correctness; both go before every row above —
details and the exact follow-up steps are the 04:30Z block in §1b).**

| # | item | follow-up steps | evidence and limits |
| --- | --- | --- | --- |
| 0a | **W26 · STALE replay that never converges** | (a) identify ino 116202 on 19810 from a KV copy and compare its chunk row with what the classifier (`write.c:880–968`) would replay; (b) repro: eight `dd bs=1M` into one mount, SIGINT mid-write, client stop, `EFS_DCACHE_TRACE=1`; (c) mechanical: the unmount drain names the inos and rc it abandons, and a `report-stale` round that replays the same single chunk > 16 times logs ino/ci/row gen/verdict once; (d) **ask** what a rec that STALEs identically on every replay should do (fail the next close/fsync after N? retry until unmount? spill locally?) | fstor007 Oct 1 00:05: 2768 rounds of `report-stale: chunks=1 … committed=0 replayed=1` and then `UNMOUNT DATA LOSS … rc=-14 after 60s`. Acknowledged writes were discarded; the log cannot say whose. Forbidden: widening the 60 s drain, dropping the STALE check, or publishing a rec the server rejected |
| 0e | **W38 · ior-hard: a client's full image folds its own published span without the span's bytes (4256 B of zeros, committed)** | the F3 block in §1b (Oct 1 08:05Z): one-client 4-rank IOR hard with `EFS_DCACHE_TRACE=1 EFS_REPORT_DBG=1`, `hardscan` cold, `raft-getchunks` on each bad chunk; fix on the client (the fold observation must come from a body that holds the span) | `results/io500/20261001-074905-rdma` (NOTE.txt, hardscan.txt, getchunks-118842-118844.txt): ino 10897 ci 118843 base 1774…2861 + len-0 tombstone 1838…0185 seq 1222; 1 of 747720 records; first IOR with W30 in the client. Data loss: goes before 0c |
| 0c | **W36 · rename-vs-unlink of one source both succeed, dangling dentry** (posix2 `peer_rename_vs_unlink_src`, 1 in 6) | the F1 block in §1b (Oct 1 07:45Z): trace the two txns with `APPLY_LOG`, decide between `apply_unlink_cmd`'s silent NOT_FOUND→OK and an EXCL DEL that passes on an absent key, fix that one | evidence `/tmp/efs-mount/posix-2c-r422-6/peer_rename_vs_unlink_src/b` on 19810 (`-?????????`), `~/efs-runs/p2r422.log`, `results/posix2/20261001-073048`. Correctness: goes before 1a–1h |
| 1j | **Read path R1–R5 — DONE (Oct 1 15:45Z, all clients 16:01Z); server PUT bounce copy — rolled 18:00Z** | R1 a prefetched chunk's buffer is handed to the rdcache (`efs_rdcache_put_owned`), R2 a whole-chunk demand read decodes into the caller's buffer (`chunk_get_job.ext`), R4 the two data fragments are received straight into the chunk (`efs_decode_chunk` skips the self-copy), R5 a chunk-aligned READ whose chunks are all cached is answered with `fuse_reply_iov` over pinned rdcache images (`efs_client_read_refs`, `efs_rdcache_pin/unpin`; `rdcache_ent.pins` keeps a pinned way from being a victim); the get pool is 64 workers (`GET_POOL_N`, was tied to `EFS_WRITE_PIPELINE`). Server: an aligned PUT with a digest is one `writev` (payload + 4 KiB tail page), no bounce memcpy. **Still a copy:** RDMA recv buffer → chunk (zero-copy *receive* is a transport change — ask), and the FUSE write copy (W40) | fcstor007 16 GiB cold read **2.5 → 3.6 GB/s** (client CPU 21.6 → 10.8 s), 4 readers **3.7 → 6.5 GB/s**, prefetch 32 is slower than 16 (2.9); cmp/md5 OK, posix jobs=1 200/201 (`results/posix/20261001-154333`). Dirs `agent-rd-20261001-145047` (base), `-153035-inplace`, `-154007-refs`. `perf record -a` is refused at `perf_event_paranoid=1`; attach `-p` after the pools exist |
| 0f | **D24 · the close-time REPORT of a long sequential write — DECIDED and in the tree (Oct 1 14:00Z, user: "implement all fixes that prevent the lock")** | `report_landed_note` (write.c): every 8192 landed PUTs (1 GiB, global) kick the flush thread's whole-set REPORT; the REPORT reply wait is sized to the record count (`EFS_IO_TIMEOUT_MS + 0.5 ms × recs`, `rpc_send_recv_dual(recv_ms)`) so a long REPORT is not re-sent while the server executes it; both daemons stamp every log line (`log_ts.c`). Gate: the 14:00Z block in §1b | the 13:41Z wedge on fstor007 (`~/efs-runs/rec-look55x.log`): 8 × 20 GiB closes → `retry type=67 why=recv rc=-6` → `slow-ok attempts=6 us=116 s`, 16 dd in D-state in the kernel's forced FLUSH, ecopy's closes queued on `report_mu` for minutes. Still open as **W41**: `report_mu` serializes every close on a client behind one REPORT's retry loop (up to 8 s on STALE); per-inode dirty-set extraction + a multi-slot `pub_ino` set would remove the serialization point. Ask before building it |
| 1i | **W39 · RDMA zero-copy fragment send — DONE (Oct 1 15:30Z, all clients 16:01Z); W40 · FUSE write copy (ask)** | W39: bufpool slabs are registered lazily per HCA (`efs_rdma_zc_region_add`, `zc_lkey`), `efs_rdma_send_frame` posts a two-SGE send (header + slab payload ≥ 4 KiB) when `max_send_sge ≥ 2`; the payload is referenced until the send CQE, which every caller already guarantees (reply or `efs_conn_destroy`). Log once `efs: RDMA zero-copy send active`. W40 (ask): the `fuse_buf_copy` into the pool is the other full write copy; libfuse 3.10.2 has no custom-buffer receive — an own `/dev/fuse` loop | 16 GiB random dd+fsync on fcstor007 1.4 → 1.5 GB/s, client CPU 21.6 s (`agent-rd-20261001-151704-new`); cmp at 0/5/15 GiB and whole-file md5 OK. CPU, not wall |
| 0d | **W37 · `raft-mkfs` on a second node forks the salt — fixed, rolled 18:00Z (`dc6b0af1`); gate = the next fresh-table start** | `server_raft_host_mkfs` reads the salt the committed MKFS recorded (`efs_meta_apply_export_salt`: root shard's export key, else group 2's anchor) and uses THAT for the SALT step, never its own (`table exists with salt …; own salt … not used`); a node holding neither record answers BUSY (`retry on a group-0 node`); `EFS_MD_CMD_SALT` is on the apply's ring-only list so a mismatching SALT answers PROTO instead of halting group 2's apply. `efs_meta_apply_mkfs` keeps its contract (no-op on an existing root, first salt kept — `test_meta_apply` `idempotent`, `test_sim` `idempotent mkfs`; an EXIST-returning variant broke both and was reverted). Not exercised on a fresh table yet — the next `cluster.sh start --fresh` is the gate | `rec-st402..404` (`apply salt rc=-7 index=3` forever), `start401`, `wipe405` |
| 1a–1h | **W28–W35 · the 1 GiB dd review (Oct 1 05:10Z)** | eight items from the fstor007 write/read straces, server straces and profile — the subsection "W28–W35: what the 1 GiB dd showed" below this table; W28 (layout window off by one group) and W29 (prefetch/demand duplicate GETs) are one-function fixes and go first; D19–D22 are the asks it raised | 1 GiB write 207 MB/s = 1.6 s of writes with zero PUTs then a 3.56 s close; 1 GiB read 184 MB/s = a serial 80 KB GETCHUNKS over TCP plus ~400 futex per MiB. Both numbers were taken with strace on both daemons and are tracer plateaus; the shapes are not |
| 0b | **W27 · REPORT identity from the staging table** | (a) find the path that leaves a dirty chunk with neither a putid nor a dcache object (`write.c:1103`: putid table eviction, reclaim after `b6c1712d`'s pin release, or an irec-only threshold REPORT); one traced ecopy of a small-file tree on an idle cluster; (b) rerun on the current client first — if `putid miss` is 0 there, record and close; (c) only if (a) names the cause: a chunk with no PUT of ours is not ours to publish (keep dirty, replay from the row), as the `fragment_nodes[0] == 0` branch already does for span-only rows | ≥ 9016 recs (`n=8812…9016` in the last rate-limited second, all `ci=0`) reported with a mapping that "may be the server's row, not this client's PUT" — the Sep 30 (gen, off, len) / conflated-table class that lost ior-hard records. Forbidden: silencing the line, or "committing" such a rec client-side |

#### W28–W35: what the 1 GiB dd showed (Oct 1 05:10Z) and how to fix it

**Source.** fstor007 `dd bs=1M count=1024` write then read of `/tmp/efs-mount/001/dat17`, 01:06:20–01:07:20 EDT; client `strace -f` + `perf record`, servers `4b2844487831-dirty` with `--perf --strace` (restarted 01:03 EDT); straces and profile in `/home/erbmi1/orcd/scratch/efs/perf/efs-mount/`, the read-only passes in `~/efs-runs/ddana2.log`, `ddana3.log`, `ddana4.log`, `srvana2-fcstor00{3,4,5,6}.log`, `srvana3-fcstor004.log`. Both daemons were traced and both sat on the strace plateau (client 97–103 K lines/s during the read, servers 101–113 K/s during the write drain), so **207 MB/s write and 184 MB/s read are tracer numbers; every count below is real**. Untraced references: single dd write 918–1000 MB/s, single dd read 585 MB/s.

**Gate for the whole group (one script, run after each item, never with strace on a daemon):** on fcstor007, remounted, idle cluster: `dd bs=1M count=8192 conv=fsync` of a non-zero source to a fresh file, then remount, then `dd bs=1M` read of the same file; record MB/s for both, the per-`write`/`read` latency histogram from `strace -T` **on dd only**, and from the servers' `efsd.log` nothing (no recorders). Plus the counters each item names. The client bench (§1a, plan only) replaces this once it exists.

**Order:** W28, W29 (one function each, read side), W30, W31 (write side, mechanical), W32, W33 (both sides, mechanical), W34 (server, mechanical), W35 (measure), then the asks D19–D22. Correctness items (0a/0b above) still come first.

| # | item | what | measured | fix (mechanical unless marked ask) | done when | forbidden |
| --- | --- | --- | --- | --- | --- | --- |
| 1a | **W28 · layout window off by one group** | `efs_client_pull_layout_miss` (`ops.c:449`): `fresh` requires `seen.ci1 >= ahead` with `ahead = ci0 + 2·win`; `seen.ci1` is the previous call's `ahead`, which is one 8-chunk range short of this one's, so after the window ramps to 256 **every 1 MiB read issues a GETCHUNKS before its data GETs** | 94 `recvfrom … 79888` on the client in a 1024-read window (one per read per libfuse worker); each is the whole 64-entry group (64 × 1248 B), **> 72 KiB ⇒ TCP side channel**, ~1.3 ms request-to-reply, serial, ≈ 25 % of each read's 5.2 ms | freshness is "the **requested** range `[ci0, ci1)` is covered and the pull is < 200 ms old"; extend the window only when the covered range ends within `win` of `ci1` (pull `[seen.ci1, ci1 + 2·win)` then), not on every call. Keep the forced `have_ce = 0` re-pull semantics (peer spans, D1) and the 200 ms bound | a 1 GiB sequential read issues ≤ 40 GETCHUNKS (8192 chunks / 256-window steps), counted on the client (`recvfrom … 79888` or a counter); posix/posix2 unchanged; `peer_overlap_pwrite_chunk_straddle` and `basic_chunk_boundary` pass | dropping the per-batch re-pull, caching the map past 200 ms, or widening the 200 ms |
| 1b | **W29 · the read fetched every chunk it already had, and the prefetch fetched nothing** | **corrected 05:45Z from the server straces:** GET sequences per server 4096 / 4098 / 4098 / 4100 = 16392 ≈ 2 × 8192 — exactly two fragments per chunk, every path opened once, no parity, **no duplicate** (the earlier "2.6 per chunk" counted strace `unfinished` lines twice). So no prefetch GET landed: `prefetch_ahead` skips any chunk `efs_dcache_has` (`read.c:1213`) and this client had just written the file — every clean image was still in the dcache. The demand worker does the opposite: `efs_rdcache_get` miss → `efs_client_fetch_published_chunk` (2 GETs) → `efs_dcache_overlay`, which **copies the whole current dcache image over the fetched bytes** (`write.c:3168`, `dcache_image_current`). The 3 ms GET wave in every read was for data the client already had | 1032 GETCHUNKS and 4096 GETs on fcstor004 in the read window, one of each per MiB; servers at ~8 % of one core during the read (blake3 verify 16.6 % of that); client wave ~3 ms per MiB | (a) the demand worker asks the dcache first — `efs_dcache_copy` (image current and no spans), the same test the sub-chunk path already makes at `read.c:1364` — and fetches only on a miss; (b) on a remounted client (no dcache) the prefetch *does* run and, by code, a demand job refetches a chunk whose prefetch is in flight (`rdcache_hit` is false until it lands): give the rdcache a *pending* entry with a waiter so demand joins the in-flight job; (c) submit the next window's prefetch before waiting on the current batch, not after the copy-out | same-mount read-after-write of 1 GiB: server GETs = 0 (the dcache serves it; reads see unchanged bytes, sha256); **remounted** read: server GETs per 1 GiB = 16384 ± 1 %, rdcache hits ≥ 7 of every 8 demand chunks (counter under `EFS_DCACHE_TRACE`); the honest read number is the remounted one | a second cache, raising `EFS_READ_PREFETCH` to hide (b), zero-filling anything, serving a dcache image whose `base_gen` the map does not name |
| 1c | **W30 · flush starts at `close()`** | a file under the dirty-byte cap is never PUT while it is written: 1024 writes in 1.6 s with **zero PUTs** (poller 27 then 2 kicks/s), then `close()` = 3.1 s PUT drain + 0.36 s REPORT; wall = write + drain, not max | `dd_1m.strace.txt`: writes 0.5–1.3 ms each, `close(1) <3.563>`; `ddana2.log` close-phase kicks ~2000 per 100 ms from 22.4 s on | **D19, ask first (recommended yes):** a chunk whose 128 KiB are complete (sequential write moved past it) is PUT-eligible at once — the reclaim thread pool takes it from the per-shard dirty list without waiting for the cap; REPORT/publish stays at close/fsync/threshold (W2 unchanged: PUT is not publish). Cost: a chunk rewritten before close is PUT twice (new generation; the STALE/putid machinery already handles it) | 1 GiB dd+fsync wall ≈ max(write phase, drain) — the close phase under 0.5 s after a 1 GiB sequential write on the gate script; poller kick rate non-zero during the write phase; posix `concurrent_appends`, `mtime_monotonic_many_writes`, posix2 63/63 | publishing on write (forbidden by spec), raising the dirty cap, a per-file flusher thread |
| 1d | **W31 · one `mmap` per chunk in the write path** | `efs_fuse_write_buf` → `efs_buf_alloc(cs)` → pool empty → `malloc(128 KiB)` → glibc mmap threshold → fresh mapping + 32 page faults per chunk; the pool refills only when the flush returns buffers, which (W30) is at close | **8193 `mmap`** in the 1.6 s write phase; `clear_page_erms`, `do_anonymous_page`, `vma_alloc_folio`, `lru_add_fn`, `rmqueue_bulk`, `gup_*` ≈ 5 % of the client profile | the pool is sized to the dirty cap at startup (or grows in slabs of N chunks from one `mmap`), and `efs_buf_free` returns buffers to it instead of `free()`; alternatively `mallopt(M_MMAP_THRESHOLD, …)` once at start — pick the pool, it is already there | `mmap` count during a 1 GiB write phase < 64 (strace on the daemon for this count only, then off); the page-fault symbols leave the top 30 | growing the pool without bound (it must cap at the dirty cap + pipeline) |
| 1e | **W32 · flush concurrency = 16 chunks in flight** | each of the 16 `dcache_reclaim_main` threads PUTs one chunk (3 fragments) and waits for all three replies before taking the next; put-pool mutex `0x597680` 8450 WAKE / 8060 WAIT in 3 s | 8192 chunks / 3.1 s = 2.6 K chunks/s; Little: 16 × 128 KiB ÷ 5.5 ms ≈ 370 MB/s = the observed drain (the 5.5 ms is the server RTT under its tracer; the 16 is ours) | a reclaim thread keeps up to P chunks' PUTs in flight (send chunk N+1's fragments before chunk N's replies are back; reap in order) so in-flight = 16 × P, with P from `EFS_WRITE_PIPELINE` logic that `dcache_flush_keep` already has for the close path — one code path for both; shard the put-pool queue per thread or per node so the mutex is not shared by 16 threads × 3 fragments | at a fixed server RTT the drain rate scales with P on the gate script (P = 1 vs 4: ≥ 3×); `0x597680`-class contention gone from the strace (count only, then off); `test_data`/posix unchanged | more threads (D-class: the poller is not the lever), a second put pool, changing the fragment size |
| 1f | **W33 · ~400 `futex` per 1 MiB read, ~27 per fragment PUT on the server** | client get pool (`0x596960`: 34.7 K WAIT / 28.9 K WAKE per 1024 reads) and frag pool (`0x545540`: 27.2 K / 18.7 K) wake 32 + 128 workers through `pthread_cond_signal` chains, 8 chunk jobs + 16 fragment jobs + batch completion per read; server: a PUT is handler → `g_pool.lock` → `not_empty` → one of 20 `writer_thread`s → `run_job` → `done_cv` back to the handler (`writer.c:173–274`), plus ~3 eventfd kicks per PUT from the recv pollers (tids 951867/951947: 19.4 K `write` for 6172 PUTs); 86 K futex in the 6 s write window on fcstor004 | **398 futex + 78 read + 39 poll + 32 write ≈ 550 syscalls per MiB read**; at the tracer's ~100 K/s that is exactly 5.5 ms = 184 MB/s; untraced it is ~1 ms of CPU and scheduler per MiB and every read sampled shows a 2.2 ms tail before the last two completions | one wakeup per batch, not per job: submit the 8 jobs then signal once per idle worker needed (or hand jobs to workers through per-worker slots like `host_waiter_sleep`); the fragment fan-out of a chunk runs on the chunk's own worker (2 GETs issued back-to-back, one wait) instead of a second pool hop; server: same count on the writer pool, measured by `efsd --bench` first | futex per 1 MiB read ≤ 40 (count by strace, then off); the 2.2 ms tail is gone from an untraced dd read's per-`read` histogram (dd-side `strace -T`); server futex per fragment ≤ 6 | adding workers, spinning instead of sleeping, touching `recv_poller` |
| 1g | **W34 · server PUT = 3 `openat`, 2 inodes, and the drain is kernel time** | per fragment: `openat(O_WRONLY\|O_TRUNC\|O_DIRECT)` without `O_CREAT` **fails ENOENT** on every first write — **corrected 05:45Z:** this is the *quota* probe, not the path-hint one: `server_write_fragment_with_sum_sync` opens no-create first so an overwrite is not charged (`store.c:1259–1291`, `arg.no_create = 1`), on every PUT under `--quota` (19810 runs `--quota 36T`); W14.4b's `EFS_PATH_HINT_SKIP` only skips `server_find_fragment_root`'s `access()` walk — then `openat(O_CREAT\|O_DIRECT)`, `write` 64 KiB (50–60 µs p50), `close`, `openat(.sum, O_CREAT)`, `write` 32, `close`, 2 `fstat`; **no `fsync`**. Perf, write-drain bin on all four servers (`~/efs-runs/perfana2-fcstor00{3..6}.log`): **75 % kernel, 12–17 % `[xfs]`, 4–5 % efsd, 3–7 % libc**; of the kernel share roughly a third is the tracer (`ptrace_do_notify`, `ptrace_stop`, `__task_pid_nr_ns`, `syscall_trace_enter/exit_work`), the rest is file creation — `xfs_btree_lookup`, `xfs_cntbt_key_diff`, `xfs_buf_get_map`, `xfs_trans_read_buf_map` (inode + extent allocation for two new files) and the path walk (`__d_lookup_rcu`, `link_path_walk`, `inode_permission`, `step_into`: an 11-component path walked three times per fragment). efsd's own symbols: `server_handle_conn`, `efs_rdma_recv_wait`, `shard_io_thread`, `pick_write_path`, `server_write_fragment_with_sum` — none above 0.6 % | fcstor004 write window: 6144 no-`O_CREAT` opens (probe → create gap 176–188 µs p50 on all four), 6194 creates, 6150 `.sum` creates, 68 `mkdir`, 0 fsync; ≈ 55 syscalls per 64 KiB; ~2.4 cores per server during the drain, traced | (i) invert the quota probe: open `O_CREAT\|O_EXCL` first (new file ⇒ charge), and only on `EEXIST` take the no-create overwrite path — or skip the probe outright when `efs_tls_path_hint == EFS_PATH_HINT_SKIP` (a first write cannot exist); the common case pays one open; (ii) `mkdir` on ENOENT stays (68 per 6172 — D7 holds); (iii) **D20, ask:** the `.sum` sidecar as a second inode per fragment — options: a 32-byte trailer in the fragment file (read path `pread` one 4 KiB `O_DIRECT` block more), an xattr, or keep; on-disk format, so it is a decision; (iv) the path walk is 3 × 11 components per fragment — an `openat` relative to a per-thread parent-dir fd was tried once (Sep 28, "thread-local inode-dir fd", reverted because the slowest 9-client dd got worse) — do not retry it without the bench | no-`O_CREAT` `openat` count = 0 during a fresh-file write (strace on one server for the count, then off); fragment-creates per 64 KiB = 1 after D20; `efsd --bench` data kind prints the before/after ops/s at QD 64 and its `perf` shows `[xfs]` + path walk below 30 % | an `fsync` per fragment (spec: the Raft log is the durability boundary), a third file, a per-thread dir fd without the bench |
| 1h | **W35 · the 1248-byte chunk record** | REPORT for 1 GiB = `writev … 10,223,693` B = 8192 × 1248 B (1 % of the data) over TCP, reply 0.355 s, leader `pub_p50=227 ms` for that publish; GETCHUNKS reply = 64 × 1248 B = 79,888 B > `EFS_RDMA_BUFSZ` (73,728) ⇒ TCP for every window pull | the REPORT's 0.36 s is 10 % of the 1 GiB close; the GETCHUNKS frame size is why W28's RPC is a TCP round trip. **Server perf, the bin after the drain (fcstor004, g2 leader for this inode):** `__memmove`, `lookup`, `search_block`, `kv_mtab_pos`, `kv_seg_probe`, `kv_seg_iter_next`, `efs_crc32` — the publish apply is one KV point-get with a block CRC per chunk record, 8192 of them sequential ≈ 28 µs each = the 227 ms `pub_p50`; the server was at ~0.2 cores while doing it (latency, not CPU) | **measure first:** `tests/tools` dump of one encoded record — which fields are the 1248 B (3 × 32 B checksums, nodes, 8 × delta, generation, padding?); then **D21, ask:** a compact record for the common case (full image, no deltas: ~120 B) with the delta list only when `ndelta > 0`; and/or **D22, ask:** `EFS_RDMA_BUFSZ` ≥ 84 KiB so a 64-entry window and a 64 KiB fragment both fit RDMA (memory: 36 bufs × conns). Both are wire/pool constants with a spec | REPORT bytes per GiB ≤ 2 MB; GETCHUNKS and GET replies never take the TCP side channel on a healthy conn (`recvfrom … 79888` count 0 in a read); 9-client IOR hard-write fsync time falls with the REPORT size | splitting REPORT into several RPCs (forbidden), compressing on the wire |

**Not new, already queued, confirmed by this run:** the two user copies per written byte (`memcpy_erms` 5.7 % + `fuse_copy_*` 2.3 % kernel→libfuse, `memmove` 16.3 % libfuse→dcache) are W15 steps 3/5; blake3 4.3 % + `xor_into` 3.1 % are the arithmetic floor for the `cpu` kind of the client bench. The REPORT publish at 227 ms is D8's `pub_p50` with a 10 MB entry.

**Server perf and strace (reviewed 05:45Z, recorders stopped 05:27Z with the daemons):** `/tmp/efs-perf/efsd.data` on fcstor003–006 is `perf record -g -F 999` cycles, 1394 s, 24–31 K samples each — the daemon averaged ~2 % of one core; the write drain is the only hot bin (7279 samples / 10 s on fcstor004 ≈ 2.4 cores for 3 s), the read bin is 841 (≈ 8 % of one core, `blake3_hash_many_avx2` 16.6 % of it — the server-side verify on GET), and the bin after the drain is the publish apply (W35). Per-bin DSO split and the symbols are in W34; the strace counts in W29/W33/W34. The whole-run top is the tracer (`ptrace_do_notify` 5.4 %, `__task_pid_nr_ns` 2.9 %, `child_wait_callback` 2.7 %); the only background efsd symbol is `host_gc_frag_pass` → `lsm_scan_prefix` → `memcmp` at 0.7 %. **What the server data changes:** W29 (no duplicate GETs; the same-mount read refetched what the dcache held and the prefetch never ran), W34 (the dead probe is the quota overwrite probe, and the drain is XFS creation + path walk, not efsd), W33 (the 27 futex is the handler↔writer-pool hand-off). W28, W30, W31, W32, W35 stand. Nothing on the server side limits the read: the read ceiling is the client's serial per-MiB chain. Scripts: `~/efs-runs/perfana.sh`, `perfana2.sh`; logs `perfana{,2}-fcstor00{3..6}.log`. The data files stay on the nodes (`/tmp/efs-perf/`, 170–375 MB strace each) until the user removes them.

In tree and rolled (Sep 29 04:08Z, `54a500da9dc8-dirty`, RDMA, `--perf --strace`; the 02:35Z roll was `bbcbcb5ad779-dirty`), not gated by a suite: 1 (sync report loop: 8 s checked between REPORT attempts, plus whatever one in-flight RPC takes; dirty set merged back; fsync returns EIO), 2 (`fuse_stat_errno`), 3 (dirty list + `dcache_ensure` so `pthread_once` is not on the lock), 4 (`send_ae` reads the log into the wire buffer; `try_commit` starts at the quorum match), 5 (PUT reply carries the storage root), 6 (a 1 MiB write walks chunk-aligned pieces; each whole chunk is one `fuse_buf_copy` into a dcache-owned buffer; a partial head or tail bounces its own length), 6a, 6b, 7's snap-tmp reap and `send_buf_pick`, 7' (an AE reply replaces an older queued reply to that peer, a heartbeat replaces an older heartbeat, a vote is not evicted, and the sender drains that lane before entry AppendEntries; a full entry lane returns AGAIN and does not increment `st_drop`), 7'' (`efs_rdma_send_frame` posts and returns; the reply wait reaps send CQEs and treats a send still busy after 5 s as a dead QP), 8, 9 step (a), 8a (snapshot by 512 MiB with a retained window), 8b (import diff in 1024-key slices), 8c (`EFS_PATH_HINT_NEW`), and the `pub_p50`/`pub_max` line of 10. 8d's `--meta-storage` flag is in and defaults to the first `--storage` root; `mdraft/` was not moved and the shared-vs-quiet `fsync` measurement was not run. 8h and 8i / D12 were rolled 12:29Z. The 12:36Z IOR (`results/io500/20260929-123635-rdma`) finished ior-easy-write at 2.218 GiB/s and mdtest-easy-write at 1.765 kIOPS, then ior-hard-write aborted on fsync (31 failures, no bandwidth) and fcstor012's ranks stuck in D behind a reclaimer holding the shard I/O lock across a fragment GET. The 13:17Z copy failed a different way: `fsync` returned EIO because D12 rewrote L1 for the whole ingest (D13). 8j / D13 is in the tree (14:20Z), not rolled, not gated. The shard-lock drop is in the tree (14:50Z), not rolled. Next is 10's one IOR, 7 step 2 (c) (measure, then ask), 11. 8e, 8f, and 8g were rolled 05:31Z, including the 1 GiB L0 byte cap; the 12:02Z trace is their measurement (§1b). In tree and rolled with that same build: W23 client (pool generation instead of `fstat`; liveness probe only after 1 s idle), W24 step 2 (one 8 s deadline around truncate's flush, REPORT, and SETATTR), W25 (`futimens` proposes the utimens command on the other group for cross-group lanes before the inode-group entry — not `LANE_FENCE`, which would delete chunks; `atime_nsec` on the SETATTR wire; `EFS_ERR_BUSY` is EBUSY). The three `test_meta_apply` cases named under W17 step 3 are in `test_chunk_deltas` (Sep 29 15:40Z): replay of a folded span is a no-op, a span after the fold attaches to the new base without naming it, overlap with a live span is STALE (a tombstone's old range is free), and a full image after the fold must name the current base. The 04:08Z roll and the 02:35Z IOR analysis are in §1b.

Prior recommended answers, for the record:

| item | question | recommended (server perf, Oct 1) | why, in one line |
| --- | --- | --- | --- |
| W13 | full-L1 compaction holds the apply path 2.4 s and costs a term | **done Sep 26**, partitioned flush rolled Sep 27 on TCP | background compactor kept `apply_max` under 70 ms. Flush now writes one L0 file per `key[0]`; one compact leaves the other range's L1 file in place (`test_kv_lsm`) |
| W11 | the KV export is larger than one 4 MiB SNAP command, so the log never truncates | **done Sep 27** — chunked InstallSnapshot of a file; import is a sorted diff | logs under 5 KB; fcstor005 rejoined in 510 ms; `apply_max` 0 on a 386 MB export. 9-host posix 200/201 (`results/posix/20260927-123717`) |
| W9 | bound the client staging table | **done Sep 27** — in-flight pin, append-reservation pin, chunk maps before the row, and the evictor walks past a pinned oldest window | a cold stat of 1M files leveled at 233 MB RSS (`results/measure/20260927-w9-walk`). posix 2 is 63/63 (`results/posix2/20260927-123946`). Leaks are clean (`results/leaks/20260927-035622`) |
| W10 | RDMA needs an empty table to gate | **live Sep 28**, suites pass, not faster than TCP on posix or the 9-client write | 19810 is RDMA (`db2b88c4802a-dirty`). jobs=1 after user xattr is 200/201 (`results/posix/20260928-043918`); 9-host before that was 199/201 in 57–59 s; posix2 63/63; 9-client dd **1326.6** vs TCP 1478 |

---

#### W1 — Shared-file (N-1) writes from two clients silently lose data — DONE

**Done Sep 18 2026** (working tree on leftover-1 19810, TCP). I12 CAS is
live: `efs_chunk_rec.base_gen` + `chunk_generation` on the wire, leader
checks the writer's base before propose, apply STALE is audible
(`EFS_INODE_RPC_STALE=11` / `EFS_ERR_STALE=-14`) and does **not** stall
`last_applied`, client refetch+overlay+PUT retries. Fragments are
`{ci}.{fi}.{gen}`; GET uses `fragment_path_at` and mints the export shell.
Report identity comes from the last PUT (`putid`), not a GETCHUNKS stub.

Full text (measurements, steps, forbidden list) is in [project-history.md](../project-history.md), "START-HERE closed items".

#### W2 — `write()` is specified as durable-and-visible; the code buffers — DONE

**Done Sep 18 2026**, option (i): the spec moved. [architecture.md §3](../architecture.md)
now lists three deviations; a returned `write()` is client-buffered;
durable + cross-client visible at `fsync` / last `close` / `O_SYNC`.
`O_SYNC`/`O_DSYNC`/`-o sync` is specified write-through and is **not
wired**. Do not implement publish-on-every-`write()` — that is the
rejected 10× throughput change.

Full text (measurements, steps, forbidden list) is in [project-history.md](../project-history.md), "START-HERE closed items".

#### W3 — Split the single-client fsync tail, then remove the larger half — DONE

**Done Sep 18 2026** (leftover-1 19810, TCP). Measure said REPORT owned
73 % of the 8 GiB `fsync` (flush 5.7 s / report 15.0 s / wall 25.8 s).
Cuts, in order: `HOST_PUB_BATCH_N=2048` + skip `get_chunk` when
`base_gen==0`; raft-log `sync_hold` across the report's proposes;
client flush pipeline (`dcache_steal` + put pool); PUBLISH follower
forward is `host_propose` not `host_propose_wait`; O_APPEND fetches a
published mapping before sparse RMW; truncate/symlink report only that
ino; FUSE `flush` waits `report_dirty_ino(ino, 1)` (W2 close is
durable — kick-only left `i_size` 0 so the next O_APPEND wiped the
prefix). N=4096 did not win. AE 1 MiB is ~5168 pubs; deeper batching
is exhausted.

Full text (measurements, steps, forbidden list) is in [project-history.md](../project-history.md), "START-HERE closed items".

#### W4 — 4-client and 9-client honest fio and dd — DONE

**Done Sep 18 2026.** Writes share a ceiling and more clients make it
worse. Gate: `results/perf/20260918-w4-honest4/gate.txt`.

Full text (measurements, steps, forbidden list) is in [project-history.md](../project-history.md), "START-HERE closed items".

#### W5 — Re-measure `sw-50g` after W3 — DONE

**Done Sep 18 2026** as the W4 morning 50g row:
`results/perf/20260918-w4-honest/` sw-50g **341** / sr-50g **2203**,
`FUSE_OK` `err=0`. W1 was 373. Later loaded reruns laid 50 GiB then
`end_fsync` EIO (the same REPORT tail). Completes when the path is
healthy; do not treat EIO as a reason to raise `EFS_IO_TIMEOUT_MS`.

Full text (measurements, steps, forbidden list) is in [project-history.md](../project-history.md), "START-HERE closed items".

#### W6 — Run IO-500 (IOR easy, IOR hard, mdtest) — CORRECTNESS DONE, perf residuals open

**Correctness gate met Sep 20 2026** (commit `cc828d8`, servers
`708b350`+, TCP, 9 clients × 4 ranks): 9×4 debug
`results/io500/20260920-debug-9x4/` — **every phase finished, ior-easy-read
and ior-hard-read 0 verification errors, every unlink OK**. The path from
the Sep 18 numbers (hard-write DNF in 2 h 18 m, `-W` 4244 errors, 76108
easy-read errors, 27 undeletable files) to this is in
[../project-history.md](../project-history.md) "W6"; the fixes were: client
STALE retry cost + server partial-commit on STALE (hard-write livelock);
`dcache_image_current` / `snap_seq` ordering / forwarded-cmd reply index
(read coherency, `concurrent_appends`); Raft follower dedupe + leader AE
flow control (005 catch-up); **duplicate ino on concurrent CREATE**
(`alloc_hint_or_next` — apply is the allocator); reaper `lane-sweep`
batch-full misread as error (no file >8 MiB/lane was ever reclaimed).

| phase | Sep 30 18:35Z 9×4 RDMA | Sep 20 9×4 | Sep 18 9×1 |
| --- | --- | --- | --- |
| ior-easy-write | **4.563 GiB/s** | 0.814 | 0.263 |
| ior-hard-write | **0.640 GiB/s** (52.6 s, 0 fsync fail) | 0.044 (495 s) | 0.025 (9×4: DNF) |
| ior-easy-read | 16.8 GiB/s same-mount, 0 errors | 1.80, 0 errors | 0.60 (`-R` 512 errors) |
| ior-hard-read | **1.034 GiB/s, 0 errors; cold hardscan bad=0** | 3.55, 0 errors | 1.26 (`-R` 15982 errors) |
| mdtest-easy-write | **4.632 kIOPS** | 0.050 | 0.053 |
| mdtest-hard-write | 2.612 kIOPS | — | — |
| mdtest-easy-stat | 16.478 kIOPS | — | — |

Sep 30 row: `results/io500/20260930-183504-rdma/NOTE.txt` (§1b top).
Not a list submission (stonewall 1 s). Do not quote the Sep 19 easy-read
4.0 GiB/s — it was zero-fill. Do not quote stonewall intra GiB/s.

**Open under this item (performance, not correctness):**

1. **ior-hard-write** — **spans are the data path (Sep 27).**
   NP=4, SEGS=3000, 47008 B, one file, cold remount: write
   **481.56 MiB/s** (1.12 s), read **91.35 MiB/s** (5.89 s), pattern
   12000 records bad 0. posix2 **63/63**
   (`results/posix2/20260927-190509`). Prior curve
   `results/measure/20260921-162514-ior-hard-scaling`: **372 / 33 /
   69 / 82 MiB/s at 1 / 4 / 9 / 36**, about half of REPORTs
   `EFS_ERR_STALE`. The 4-rank point moved 33 → 482. 1/9/36 were not
   remeasured: group 2's REAP_DONE tail stayed ~11–16 entries/s
   (distinct inodes) and preflight fails above 5/s. Do not raise it
   (**D3**). Do not add a chunk lock. Do not tune 47008.
   **Sep 28, NP=9 via `run.sh ior`: STALE storm, fsync EIO, D-state
   ranks — W17.** **Decision D1 (Sep 28): the span publish commutes
   (no base-gen CAS for spans; sub-range writers always publish spans;
   fold by the chain-filler or a reader). Implement as W17 step 3.**
2. **1 GiB open costs 20 s of a 22 s easy-read** — 128 sequential
   GETCHUNKS + a 64-lane stat per open. Spec §8 per-lane range fetch
   ([performance.md](performance.md)) is the fix. **Decision D2 (Sep
   28): `open()` adopts the inode row only; `pull_layout_miss` is the
   one pull path, one GETCHUNKS per lane group in parallel, a metadata
   window one data window ahead of the prefetcher, size adaptive and
   internal (no knob).** Steps: (a) remove `pull_file_layout` from
   adopt for regular files; (b) make `pull_layout_miss` issue the
   window's lane-group GETCHUNKS concurrently and record the covered
   range per ino; (c) have the read prefetcher request the next
   metadata window when the data window advances; (d) keep the 64-lane
   size stat at open (size authority is not the map). Gate: 1 GiB cold
   open under 10 ms with 36 concurrent openers
   (`tests/measure/` open-cost script); easy-read not worse than
   `results/io500/20260928-150609-rdma`; `-R` errors 0 on both reads;
   posix 1 jobs=1 200/201. Forbidden: a whole-map pull anywhere;
   zero-fill on a miss; a mount option or env var for the window.
3. ~~mdtest `rmdir` ENOTEMPTY/EIO under load~~ — **FIXED Sep 21** (it was
   the parent-row lost update, not transient; §7.2 reductions). **Rate
   fixed Sep 27.** The flat 138 / 134 / 159 ops/s
   (`results/measure/20260921-161931-samedir-rate`) was one AppendEntries
   per create: a proposer that arrived while a fsync hold was open
   broadcast that single entry under the raft lock. Those proposers now
   append locally; the hold owner fsyncs the burst and marks every
   covered index durable; the pump sends one batch.
   `results/measure/20260927-211953-samedir-rate` (ROUNDS=100, storm
   PASS, parent `children=0 nlink=2`): **333 / 1385 / 1241 ops/s at
   1 / 9 / 36 procs**, `busy_n` 0 / 0 / 1. Idle mkdir median 3.1 ms.
   Do not change the backoff or the txn protocol. Do not lower
   `EFS_DIR_SPREAD_MIN`.

Harness (`tests/perf/io500/run.sh`): `SLOTS=4 NP=36 … debug` detaches
`prterun` and logs to `$IO500_DIR/last-run.log` (the ssh timeout used to
kill it); `ior-easy-write|verify <mb>` and `ior-hard-write|verify <segs>`
run IOR directly so a cold verify is possible (the io500 driver deletes
its data at the end of a run).

- **Read:** `tests/perf/io500/README.md`; the fio rule's FUSE check
  applies to every rank.
- **Gate (met):** 9×4 debug with 0 `-R` errors on both reads. For the
  three open sub-items the gate is the number in the table moving with
  the same harness and 0 errors kept.
- **Forbidden:** quoting a rank that fell back to local disk; tuning
  IOR's transfer size (47008 is the point); a chunk lock (W1); `pkill -f`
  (matches the agent). Kill hung `io500` with `pkill -9 -x io500` then
  remount FUSE (D-state `request_wait_answer` ignores SIGKILL until
  `efs-fuse` dies). Never gdb-attach an MPI rank through a timeout'd ssh
  (left a rank T-stopped, job unrecoverable).

#### W7 — Two POSIX suite-1 tests exceed the 15 s budget even in isolation — DONE

**Done Sep 21 2026.** The 15 s failures were the metadata wakeup floor
(fixed Sep 20), not a remaining per-test bug. Isolated on an idle cluster,
`results/measure/20260921-133437-posix-isolated` (budget 15 s, jobs=1):

Full text (measurements, steps, forbidden list) is in [project-history.md](../project-history.md), "START-HERE closed items".

#### W8 — 9-node POSIX suite 1 (gate: 201 rows, 0 NOTRUN, every host)

**State (Sep 21 22:15): 191–195 / 201 on every host, 0 NOTRUN, all nine
finish in ~62 s** (`results/posix/20260922-020950`). History and the
three fixes that got here are in the §1b progress log (harness clock at
submit; `h->mu` contention after `read_mu`; KV WAL fsync per apply).
Earlier symptoms — nine hosts at the 385 s cap with `[None]` rows
(`results/posix/20260917-191430`), the 1.2 s / 1.03–1.08 s root mkdir,
`rmdir` ENOENT on a just-created name, `EBUSY` after the 10.4 s backoff —
are closed: whole-shard txn scans (`165e779`), stranded txn records
(`9534e53`, `3291c6d`), `read_mu` (`223da15`), peer-pool starvation
(`f10fec0`), harness (`a683def`), view (`4eb1419`), WAL hold (`84a2a55`).

What still fails, in order (details and instructions in §1b):
0. Half-applied cross-shard txns (I17) — **fixed `46d54e6` and gated**
   (Sep 22): freeze both leaders during `same_parent_storm`. The parent
   row stayed consistent (`nlink=5 nents=3` with three real children on
   the run that left names behind; the other run removed the parent).
   `arc_term_miss` moved. Details in §1b.
1. Retry of a committed non-idempotent op after a BUSY (EEXIST on a
   fresh LINK name, EIO, empty read) → I16 op-id dedup for
   LINK/UNLINK/MKDIR/RENAME. Mechanical, spec §7.9.
2. The synchronous full-L1 compaction: 2.4 s under `h->mu`+`l->mu` on
   every replica at once, costs the leader its term. **Decision.**
3. Six many-op tests at 144 concurrent jobs (mkdir p50 21 ms under
   load; apply 0.3 ms/entry of KV reads). Measure, then decide.

Run it as `bash tests/measure/w8_stall_timeline.sh` (runs the suite,
gives the raft/latency timeline) and read `raft-obs:` from the four
`efsd.log`s. Do not raise the 70 s warmup, the 15 s per-test budget,
or the 400 s suite timeout; do not point the suite at a subdirectory;
do not raise the election timeout.

- **Forbidden:** reporting `[None]` rows as failures, or as passes;
  the clock-at-submit bug coming back (a queued test cannot time out).

#### W9 — The client staging table is unbounded

`g_client.export` in `efs-fuse` keeps one row per inode this client has ever
touched and one entry per chunk it has written or pulled, and evicts nothing —
so a client that walks a large namespace holds the whole tree in RAM and the
server-side memory wall reappears per client. This is step 12 part A.
The evictor and the pin rules are in the client as of Sep 27.

The plan, the pin rules that make eviction safe (a report builds its records
out of this table, so evicting a dirty row is data loss), and the gate are in
[../client-cache-design.md](../client-cache-design.md). The Sep 23
recommendation below is in the client as of Sep 27. Posix 1 is
200/201, posix 2 is 59/63, the leak gate is clean, and a cold stat
of 1M files leveled at 233 MB RSS
(`results/measure/20260927-w9-walk`). Done.

**Recommendation (Sep 23), implemented Sep 27:** (1) The pin rules —
dirty / publishing / dirty dcache, ghost with an open fd, in-flight op pin,
live append reservation or lock — are exactly the set of rows whose absence
a report or an open fd could observe; anything narrower loses a write,
anything wider is not a bound. (2) `EFS_CLIENT_META_MB` = 256 MB default,
soft: at ~200–300 B per row plus chunk maps that is on the order of a
million rows, above any FUSE working set we have measured (IO-500 9×4,
the posix suites, `find` over the 410-name root), and a table where
everything is pinned grows and logs once rather than evicting work. Pick
a different number only if a client RSS measurement says so; the cap is
an env var, not a protocol.

Steps, once ratified:
1. Pin bookkeeping: a per-row pin count set by the dirty/publishing sets,
   `efs_open_note`/`close_note` for ghosts, and a scoped pin in every
   dual-apply window (create, rename, link, unlink). Unit test: a report
   built while eviction runs never skips a dirty row.
2. LRU by last-touch tick over unpinned rows; evict chunk maps of clean
   closed files first, then rows. Ghost reclaim at last close.
3. `statfs` from server numbers; `efs_export_fits_page_cap` becomes a
   best-effort early-out (the doc's §4).
4. Gate: `make test`; posix 1 (jobs=1 and 9-host) and posix 2 with no new
   failures; the walk-RSS gate — one client walks a multi-million-file
   tree and RSS stays near the cap where it grew linearly before; the
   valgrind leak gate (eviction is a new free path).

- **Gate:** the walk-RSS gate in that doc, plus posix 1 + 2 and `leaks`.
- **Forbidden:** evicting a row that is dirty, publishing, a ghost with an open
  fd, or pinned by an in-flight op. Bounding it by dropping records instead of
  refetching them.

#### W10 — RDMA — live on 19810 Sep 28, not faster than TCP

Sep 28 gate on `db2b88c4802a-dirty` (`EFS_TRANSPORT=rdma`,
`EFS_RAFT_OBS=1`). posix jobs=1 199/201 in 90.1 s
(`results/posix/20260928-033823`); 9-host 199/201 on all nine,
56.7–59.1 s, 0 fail, mmap + xattr SKIP
(`results/posix/20260928-034049`); posix2 63/63 in 76.8 s
(`results/posix2/20260928-034350`). After user xattr, jobs=1 is
**200/201** (`mmap_write_read` SKIP only,
`results/posix/20260928-043918`); 9-host was not remeasured.
9-client 8 GiB dd+fsync
**1326.6** MiB/s, walls 28.55–55.57 s, every file 8589934592
(`results/measure/20260928-033420-dd-wall`). TCP bars are posix
9-host ~13–15 s and 9-client dd 1478. The outbox fix that let
fcstor004 catch up is in this build. The speed bar in step 3
below is still open. Do not roll back to TCP unless a suite fails.

The Sep 27 live switch below was rolled back the same hour. It is
history, not the current cluster.

Private gate passed 5/5 (`tests/rdma_first_inode.sh` on fcstor007).
The live switch (`EFSD_ENV='EFS_TRANSPORT=rdma EFS_RAFT_OBS=1'`,
`roll_efsd.sh --all`, clients remounted `EFS_TRANSPORT=rdma`) did not
match TCP. 9-host posix jobs=1 was 193–196/201, skip
`mmap_write_read`, 0 not-run, duration 385 s on every host
(`results/posix/20260927-044348`). The TCP bar on this tree is
200/201 in 30.4–31.3 s. What failed that passes on TCP: the many-op
tests (`dir_deep_nesting`, `dir_deep_nesting_beyond_64`,
`names_crazy_dirs`, and on some hosts `concurrent_creates_same_dir`
and `mtime_monotonic_many_writes`) hit the 15 s cap, plus a few
EIO/EEXIST one-offs. During that run `apply_max` on fcstor003 stayed
under 1 ms. Step 5 rolled 19810 back to
`EFS_TRANSPORT=tcp EFS_RAFT_OBS=1` the same hour; clients
fcstor007–015 are TCP and a mkdir/rmdir on fcstor007 returned
immediately. Freeze, idle `md_latency`, and the dd rebaseline were
not run on RDMA. Do not debug this on the live cluster.

The private-cluster gap (100 mkdirs, fcstor007, ports 19950–19952)
was two bugs in `src/common/rdma.c`, both fixed in tree and not
rolled. The shared recv poller acked the completion-channel event
and then slept 100 ms, so a work completion already in the CQ
stalled every conn (11 of 100 mkdirs; 9-host posix then hit the
15 s cap). It now harvests again before a 1 ms backstop. Every
SEND also called `ibv_query_qp` and read a port counter, which made
a raft AppendEntries ~380 µs against ~15 µs on TCP; those reads
happen only after a send has already failed. With both fixes, one
clean run was 642 ms RDMA vs 507 ms TCP and raft RTT ~50 µs
(`~/efs-runs/rdmaprof7.log`). The Sep 28 live gate is the paragraph
above this section. 19810 is RDMA. The "same numbers as TCP or
better" speed bar is not met.

The steps below are the procedure that was followed, kept so the
gate stays findable.

The client connection-pool lifecycle fix is in tree and unit-gated
(`test_conn_fd`): a pooled conn pins its socket identity, checkout evicts on
mismatch, destroy refuses to close a recycled fd. The **live** repro was never
re-run, because it only reproduces on a freshly `mkfs`'d / effectively empty
table, and 19810 is populated. A remount there is *not* this gate.

19810's transport is RDMA as of the Sep 28 gate in §1b. Suites pass.
Posix and the 9-client write are still slower than TCP, so the speed
bar in step 3 is open. Every ceiling in the table above assumes the
data path can use the fabric; TCP over IPoIB will not reach it.

**Recommendation (Sep 23): do not wipe 19810 for this.** The repro needs an
*empty* table, not *the* table: `tests/rdma_first_inode.sh` stands up a
private 3-node efsd on one host (fcstor007, ports 19950–19952, storage
under `/tmp/efs-rdma-first`, `EFS_TRANSPORT=auto`) and runs the first
`mkdir`. That is the gate, and it touches nothing on 19810. The live
switch of 19810 to RDMA then needs no fresh table either — the project
state rule records that a populated table drove millions of creates over
RDMA with 0 errors; the empty-table hang was the only open defect. A wipe
buys nothing here that the private cluster does not; keep it for W11
(below), whose gate is easier on a table that grows from zero.

Steps:
1. `efs-bg.sh start w10-first 'bash tests/rdma_first_inode.sh'` with
   `EFS_RUNNER=fcstor007.ib`, ×5 (the tree must be built on 007 first:
   `efsd`, `efs-mgmt`, `efs-fuse`). Pass = every run prints
   `MKDIR_RC=0 WRITE_RC=0` and exits 0 (the first `mkdir` and a write
   inside it each have an 8 s `timeout`). Fail = any `MKDIR_RC=124`, and
   the fix is not in; stop and bring `/tmp/efs-rdma-first/fuse.log`'s
   `RDMA transport|WAIT TIMEOUT|SOCKET CLOSED` lines.
2. Switch the live cluster: `tests/roll_efsd.sh --all` with
   `EFSD_ENV="EFS_TRANSPORT=rdma EFS_RAFT_OBS=1"`, then remount the
   clients with `EFS_TRANSPORT=rdma`. Wait for `commit` flat on both
   groups (post-roll election churn is ~2–5 min).
3. Gate on RDMA, same numbers as TCP or better: posix jobs=1 (200/201),
   9-host posix (≥185/201, 0 not-run), `i17_leader_freeze.sh` ×2 with
   0 worker errors, idle `md_latency.py` within the TCP reference.
4. Re-baseline the write wall (`efs-fio-honest`: 8 GiB dd+fsync 1/4/9
   clients) and record it in §1a's ceiling table. Then TCP is no longer
   the default in the deploy rule and START-HERE.
5. If step 3 fails on anything that passes on TCP, roll back to TCP with
   the same `roll_efsd.sh --all` and bring the failure; do not debug
   RDMA on the live cluster with the suites down.

- **Read:** the root cause and what was already disproven is in the project
  state rule — the RNR-NAK/recv-buffer hypothesis is **dead**, do not re-chase
  it.
- **Forbidden:** re-deriving the diagnosis; wiping 19810 without being asked
  (this item no longer needs it).

#### W11 — chunked InstallSnapshot — DONE Sep 27

Running build `612ee9ba9202-dirty`, TCP. The log truncates. A follower
install is a sorted diff (`src/kv/kv_snap.c`). fcstor005 rejoined in
510 ms. 9-host posix is 200/201, skip `mmap_write_read`, 30.4–31.3 s
(`results/posix/20260927-033723`). The earlier 199–200 run is
`results/posix/20260927-015719`. Gate numbers are in
`results/measure/20260927-w11-gate` and `docs/project-history.md`
(Sep 27). The original recommendation, kept so the steps stay
findable:

Full text (measurements, steps, forbidden list) is in [project-history.md](../project-history.md), "START-HERE closed items".

#### W13 — Synchronous full-L1 compaction is the remaining election trigger — DONE Sep 26 2026

Gate is at the top of §1b. Steps 1–4 landed: pinned view, background
compactor, flush no longer compacts, and the 9-host suite kept
`apply_max` under 100 ms with no term change. Step 5 (partitioned
flush) is in the TCP build rolled Sep 27 05:07 UTC. A flush writes
one L0 file per `key[0]` (at most 16 on efs keys, shard in the first
two bytes). Compaction merges one of those ranges plus the L1 files
that overlap it, and cuts an output file when the range changes, so
the next cycle does not rewrite the other ranges. `make test` on
fcstor014 passed, including `test_partitioned_flush`. The live table's
existing L1 files are still wide until the next compaction rewrites
them; the byte bound is the steady state after that.

Full text (measurements, steps, forbidden list) is in [project-history.md](../project-history.md), "START-HERE closed items".

#### W14 — Server: snapshot install and the fragment probe are on the write path

**Status (Sep 28 evening).** Step 1 is done in `7eecf1d` (the diff is
prepared on the GC thread; the pump applies it on the ack) and
`bbcbcb5` (the pump's `host_snap_open` no longer flushes a memtable it
cannot place; a no-progress snapshot ack waits one heartbeat). The
`send_snap` and `kv_flush_locked` stacks are gone from the run-3
profiles. Step 3's reap runs at `efsd` start. Step 4 carries
`path_hint` on the next PUT and probes the other roots only on a
miss. Step 5 sizes the send-buffer pool to `EFS_WRITE_PIPELINE`.
Step 2 (the election trigger) is still open. The
L1 cap that kept fcstor004/005 from compacting is removed in
`bbcbcb5` (growable L1 list; L0 still 64).

**Source (Sep 28 2026, 13:30–13:45 EDT).** `perf record -F 499 -g` attached
to all four `efsd` on 19810 (RDMA, `2f086f5a0ef8-dirty`) while fstor007 ran
`ecopy` and then a `dd` that did not start. Reports:
`~/orcd/scratch/efs/perf/efsd-19810-fcstor00{3,4,5,6}/{flat,by_thread,callers}.txt`.
The recorded binary was replaced on disk by the roll, so user frames are
raw addresses in the reports; resolve them with
`addr2line -f -C -e /tmp/efs/efsd <addr>` on that host (the file there is the
same build). fcstor005, 265K samples:

| share | stack | what |
| --- | --- | --- |
| 16% | pump → `drain_inbox` → `on_snap_req` → `host_snap_chunk` → `efs_kv_group_import` → `efs_kv_scan` (`lsm_scan_from`), `memcmp` 12% self | a follower installing a snapshot: full local KV scan under `l->mu`, then sort + diff of every key, **on the pump thread under `h->mu`** |
| 12% | `writer_thread` → `run_job` → `server_write_fragment_with_sum_sync` → `pwrite` | fragment writes (the useful work) |
| 10.6% | `server_handle_conn` → `recv` (`copyout`) | PUT payloads over TCP |
| 10.4% | `server_handle_conn` → `server_find_fragment_root` → `access()` ×6 (`__d_lookup_rcu`, `link_path_walk`) | six path walks per PUT to find which of the six roots has the fragment |
| 8.6% | `host_gc_thread` → `host_snap_export_pass` → `vx_pop` / `msort` | a leader exporting a snapshot |
| 7.5% | `compactor_main` → `kv_compact_locked` → `cm_push` | L1 compaction |

`raft-obs` in the same window: group 0 changed term at 13:31, 13:37, 13:40,
13:43–13:44; group 2 at 13:39–13:40. Every leader change re-exports and
re-ships a snapshot. Every server holds 5–12 abandoned
`/data1/01/efs/mdraft/snap-*.kvx.tmp` files (34 MB–960 MB each, from 00:41
through 11:24 that day); nothing removes them.

**Why the `dd` did not start.** `create` is a group-0 commit that waits in
`host_wait_settled` for the pump. The pump was inside
`efs_kv_group_import` under `h->mu`, and group 0 was re-electing. The
client's 16 BUSY/NOT_PRIMARY retries back off to 800 ms each, so the
`open()` sat for over ten seconds with nothing printed.

Steps, in this order; each is its own change with its own gate:

1. **Take `efs_kv_group_import` off the pump thread.** The scan and the sort
   run on the GC thread (the export already does). Only the resulting diff
   batch is applied under `h->mu`. `on_snap_req` hands the finished `.part`
   file to that thread and answers the leader when the diff has been
   applied. Gate: `perf` on a follower during a forced InstallSnapshot
   (`tests/measure/i17_leader_freeze.sh` or a bounce of one voter) shows
   `efs_kv_scan` off the pump, and `raft-obs` `apply_max` stays under
   70 ms while the install runs.
2. **Find the election trigger and stop it.** Run
   `tests/tools/raft_log_tail.py` on fcstor004's `raft.log` for the
   13:37 and 13:43 windows. If step 1 removes the term changes under the
   same load, this is done. If not, the follower that cannot answer
   AppendEntries during an import needs the heartbeat answered from a
   thread that is not importing. Do not raise the election timeout.

   **Measured cause candidate (Sep 28 22:19 EDT, after step 1 landed in
   `bbcbcb5`).** Terms still move: group 0 9309 → 9368 and group 2
   2763 → 2789 between 00:39Z and 02:19Z with no roll in between.
   fcstor004 (`raft_id` 1; follower in group 2 whose leader is
   fcstor006 = `raft_id` 3) prints
   `raft-obs: tx->3 enq=50698 drop=72114 sent=50695 hi=2048`: the
   per-peer outbox to the group 2 leader hit `HOST_OUTBOX_MAX` (2048)
   and **dropped more frames than it sent**. `outbox_must_keep` protects
   only `SNAP_REQ` and entry-carrying `AE_REQ`; the victims are
   AppendEntries **replies**, heartbeats, and vote traffic. A leader
   whose follower's replies are dropped re-sends after the `ae_inflight`
   timeout and never sees the match advance; a follower whose heartbeats
   are dropped on the way in campaigns. `rtt_avg=239us` on that link and
   a synchronous request/reply `host_sender` bound one peer link to
   roughly 4 000 frames/s, which the leader's AE rate under 36 writers
   exceeds; the excess is dropped, not queued. `wait_timeouts=366`
   on the same node are the client-visible BUSYs (W16).

   Fixed implementation, in this order:

   **(a)–(b) are in tree and rolled 02:35Z Sep 29.** Gate result: `drop=0`
   on every `raft-obs tx->` line on all four nodes over a 9-client
   `run.sh ior` (was `drop=72114 > sent`), but terms still moved
   (group 0 +45, group 2 +105). The drops were not the trigger; the
   follower's InstallSnapshot import is — see **W22** / D5. `hi` still
   reaches 2048 on fcstor003→1 and fcstor004→2 (entry lane full → AGAIN
   to `send_ae`, no drop), which is (c)'s measurement.

   a. **Coalesce instead of drop.** An AE reply to peer P supersedes any
      older AE reply to P still queued (only the latest `match`/`term`
      matters); a heartbeat (empty `AE_REQ`) supersedes an older queued
      heartbeat to P; a vote/pre-vote message is never dropped. When the
      outbox is full, replace the superseded item in place; only if
      nothing is replaceable return `EFS_ERR_AGAIN` to the caller (as
      `send_ae` already handles for entry AEs). Gate: `drop=0` in every
      `raft-obs tx->` line over a 9-client `run.sh ior`; both groups'
      terms unchanged across the run.
   b. **Do not let one peer's backlog hide a heartbeat.** If (a) leaves
      `hi` at the cap, the sender needs the heartbeat ahead of queued
      entry AEs: dequeue heartbeats and replies before entry AEs
      (two-lane outbox), keeping entry AEs in order among themselves.
   c. **The per-link rate is a design question.** `host_sender` waits
      for `RAFT_REPLY` per frame. Letting more than the one in-flight
      batch ride a link is pipelining and is forbidden below; batching
      several reply frames into one RPC is not, but it changes the wire.
      Measure after (a)+(b): if `hi` still reaches the cap with
      `drop=0`, bring the frames/s and RTT numbers to the user.
3. **Reap dead `snap-*.kvx.tmp`.** At `efsd` start, and whenever an export
   is abandoned (`send_snap` BUSY path, leader step-down), unlink every
   `snap-<group>-*.kvx.tmp` that is not the one in progress. Gate: after
   a roll, `ls /data1/01/efs/mdraft/*.tmp` is empty on all four.
4. **Drop the six `access()` calls per PUT.** **Review after the Sep 29
   02:35Z run:** the hint as specified only helps a re-PUT of the same
   fragment; a first write has no previous PUT to carry a hint from, so
   IOR-easy and a fresh `dd` still walk all six roots (fcstor004: 1.59M
   `access()`, 192 s across six handler threads). **Step 4 (b), decided
   D7 (Sep 29):** the client sends `path_hint = 0xffffffff` when its
   dcache slot has never PUT this fragment generation (no recorded hint,
   not a STALE replay, not a retry of a failed reply); the server then
   skips `server_find_fragment_root` and creates on the writer's
   least-queue root. A fragment name is `{ci}.{fi}.{gen}`, so a first
   write of a new generation cannot collide with an existing file; the
   quota charge stays on the create path. Any re-PUT sends the recorded
   root or 0. Gate: the step-4 gate below (under 1 % on a single-client
   dd) plus `peer_shared_pwrite` / `concurrent_appends` (re-PUT paths
   still probe). Order-table row 8c. A global fd cache hung the
   9-client dd; a thread-local fd cache removed the sample and made the
   slowest client worse (both reverted, see `docs/project-history.md`
   Sep 28). Do not retry either. The PUT reply already tells the client
   which storage path took the fragment; carry that `path_index` back on
   the next PUT of the same `(ino, ci)` as a hint and probe only on a
   miss. Gate: `server_find_fragment_root` under 1% in a single-client
   8 GiB dd profile, and the 9-client dd slowest wall not worse than
   `results/measure/20260928-134637-dd-prof-r5b` (2551.5 MiB/s).
5. **`send_buf_pick` spin.** `pthread_spin_lock` inside
   `efs_rdma_send_frame` is 2.3% of the client and has a server
   counterpart. Size the send-buffer pool per QP so a pick does not
   contend, or hand each sender its own ring. Gate: the spin is gone from
   `flat.txt` on both sides.

- **Gate:** items above, plus posix 1 jobs=1 and 9-host, posix 2, and a
  1-client and 9-client 8 GiB dd with the flush in the clock, none worse
  than the Sep 28 numbers in `.cursor/rules/efs-fio-honest.mdc`.
- **Forbidden:** raising the election timeout or `HOST_TICK_US`;
  chunking InstallSnapshot differently (W11 is done); the global or
  thread-local fd cache; changing `EFS_RAFT_SNAP_CHUNK`, `HOST_PUB_BATCH_N`
  or `EFS_RAFT_AE_BYTES` (all three were measured worse on Sep 28).

#### W15 — Client: copies and busy-waits are the write CPU

**Status (Sep 29).** Step 2 is done in `bbcbcb5`
(`efs_rdma_reply_ready_us` pauses at most 16 times, then the caller
blocks on the CQ fd); on the run-3 profile the vDSO is 0.87% and the
symbol is under the 0.5% floor. Step 1's fresh profile is run 2 /
run 3 in §1b: `ll_write_buf` `memmove` 7.5%, RDMA send `memmove`
6.3% (step 3 stands), and no `memmove` caller under
`dcache_flush_slot_inner` above the 0.5% floor (the body-drop path is
taken). Run 2's client report files were overwritten by run 3 (same
`efs-mount` directory); those three numbers survive only here. The
larger client item was the reclaim walk, W18, which is in tree.
Step 3 hands a whole chunk to the dcache after one `fuse_buf_copy`
(the request buffer does not outlive the reply). Step 4 is the
same send-buffer pool as W14 step 5.

**Source.** `client.sh --perf` on fstor007 during the same `ecopy`
(`~/orcd/scratch/efs/perf/efs-mount/{flat,by_thread,callers}.txt`,
371K samples). This profile is of the binary **before** the Sep 28
dcache changes (full-chunk body drop, no snapshot copy, running
`staged_bytes`); take a fresh one first and strike whatever those already
removed.

| share | where |
| --- | --- |
| 36% | `memmove`: 15.4% in `ll_write_buf` (FUSE copy-in + dcache patch), 3.5% into the RDMA send buffer, the rest under `dcache_flush_slot_inner` (snapshot copy + `dcache_install_image` copy-back) |
| 7.5% + 5.8% | vDSO `clock_gettime` and `efs_rdma_reply_ready_us` from `efs_conn_reply_watch_us`: busy-waiting for the PUT reply |
| 7.1% | `blake3_hash_many_avx512` |
| 5.2% | `efs_export_staged_bytes` (the evictor walk; replaced by a running total Sep 28) |
| 4.6% | `xor_into` (parity) |
| 2.3% | `pthread_spin_lock` in `efs_rdma_send_frame` (`send_buf_pick`) |
| 2.2% | `__lll_lock_wait` from `dcache_put_now` |

Kernel futex + schedule under 3%: the client is not lock-bound. It is
copying and spinning. `ecopy` and `dd` on the same mount were not blocked
by each other; the `dd` was blocked on the server (W14).

**Re-profile, Sep 28 20:36–20:39 EDT (fstor007 `client.sh --perf`,
214K samples, same report dir, binary with the body-drop and
`dcache_ensure` changes).** `memmove` is still 35%. Resolved by call
address (`perf report --no-children --symbol-filter=memmove -g
caller,address` on fstor007): 14.6% under `ll_write_buf`
efs_fuse.c:2569 (the `efs_dcache_try_patch` copy bounce → dcache),
3.6% into the RDMA send buffer, and **~17% with no caller at all**.
That last share is `fuse_buf_copy` in `efs_fuse_write_buf` (libfuse
is built without frame pointers, so the chain stops at `memmove`).
Every written byte is copied kernel → libfuse buffer (`memcpy_erms`
1.6%), libfuse buffer → bounce (`fuse_buf_copy`, ~17%), bounce →
dcache (14.6%), dcache → RDMA send buffer (3.6%). Step 3's gate counts
the bounce copy too, not only the `ll_write_buf` line. `memmove` under
`dcache_flush_slot_inner` is gone (body drop landed). The vDSO share is
back at 5.7%, but from a different place: 2.5% `now_us()` in
`send_buf_pick` (inlined into `efs_rdma_send_frame`) plus 1.7%
`pthread_spin_lock` there — waiting for one of `EFS_RDMA_NSEND` = **2**
pool send buffers per QP; a third 64 KiB PUT on the same conn spins.
That is step 4 / W14 step 5, with its cause named. Two things in this
profile are not on any item and are now **W20** and **W21** below:
`ll_setattr` → `efs_fuse_getattr_ino` → `fill_stat_from_inode` is 8.9%
(a stat walks every chunk of the file), and `efs_export_staged_bytes`
is 6.6% self on the evictor thread (it was not replaced by a running
total; the per-call loop over `shard_tabs` and up to 1024 passes per
wake are still there).

Steps:

1. **Re-profile on the current binary** (one client, 8 GiB `dd bs=1M
   conv=fsync`, non-zero source, `client.sh --perf`, `stop` writes the
   reports). Record the three shares above again. If `memmove` under
   `dcache_flush_slot_inner` is still above 5%, the body-drop path is not
   being taken for the sequential write; find out why before anything
   else.
2. **Replace the reply busy-wait.** `efs_conn_reply_watch_us` +
   `efs_rdma_reply_ready_us` spin on `clock_gettime`. A PUT reply is
   behind a disk write, tens of milliseconds. Spin for at most ~50 µs,
   then block on the CQ event fd (`rc->efd`) the way `efs_rdma_recv_wait`
   already does. Gate: the two symbols together under 2% on the dd
   profile; the 1-client dd wall not worse.
3. **Cut the FUSE copy-in for whole chunks.** `ll_write_buf` copies the
   libfuse buffer into the dcache. When one write covers a whole 128 KiB
   chunk, hand the libfuse buffer to the slot as the body (the body-drop
   path then sends it and frees it). Check `fuse_buf_copy` ownership in
   libfuse 3.10.2 before assuming the buffer outlives the reply. Gate:
   `ll_write_buf` `memmove` under 5%.

   **Review of the in-tree version (Sep 28 22:00, `efs_fuse_write_buf`
   "W15.3").** It copies once only when `size == cs && offset % cs == 0`,
   i.e. a write of exactly one chunk. `max_write` is
   `EFS_WRITE_PIPELINE × cs` (≥ 1 MiB), so `dd bs=1M`, IOR `-t 1m`, and
   every `cp` deliver 1 MiB writes = eight chunks, and those all fall
   through to the bounce + `efs_dcache_try_patch` path. The 22:11
   profile of this binary shows it: `fuse_buf_copy` ~19% + `ll_write_buf`
   16%. Fixed implementation: walk the write in chunk-aligned pieces.
   For every whole chunk inside `[offset, offset+size)` allocate the
   dcache-owned buffer (`efs_buf_alloc(cs)`), set `dst` to that one
   buffer and call `fuse_buf_copy(&dst, buf, FUSE_BUF_NO_SPLICE)`;
   `fuse_bufvec` keeps the source position (`buf->idx`, `buf->off`), so
   successive calls consume the request buffer in order without a
   bounce; store each with `efs_dcache_store_full_owned`. Only a partial
   head or tail chunk goes through a bounce of its own length into
   `efs_dcache_try_patch`. Keep the O_APPEND reservation and the
   `stage_unpin` exactly where they are. If a mid-write store fails,
   fall back to the old path for the rest of the request (no partial
   success visible to the caller). Gate: on a 1-client `dd bs=1M`
   profile, `memmove` under `ll_write_buf` under 2% and total `memmove`
   with no caller under 5% (that is the single remaining copy); posix
   2 `peer_shared_pwrite` / `concurrent_appends` unchanged.
4. **`send_buf_pick` spin** — same as W14 step 5, one change for both
   sides.

   **Review (22:11 profile, `NSEND = EFS_WRITE_PIPELINE` in the binary).**
   The vDSO + `pthread_spin_lock` share did not move (6.9%). The
   callers are `now_us()` and the spin at `efs_rdma_send_frame`
   lines ~1257–1301: after `post_send` the sender **waits for its own
   send CQE** (spin 200 µs, then `sched_yield` until `send_busy[idx]`
   clears). More pool buffers cannot help; each PUT fragment still costs
   the thread one HCA round trip before it can post the next. The
   comment says the wait exists so a failed first SEND after upgrade
   surfaces as EIO instead of hanging in recv. On the 22:48 single-`dd`
   profile the vDSO share was 0.8%, so the wait costs CPU mainly when
   several writers share one QP; the gate below still applies to the
   seven-`dd` shape. Fixed implementation:
   post and return; reap send CQEs where they are already reaped
   (`send_buf_pick` on the next send, and in the reply wait —
   `efs_rdma_recv_wait` / `efs_rdma_reply_ready*` call `reap_sends`
   before blocking, and a send CQE with an error status marks
   `rc->broken` and makes the pending reply wait return `EFS_ERR_NET`).
   That preserves the "failed send becomes EIO on the reply path"
   property without a synchronous wait. Keep the 5 s
   `EFS_RDMA_SEND_WAIT_US` as the reply-wait's dead-QP bound. Gate:
   vDSO + `pthread_spin_lock` under 1% on the dd profile;
   `tests/test_rdma_xprt` and `tests/rdma_first_inode.sh` pass
   (the first-SEND-after-upgrade case is the one the wait was added
   for); 9-client dd not worse than 2551.5.
5. **The last user-space copy: let the kernel fill the dcache buffer.**

   **Checked Sep 29 15:33Z on fstor007 (`~/efs-runs/rec-splice1.log`):
   cannot take effect on this cluster as configured.** libfuse
   3.10.2's `fuse_session_receive_buf_int` uses the pipe only if it
   can grow it to `se->bufsize` = `max_write` + 4 KiB; `efs_fuse_init`
   sets `max_write` to `EFS_WRITE_PIPELINE × cs` = 4 MiB, and
   `/proc/sys/fs/pipe-max-size` is 1048576, so `F_SETPIPE_SZ` fails
   for a non-root process and libfuse falls back to the buffer path
   for good (`can_grow = 0`). **Resolved 15:45Z: the user set
   `fs.pipe-max-size=8388608` on node9901, fstor007, fcstor003–015
   (`sysctl -w`, runtime only — it reverts on reboot like
   `ptrace_scope`; ask the user to re-apply, do not edit
   `/etc/sysctl.d`).** `efs_fuse_init` now sets `FUSE_CAP_SPLICE_READ`
   when libfuse offers it. Rolled 16:09Z on the servers; clients are
   not mounted, so this is not gated. The gate
   below stands; the `fuse_copy_page` share is the number to compare.
   Lowering `max_write` under 1 MiB was the other route and was not
   taken (every `dd bs=1M` write would become two requests).

   After step 3 (verified 22:48: `ll_write_buf` `memmove` gone) every
   written byte is still copied twice: kernel → libfuse request buffer
   (`fuse_copy_page`/`memcpy_erms`, ~7% + 2.5% kernel) and libfuse
   buffer → dcache (`fuse_buf_copy`, ~22% user, caller-less in perf
   because libfuse has no frame pointers). libfuse 3 can receive a
   write's payload through a pipe instead of its buffer
   (`FUSE_CAP_SPLICE_READ`; `fuse_session_receive_buf` marks the
   payload `FUSE_BUF_IS_FD`), and `fuse_buf_copy` from an fd buffer
   into a memory buffer is one `read()` from the pipe into the
   destination — the dcache buffer step 3 already allocates. Net: one
   kernel copy per byte, none in user space. Do this as its own
   change: in `efs_fuse_init` set `conn->want |= FUSE_CAP_SPLICE_READ`
   when `conn->capable` has it; keep `FUSE_BUF_NO_SPLICE` on the
   `fuse_buf_copy` calls (it only forbids `splice()` for fd→fd, which
   we never do). Verify first, on fstor007, three things libfuse
   3.10.2 decides at runtime: that a 1 MiB write is actually delivered
   as an fd buffer (libfuse copies small requests to memory; check the
   threshold in `fuse_session_receive_buf_int`), that the pipe can hold
   `max_write` + header (`F_SETPIPE_SZ` vs `/proc/sys/fs/pipe-max-size`,
   1 MiB default — if the cap is below `max_write` + 4 KiB the request
   falls back to the buffer path and nothing changes), and that the
   `write_buf` path is the only consumer (`efs_fuse_write` with a plain
   pointer must not be reachable for fd buffers). Gate: on the 1-client
   `dd bs=1M` profile, caller-less `memmove` under 3% and total
   `memmove` under 12%; `fuse_copy_page` unchanged or lower; posix 1
   jobs=1 200/201, posix 2 63/63 (`peer_shared_pwrite`, appends,
   O_DIRECT tests); 1-client dd wall not worse than 947 RDMA.

- **Gate:** posix 1 jobs=1 (200/201, `mmap_write_read` SKIP only), posix 2
  63/63, 1-client 8 GiB dd ≥ 977 MiB/s TCP / 947 RDMA (the Sep 27–28
  numbers), 9-client dd not worse than 2551.5.
- **Forbidden:** clearing `FOPEN_DIRECT_IO`; touching `entry/attr_timeout`;
  replacing blake3 (the hash is not the wall; W3); putting the
  `sched_yield` loop back in `recv_poller` or the 200 µs clock spin back
  in `efs_rdma_recv_wait`.

#### W16 — Under a write flood, a metadata read fails after 10 s and surfaces as ENOENT

**Source (Sep 28 2026, ~18:30 EDT).** fstor007-mgmt mounted 19810 with
`client.sh --perf` while the 9×4 IOR was writing. Mount succeeded
(`fuse serving`, RDMA up). `df -h /tmp/efs-mount/` printed
`No such file or directory`. The fuse log has one line for it:
`inode-rpc: shard=0 type=47 exhausted 16 BUSY/STALE retries (10.3 s) -> EBUSY`.
Type 47 is `EFS_MSG_INODE_GETATTR`; shard 0 is the root's shard (even →
group 2). The mount was alive; `client.sh stop` unmounted cleanly. Same
class as the `INODE_LOOKUP` on shard 3745 that aborted IOR's `stat` in the
22:00Z run (§1b). Not data loss, not a dead mount, not RDMA.

Two defects, one visible and one underneath:

1. **Error mapping.** `efs_client_stat_ino` and `efs_client_stat_refresh`
   (`src/client/ops.c`) return `EFS_ERR_NOT_FOUND` for *any* RPC failure;
   `ll_getattr` (`src/client/efs_fuse.c`) maps every non-ACCES failure to
   `-ENOENT`. An overloaded cluster therefore looks like a missing
   directory to `df`, `ls`, and every path walk. The comment above the
   `no-hint` retry in `rpc_send_recv_shard` already names this.
2. **The read starves.** A GETATTR is a linearizable read:
   `host_read_index` records `commit` on arrival and waits for a read
   round and `applied ≥ read_index`; `host_wait_applied` has the same
   budget. Both give up after `HOST_READ_TRIES × HOST_TICK_US` = 400 ms
   with BUSY (`obs_wait_timeouts` counts it). The client
   (`rpc_send_recv_shard`) retries 16 times with `50 ms << min(attempt,4)`
   backoff, ~10.35 s total, then returns `EFS_ERR_BUSY`. So the observed
   line means group 2 could not satisfy a read for ten seconds straight:
   either `applied` trailed `commit` by more than 400 ms the whole time
   (36 ranks publishing; REPORT/publish applies on the pump; L1 pressure
   from this same IOR is recorded in §1b), or group 2 was re-electing
   (a hintless NOT_PRIMARY lands in the same BUSY/STALE bucket). A fresh
   client is the victim because its first op is the root GETATTR and it
   has no cached row; the IOR ranks mostly write and read their own
   dcache.

Steps, in this order:

1. **Fix the mapping first (client, small).** Carry the RPC rc out of
   `efs_client_stat_ino` / `efs_client_stat_refresh` (return it, do not
   fold to NOT_FOUND). In `ll_getattr` and the `lookup_fill` path map
   `EFS_ERR_BUSY` and `EFS_ERR_NOT_PRIMARY` to `-EBUSY` (rmdir/unlink
   already use EBUSY for `EFS_ERR_BUSY`), `EFS_ERR_NET`/`EFS_ERR_IO` to
   `-EIO`, and only a real `EFS_INODE_RPC_NOT_FOUND` to `-ENOENT`. Grep
   every caller of both functions (there are ~10 in `efs_fuse.c`) —
   some legitimately treat "cannot read parent" as ENOENT for a *child*
   lookup; those need the same split. Gate: posix 1 jobs=1 200/201,
   posix 2 63/63, and a forced 10 s BUSY (step 2's repro) makes `stat`
   return EBUSY, never ENOENT. If the user prefers EIO over EBUSY for
   stat, that is a one-line choice; ask before choosing EIO.
2. **Reproduce and attribute, then decide which server fix applies.**
   Script under `tests/measure/` (runbook in runbooks.md): start the 9×4
   IOR (`run.sh debug`), mount a 10th client on node9901 or fstor007
   (TCP or RDMA, whichever the cluster runs), and once a second run
   `stat /tmp/efs-mount/` recording errno + wall, while sampling
   `efs-mgmt raft-status` (`commit − applied`, `leader`, `term` per
   group) and grepping the leaders' `efsd.log` for
   `raft-host: read-sleep` / `apply-sleep us=` and `raft-obs`
   `obs_wait_timeouts`. Outcome A: `commit − applied` on group 2 stays
   above what 400 ms of apply covers for the whole IOR → the read is the
   messenger and the fix is apply throughput (W14 step 1 plus the cost of
   publish/REPORT apply under load; measure `apply_max` and per-cycle
   apply count first). Outcome B: term changes line up with the BUSY
   window → it is the W14 step 2 election trigger. Outcome C: lag is
   bursty (compaction/flush stalls of ≥ 400 ms) → it is L1/L0 pressure
   (§1b, W13's follow-on), not the read path. Record which; do not
   guess.
3. **Only after step 2:** if the lag is steady-state and apply throughput
   cannot close it, bring the trade-off to the user: readers currently
   wait for `applied ≥ commit-at-arrival`, which is the linearizable
   contract; a read that gives up after 400 ms and a client that gives
   up after 10 s are policy numbers the spec does not set. Do not change
   either number on your own.

**Step 2 outcome, first sample (Sep 28 22:11–22:19 EDT, no script yet).**
Seven parallel `dd bs=1M` from fstor007 into new files in one directory;
one `dd` got `open: Device or resource busy` (step 1's mapping is in
the binary, so this is the same failure that printed ENOENT before).
The fuse log for that mount holds **249** `exhausted 16 BUSY/STALE`
lines: LOOKUP 98, SETATTR 50, GETATTR 47, RENAME 31, CREATE 23. Every
`inode-rpc: retry … why=no-hint` line is a NOT_PRIMARY with no leader
hint. `raft-status`: group 0 term 9368 (was 9309 at 00:39Z), group 2
term 2789 (was 2763), both `commit == applied` when sampled;
fcstor004 `raft-obs`: `wait_timeouts=366`, `apply_max=30us`,
`apply-sleep` up to 57 ms on single entries, and the outbox
`drop=72114 > sent=50695` to the group 2 leader. **That is Outcome B:
elections, not apply lag.** The fix is W14 step 2 (a)–(c); W16 step 3's
policy question does not arise until W14 step 2 is done and this is
re-sampled with the script. Still write the script: it is the gate.

**Sep 29 02:35Z re-sample, and D8 (decided).** With W14 step 2 (a)–(b)
rolled, `drop=0` on every outbox and the terms still moved (+45 /
+105) — the trigger is the InstallSnapshot import (W22, D5). The
write-side wait now has a number: `report-split nrec=86234
pack_ms=1431 push_ms=9869 finish_ms=9227` — 337 publish batches at
~29 ms each through the one in-flight batch. **D8:** before any
read/wait policy or budget question, add `pub_batch_ms` (p50 / max
per group, the time from a batch's propose to its apply) to
`raft-obs`, run one 9-client `run.sh ior` after W22 steps 1–2 are
rolled, and bring the number: ~2–6 ms per batch means the 29 ms is
queueing behind the other eight clients' batches and the lever is
fewer, larger entries per REPORT (a wire question — ask); ~100 ms
means the `fsync` mode and D6 is the lever. Pipelining past one
in-flight batch stays forbidden. Order-table row 10.

- **Gate:** step 1's mapping gate; step 2's script committed with one
  attributed run in `results/measure/`; §1b updated with the outcome.
- **Forbidden:** raising `HOST_READ_TRIES`, `HOST_TICK_US`, or the
  16-attempt client budget to make the symptom go away; serving a
  GETATTR from the follower's or client's local state without the read
  round (that is the linearizability I2/I10 guarantee); touching
  `entry/attr_timeout`; clearing `FOPEN_DIRECT_IO`.

#### W17 — Nine writers on one file: publish STALE storm, fsync EIO, and a FUSE request that outlives its process

**Source (Sep 28 2026, 22:14–22:56Z, `bbcbcb5`).** ior-hard, 9 ranks,
one file (ino 1166063), 47008-byte records, `run.sh ior`. Evidence:

- fcstor004 `efsd.log`: a run of `raft-host: apply publish rc=-14
  index=… ino=1166063 ci=…` with distinct chunk indexes (512867,
  617329, 566269, 856797, …). `-14` is `EFS_ERR_STALE`: the CAS base
  the client reported is no longer the published generation because
  another rank published that chunk in between.
- Every client `fuse.log`: `inode-rpc: retry type=67 … why=recv rc=-6`
  then `inode-rpc: dual type=67 exhausted 16 BUSY/STALE retries
  (10.3 s) -> EBUSY`, several times per client. Type 67 is
  `EFS_MSG_REPORT_CHUNKS`. The BUSY is the server's
  `host_wait_applied` 400 ms budget inside `host_pub_batch_wait`
  (group 2 apply lagging under nine clients' publish batches), and the
  STALE is `host_pub_batch_wait` reporting any STALE verdict for the
  batch.
- fcstor013: `efs-fuse fsync: resource busy (efs_rc=-13) ino=1166063`
  → `efs_fuse_fsync_ino` returns `-EIO`. IOR: `WARNING: fsync(19)
  failed` ×8, `ERROR: close(20) failed`, rank 8 `MPI_ABORT`.
- After the abort, `io500` on fcstor007–010 and 012–014 sat in
  D-state `request_wait_answer` for 20+ min (SIGKILLed, still waiting
  on a FUSE reply). The reply they wait for is a `flush`/`release`/
  `fsync` whose `efs_client_report_dirty_ino(ino, sync=1)` is still in
  its loop: up to 64 STALE rounds, each a `stale_repull_replay` (GET +
  PUT of every moved chunk) plus a REPORT that can itself take the full
  16-retry 10.3 s, and up to 8 BUSY rounds on top. The loop is bounded
  in code and unbounded in practice.

This is the W1 N-1 CAS shape (36-way sub-chunk CAS on Sep 21 gave
hard-write 0.046 GiB/s) with nine whole ranks instead of sub-chunk
writers. The 4-rank shared-file IOR passed on Sep 27 (481.56 MiB/s,
`results/posix2/20260927-190509`); nine did not.

What to implement, in this order:

1. **A FUSE request returns.** `efs_client_report_dirty_ino` with
   `sync=1` must have a wall-clock bound that is shorter than what the
   kernel will wait, and the bound must cover the whole loop (STALE
   rounds × REPORT retries × BUSY retries), not one leg. When the bound
   is hit, `fsync` returns EIO with the dirty set merged back (the data
   is not lost; the next fsync retries). Measure first: add a counter
   line at loop exit (`report-loop ino= rounds= stale= busy= ms= rc=`)
   and run ior-hard NP=9 once to record the distribution. Then bring the
   number to the user — the 64-round and 16-retry budgets are policy
   the spec does not set. Gate: after `pkill -9 -x io500` mid-ior-hard,
   `pgrep -x io500` is empty within that bound on all nine clients
   and `efs-fuse` is still serving (`stat` OK).
2. **Attribute the STALE per chunk.** Log, on the server, how many
   publishes of one `(ino, ci)` lost the CAS within one fsync window
   (`raft-obs` counter `pub_stale` per group is enough), and on the
   client how many bytes each `stale_repull_replay` re-PUT versus how
   many bytes it owned. If a 47 KB record costs a 128 KiB GET + PUT +
   re-REPORT per losing rank, that ratio is the number to show the user.
   The `delta_off`/`delta_len` fields in `efs_chunk_rec` exist for a
   sub-chunk span publish; check whether the replay path fills them or
   always re-publishes the whole chunk, and record which.
3. **Decided (D1, Sep 28): make the span publish commute.** Three
   changes, in `efs_meta_apply_publish` and `dcache_flush_slot_inner`:
   - **Server:** for `delta_len > 0`, drop the `expected_gen != committed
     → STALE` check; a span attaches to whatever base is current. Keep
     the check for full-image publishes (`delta_len == 0`), which still
     require the live delta list to match exactly. When a fold replaces
     the base, write the folded spans' `candidate_gen`s into the new
     trailer (≤ `EFS_CHUNK_DELTA_MAX` entries); a span whose
     `candidate_gen` is in that list is a replay → OK, no-op. Overlap
     with a live span stays STALE (the client re-pulls that span's
     bytes only). A full chain still returns STALE to the publisher,
     who folds — that publisher, not the others.
   - **Client:** a slot whose dirty ranges cover less than the chunk
     publishes a span **even when `have_base` is set**
     (`span_of` regardless of `have_base`); the full-image path is for
     `dcache_full_overwrite` and for the fold. `stale_repull_replay`
     on an overlap STALE re-pulls and re-publishes the overlapping span,
     not the chunk.
   - **Reader:** `efs_meta_apply_get_chunk_deltas` and the client's
     base+spans overlay are unchanged; a reader that sees a full chain
     may fold as a background PUT, off the read.
   Gate: `test_meta_apply` gains "span after fold is OK",
   "replay of folded span is no-op", "overlapping span is STALE";
   `peer_shared_pwrite` and `concurrent_appends` pass; ior-hard NP=9
   and NP=36 SEGS=3000 cold verify bad 0 with `pub_stale` per fsync
   at 0 in steady state and the write rate rising from NP=4's 481.56
   MiB/s.

- **Gate:** step 1's bound + kill test; step 2's numbers in
  `results/measure/` with the script under `tests/measure/`; step 3's
  gate; ior-hard NP=4 SEGS=3000 cold verify still 12000 records bad 0;
  posix 1 jobs=1 200/201; posix 2 63/63.
- **Forbidden:** a distributed chunk lock; splitting REPORT into
  several RPCs; widening the 16-attempt RPC budget or `HOST_READ_TRIES`;
  making `fsync` return success on a merged-back (unpublished) dirty
  set; `hard_remove`; raising `EFS_CHUNK_DELTA_MAX` to avoid the fold;
  folding on a rank's `fsync` other than the one that filled the chain.

#### W18 — Client: the dcache reclaim is a table walk, and it is most of the client's CPU under a long write

**Source (Sep 28 2026, run 3, fcstor007 `efs-fuse`, 917K samples over
~40 min).** `by_thread.txt`: `dcache_flush_slot_inner` 43.8% **self**,
`pthread_once@GLIBC` 25.0% self, `pthread_mutex_lock` 8.3% +
`unlock` 3.5%, blake3 3.3%, `memmove` 2.4%. `callers.txt`:
`dcache_reclaim_main` is 73.5% of the process. The 5-minute profile
from run 2 (36K samples) had blake3 at 27% and this function's self
time under the floor; the long capture is the one that shows the
steady state.

Why it looks like this (`src/client/write.c`):

- `DCACHE_SLOTS` is 65536 hash buckets of chained `dcache_ent`;
  `DCACHE_RECLAIM_THREADS` is 16; each wake walks
  `DCACHE_RECLAIM_SCAN` = 64 slots via a shared cursor and, in
  `dcache_flush_slot_inner`, walks each slot's whole chain under the
  shard mutex skipping clean entries (`!e->dirty || !e->data`). With
  the staging table "over EFS_CLIENT_META_MB (256 MB) with nothing
  evictable; growing" (every client logged this), the chains are long
  and mostly clean. The self time is the chain walk.
- `dcache_mu()` and `dcache_io_mu()` call
  `pthread_once(&g_dcache_once, dcache_init)` on every call; the walk
  calls them per slot. 25% of the client is that check.
- 64 shard mutexes for 65536 slots, 16 walkers plus the FUSE write
  threads (`efs_dcache_try_patch`) on the same locks: the 12% in
  lock/unlock.

What to implement:

1. **Reclaim from a dirty list, not a table scan.** When an entry
   becomes dirty (`dcache_note_dirty_bytes(+len)` call sites), put it on
   a per-shard dirty list; reclaim pops from that list. A popped entry
   that is no longer dirty is skipped in O(1). The chain walk in
   `dcache_flush_slot_inner` stays only for `have_only=1` (close/fsync
   of one inode), which can also use a per-inode index if the profile
   still shows it. Gate: `dcache_flush_slot_inner` self under 5% and
   `dcache_reclaim_main` under 15% of a 30 s-stonewall easy-write
   profile; `dirty_bytes` accounting unchanged (posix 2 63/63;
   `concurrent_appends` and `peer_shared_pwrite` pass).
2. **Initialize the dcache once.** Call `dcache_init` from client
   startup (the export mount path) and drop the `pthread_once` from
   `dcache_mu` / `dcache_io_mu`. Gate: `pthread_once` gone from
   `flat.txt`.
3. **Re-measure W15 steps 1 and 3** on the same profile: `memmove` in
   `ll_write_buf` (7.5% in run 2) and in the RDMA send (6.3%). Those
   items stand; this one goes first because it is the larger share.

- **Gate:** the two profile gates; 9-client 8 GiB dd not worse than
  2551.5 MiB/s; 1-client dd not worse than 947 RDMA; posix 1 jobs=1
  200/201, posix 2 63/63.
- **Forbidden:** changing the 2 GiB reclaim limit or the 2× inline-help
  cap to hide the walk; dropping the `have_base` guard for reclaim
  (the `concurrent_appends` NULs); clearing `FOPEN_DIRECT_IO`.

#### W19 — Server: the Raft pump copies every AppendEntries twice, and `try_commit` re-walks the log per reply

**Source (Sep 28 2026, run 3, fcstor003 = group 0 leader, 1M
samples).** `flat.txt`: `memmove` 18.5%, kernel `rep_movs_alternative`
17.0%, `try_commit` 4.0% self, `__d_lookup_rcu` 2.6%. `callers.txt`:

| share | stack |
| --- | --- |
| 12.6% | `host_pump` → `efs_raft_recv`/`efs_raft_flush` → `send_ae` → `host_send` → `memmove` |
| 5.2% | same → `send_ae` → `memmove` (the arena fill) |
| 6.9% | `host_sender` → `efs_conn_send_msg` → `writev` → `tcp_sendmsg` → `copyin` |
| 10.5% | `host_pump` → `efs_raft_recv` → `try_commit` (4.0% self, `log_term` → `disk_get` → mutex) |

On fcstor004 the receiving side is `server_handle_conn` → `recv` →
`copyout` 15.4%. The peer connections are TCP for frames above the
72 KiB RDMA buffer (`conn_pick_send_chan`), which is every catch-up
AppendEntries batch.

What the code does (`src/raft/raft.c`, `src/server/raft_host.c`):

- `send_ae` mallocs an `EFS_RAFT_AE_BYTES` arena and `store->get`s each
  entry into it (copy 1, `disk_get`). `host_send` encodes the message
  into a stack buffer or a heap buffer (copy 2) and, when the stack
  buffer sufficed, `malloc`+`memcpy`s it again (copy 3) before queueing
  it for `host_sender`, which `writev`s it (kernel copy 4). All but the
  last run on the pump thread, under whatever the pump holds.
- `try_commit` runs on every AE reply and walks `n = last_i` down to
  `commit_index`, calling `log_term` (a `disk_get` under the store
  mutex) for each `n`, until it finds a quorum. When a follower is
  behind, `last_i − commit_index` is large and the walk repeats per
  reply.

What to implement:

1. **One user-space copy per AppendEntries.** Encode the wire frame
   directly into the buffer `host_sender` will write: `send_ae` asks
   `host_send` (or a new `net->alloc`) for a frame buffer sized from
   the entries' `clen`, fills header + entries in place, and hands
   ownership to the outbox. Drop the arena and the stack-then-heap
   re-copy. Gate: `memmove` under `host_send` + `send_ae` under 3% of
   the leader's profile during a 9-client write; `test_raft` OK.
2. **`try_commit` from the reply's match index.** The only index whose
   match changed is the replying peer's; a commit can only advance to
   an `n` in `(commit_index, match_index[from]]` whose term is current.
   Start the walk at `min(last_i, match_index[from])` and stop at the
   first quorum; cache the term of the highest entry so the common case
   is one `log_term`. Gate: `try_commit` self under 0.5%; `test_raft`
   commit/term tests OK; no change in `commit_index` sequence on a
   replayed unit log.
3. **Measure, do not change, the transport choice.** Record how many
   AE frames per second exceed 72 KiB and their size distribution
   (`raft-obs` line). `EFS_RAFT_AE_BYTES` and the RDMA buffer size are
   not to be tuned here; if the numbers say the peer path should ride
   RDMA with a different framing, that is a design question for the
   user.

- **Gate:** the two profile gates; both groups commit==applied and no
  term change during a 9-client 8 GiB dd; posix 1 9-host not worse
  than `results/posix/20260928-034049`.
- **Forbidden:** changing `EFS_RAFT_AE_BYTES`, `EFS_RAFT_AE_MAX`,
  `HOST_PUB_BATCH_N`, `HOST_TICK_US`, or the heartbeat/election
  timeouts; taking `h->mu` in `host_sender`; pipelining more than the
  one in-flight batch per behind peer.

#### W20 — Client: `stat` is O(chunks) — `st_blocks` walks every chunk of the file under two locks

**Status (Sep 29, in tree, not measured).** `efs_inode_mem` keeps
`present_chunks` (table inserts and removals) and `present_extra`
(a dirty dcache slot that is not in the table). `st_blocks` reads
that sum and probes only a partial tail. `ll_setattr` reads `size`
from the row.

**Source (Sep 28 2026, 20:36–20:39 EDT, fstor007 `client.sh --perf`,
214K samples, `~/orcd/scratch/efs/perf/efs-mount/callers.txt`).**
`ll_setattr` 8.9% of the process, all of it
`efs_fuse_getattr_ino` → `fill_stat_from_inode` → `inode_allocated_bytes`
(`src/client/efs_fuse.c`). Inside that: `efs_dcache_has` 3.7% self,
`pthread_mutex_lock`/`unlock` 2.5%, `__lll_lock_wait` 2.0% (the futex
sleeps under `fill_stat_from_inode` are 1.4% of the kernel time; the
flush path holds `idx_mu` and the dcache shard mutexes).

Why: `inode_allocated_bytes` computes `st_blocks` as "present chunks,
not holes" by looping `ci = 0 .. size/128 KiB` and, per chunk, taking
`efs_client_lock_dir` + `idx_mu` for `efs_export_get_chunk`, then the
dcache shard mutex for `efs_dcache_has`. A 16 GiB IOR file is 131072
chunks, so one `stat` is 262144 lock/unlock pairs and hash probes, and
`ll_setattr` calls `efs_fuse_getattr_ino` **twice** (before, for
`old_size`; after, for the reply). `ls -l`, `du`, `cp -p`/`ecopy`
(utimes + chmod after each file), IOR's `stat`, and every `getattr`
the kernel issues with `attr_timeout=0` pay this per file. It was ~0%
when added (project-history: small files) and is now a scaling bug by
the §1 rule: `stat` cost grows with file size.

What to implement:

1. **Make `st_blocks` O(1).** Keep a per-inode "present bytes" (or
   present-chunk count) in the staged inode row, maintained where chunks
   are inserted/removed in the client table (`efs_export_*chunk*` add /
   remove / truncate) and where the dcache admits a dirty chunk that is
   not yet in the table (`dcache_note_dirty_bytes` call sites already
   count bytes per entry). `inode_allocated_bytes` reads that counter.
   Holes stay holes: a chunk that is neither in the table nor dirty in
   the dcache is not counted, same as today.
2. **`ll_setattr` asks for `old_size` from the row, not a full
   `getattr`.** Only `FUSE_SET_ATTR_SIZE` needs the old size, and it
   needs `size`, not `st_blocks`.
3. **Gate:** a `stat` of a 16 GiB file on an idle mount under 1 ms
   (measure with `tests/measure/md_latency.py`'s stat row on a large
   file, add that row); `ll_setattr`/`fill_stat_from_inode` under 0.5%
   in the W15 re-profile; `st_blocks` unchanged for the posix suite
   (`size_and_mtime`, sparse and truncate tests) — posix 1 jobs=1
   200/201, posix 2 63/63.

- **Forbidden:** reporting `st_blocks` from `size` alone (sparse files
  and truncate stubs must still show holes); touching
  `entry/attr_timeout`; caching a stale `st_blocks` across a peer's
  REPORT (the counter is updated when the table adopts chunks, not
  time-based).

#### W21 — Client: the staging evictor recomputes the table size up to 1024 times per second and evicts nothing

**Status (Sep 29 16:09Z, steps 1 and 2 rolled with the servers,
not measured; clients are not mounted).** Step 1: a wake that evicts nothing while over the cap
sets a 1 s idle deadline that `stage_evict_kick` honours; an unpin
clears it. Step 2: `efs_export_staged_bytes` returns the root's
`staged_total`, a running sum of every table's `staged_est`;
`staged_refresh` re-derives one table's estimate wherever a capacity
or arena changes (slab and name arenas via `staged_slab_add`, the
slab directory, chunk array growth and shrink, the ino/name/chunk/icnt
index rebuilds, the child index and vectors, `compact_one_tab`, tab
attach and free). `test_stage_evict` checks the total across a
compaction. Step 3 stays behind a measurement. The earlier in-tree
state, for the record: one `efs_export_staged_bytes` per wake, a
wake walks at most four oldest bands, a fully pinned band advances a
cursor for the next wake instead of looping 1024 times.

**Re-review on the 22:48–22:55 profile (single `dd bs=1M`, 260K
samples).** `efs_export_staged_bytes` **7.4% self** and
`stage_evict_main` **5.4% self** (the inlined LRU scan in
`evict_pass`) — 12.8% of the client, more than before. The per-wake
cap works; the wake *rate* is the problem. `efs_client_stage_evict_kick`
is called from the staging paths on every write and is gated only on
`g_stage_bytes_seen > cap`. Once the table is over the cap with every
entry pinned (the "growing" state every writer reaches), that gate is
permanently true, so each 1 MiB write kicks the evictor, which wakes,
walks up to 4096 `shard_tabs` in `efs_export_staged_bytes`, scans the
LRU array up to four times, evicts nothing, goes back to
`pthread_cond_timedwait`, and is kicked again by the next write. The
1 s timer never gets to run because the kicks arrive faster.

Fixed implementation (replaces steps 1–2 above):

1. **A kick must not wake a scan that cannot evict.** Keep a
   `g_evict_idle_until` (monotonic) set to now + 1 s whenever a wake
   ends with `bytes > cap && evicted_total == 0`; `stage_evict_kick`
   returns without signalling while now < that deadline. Clear the
   deadline (set 0) where an entry becomes evictable — the unpin paths
   (`efs_client_stage_unpin`, last close, dcache clean-and-unpinned
   transitions) — so a real opportunity wakes the evictor at once.
   Gate: with one writer over cap, the evictor wakes at most once per
   second (count wakes in `EFS_STAGE_DBG` output).
2. **`efs_export_staged_bytes` in O(1).** Maintain one cross-tab
   running total in the root export, updated by `staged_slab_add` and
   the child/name-arena accounting where each tab's counters change
   (they already update per-tab totals); the shard-tab loop goes.
   `efs_fuse.c:5047` (the mount log line) reads the same total.
   Gate: `efs_export_staged_bytes` under 0.2% on any profile.
3. **The LRU scan is the remaining cost per real wake** (5.4% here
   because of the wake rate; with step 1 it is 1 scan/s). If it still
   shows above 0.5% after step 1, keep the LRU in tick order (a
   min-heap or a circular array indexed by tick) so "the 256 oldest
   above a cursor" is a slice, not a pass over `g_lru_mask + 1` slots.
   Do not do this before measuring after step 1.
4. **Gate:** as below, plus `stage_evict_main` self + children under
   0.5% on a 1-client `dd bs=1M` profile with the table over cap.

**Source (same profile as W20).** `efs_export_staged_bytes` 6.6%
**self**, caller `stage_evict_main` (`src/client/stage_evict.c`).
`stage_evict_main` itself 2.4%. W15's table said this was "replaced by a
running total Sep 28"; it was not. `export_staged_bytes_one` reads
running totals for one tab, but `efs_export_staged_bytes` still loops
`s = 1 .. shard_tab_cap` over `shard_tabs` (up to 4096) per call, and
`evict_pass` calls it once per pass.

Why the call count: when `bytes > cap` and the SCAN_BATCH (256) oldest
LRU entries are all pinned, `evict_pass` returns `evicted=0,
band_full=1`; `stage_evict_main` sets `min_tick = band_max` and loops,
up to 1024 rounds per wake, each round recomputing staged bytes and
scanning the **whole** LRU array (`for i in 0..g_lru_mask`) to find the
next 256 candidates. Every client logged "staging table over
EFS_CLIENT_META_MB (256 MB) with nothing evictable; growing" during the
IOR, so this is the steady state under a write flood: O(rounds × (tabs
+ LRU)) per second, no eviction, on a core the writers wanted.

What to implement:

1. **One `staged_bytes` per wake.** Compute it once in
   `stage_evict_main` before the round loop and adjust by what each
   round actually freed (`evict_pass` knows the bytes it dropped), or
   keep a true running total across tabs (`staged_slab_add` already
   exists per tab; add the cross-tab sum there). Gate:
   `efs_export_staged_bytes` under 0.5% in the W15 re-profile.
2. **Stop the pinned-band loop from rescanning the LRU.** Walk the LRU
   once per wake in tick order (or keep it ordered) so advancing past a
   pinned band is a cursor move, not a fresh O(N) scan. If the whole
   table is pinned, log the "growing" line once (already done) and
   sleep until the next kick or 1 s — do not spend 1024 rounds proving
   it. Gate: `stage_evict_main` + children under 1% while a client is
   over cap with all entries pinned (the IOR steady state).
3. **Gate:** W9's RSS result still holds (cold stat of 1M files levels
   near 233 MB, `results/measure/20260927-w9-walk`); posix 1 jobs=1
   200/201; posix 2 63/63.

- **Forbidden:** raising `EFS_CLIENT_META_MB` to hide it; evicting
  pinned (dirty or open) inodes; changing the 1 s wake period to
  something longer so the walk is merely rarer.

#### W23 — Client: five liveness syscalls per connection checkout, plus an `fstat` per send

**Source (Sep 29 2026 00:18–00:22 EDT, `client.sh --perf --strace` on
fstor007, `~/orcd/scratch/efs/perf/efs-mount/strace-summary.txt`,
230 s window, `ecopy --verify` + one `dd`).** Syscall counts:
`fstat` 1 171 561, `getsockopt` 1 172 007 (586 035 `SO_ERROR`,
585 972 `TCP_INFO`), `recvfrom` 598 781 (all `MSG_PEEK`), `poll`
1 121 932, `read` 1 195 618 (297K on the FUSE fd, the rest eventfds and
sockets), `write` 583 579 (`recv_poller` eventfd wakes). In the
untraced 22:11 profile the same path was `__fstat64` 1.2% +
`__getsockopt` 1.2% + `efs_client_conn_get` 0.6%; under ptrace it is
`efs_client_conn_get` 5% and `raft_voter_conn` 8%.

Why: `efs_client_conn_get` (`src/client/node_cache.c`) calls
`conn_fd_is_dead` on **every** checkout: `efs_conn_fd_matches` (an
`fstat`), `getsockopt(TCP_INFO)`, `poll(0)`, `recv(MSG_PEEK)`,
`getsockopt(SO_ERROR)`. Then `efs_conn_send_msg_parts`
(`src/common/protocol.c`) does `efs_conn_fd_matches` again before the
RDMA send. Six syscalls per checkout, three checkouts per chunk PUT,
one per metadata RPC — ~586K checkouts in this window, ~15K
syscalls/s. The probe exists for a real reason (the comment: a pooled
fd in CLOSE-WAIT after a peer FIN looked like a live checkout and every
PUT then failed; IPoIB CLOSE-WAIT sometimes reports no POLLIN/HUP), and
`fd_matches` exists because a recycled fd number once sent into a dead
QP. Neither reason needs a probe per checkout.

What to implement:

1. **Pool generation instead of `fstat`.** The pool is the only closer
   of pool fds. Give each slot a `gen` that `efs_client_conn_drop` /
   destroy bumps, store it in `struct efs_conn` at checkout, and make
   `efs_conn_fd_matches` compare integers. Keep the `fstat` form only
   for conns not owned by the pool (server side, tests) behind the
   existing `fd_id_ok` flag. Gate: `fstat` count in an
   `EFS_STRACE_EXPR='trace=fstat'` summary under 1% of the PUT count.
2. **Probe once per idle period.** Record `last_ok_ms` on a conn at
   every successful send/recv. In `efs_client_conn_get`, run
   `conn_fd_is_dead` only when `now - last_ok_ms > 1000` (the CLOSE-WAIT
   case is a conn that sat idle while the peer went away; a conn that
   completed an RPC a few milliseconds ago is not in CLOSE-WAIT). A
   checkout that skipped the probe and then fails in send/recv already
   drops the conn and marks the node (`efs_client_conn_drop`,
   `note_fail`); that path stays. Gate: `getsockopt` + `recvfrom(MSG_PEEK)`
   under 1% of the PUT count on the same summary; the Sep 20 CLOSE-WAIT
   repro (bounce one `efsd` while a client is idle for 30 s, then write)
   still recovers without an EIO.
3. **Gate:** posix 1 jobs=1 200/201, posix 2 63/63, 9-client dd not
   worse than 2551.5; `efs_client_conn_get` + `__fstat64` +
   `__getsockopt` together under 0.5% of an untraced dd profile.

- **Forbidden:** removing the dead-conn detection (the probe's reasons
  are real); probing on a timer thread (it would need the pool lock the
  hot path uses); changing `EFS_NODE_DOWN_FAILS` / `EFS_NODE_DOWN_MS`.

#### W24 — Client: `open(O_TRUNC)` of a large existing file did not return; SETATTR is the most-exhausted RPC

**Source (same window).** `dd.trace.txt`:
`openat(AT_FDCWD, "/tmp/efs-mount/001/dat08", O_WRONLY|O_CREAT|O_TRUNC, 0666) = ?`
followed by `+++ killed by SIGKILL +++` — the create-with-truncate of
the 1.7 GB file the previous `dd` had left never returned before the
user killed it. The fuse log's `exhausted 16 BUSY/STALE` count by type
went from 50 → **136** for SETATTR (type 63) between 22:19 and 00:27,
the largest of any type (RENAME 106, LOOKUP 98, GETATTR 47, CREATE
37). `clock_nanosleep`: 564 sleeps of 0.8 s in 230 s — the tail of
the BUSY backoff — plus `report-loop … busy=1 ms=8435..24182 rc=-13`
lines.

Why an `open` can outlive every budget in the tree:

- Kernel `open(O_TRUNC)` on an existing inode becomes FUSE `setattr(size)`
  → `efs_fuse_truncate_ino` (`src/client/efs_fuse.c`) →
  `efs_dcache_flush_ino` (flush every dirty chunk of the ino; each PUT
  waits on the writer pool), then `efs_client_truncate`
  (`src/client/ops.c`) → `efs_client_report_dirty_ino` (a REPORT loop
  — `report-loop … ms=24182` is one of those), then the SETATTR RPC,
  then `ll_setattr`'s getattr. Three to four RPC chains in sequence.
- Each chain is 16 attempts, but an attempt is bounded by the socket
  `EFS_IO_TIMEOUT_MS` = **30 s**, not by the 400 ms server read
  deadline: when the server holds the RPC (a REPORT was measured
  holding 20.5 s in W22's source; `host_truncate` in `raft_host.c`
  does a `host_read_index`, then proposes a LANE_FENCE per cross-group
  lane, then the truncate entry, each a commit wait), one chain can be
  minutes. The 10.3 s figure in the log line is the sum of the sleeps,
  not the wall.
- With W16 step 1 the final answer is EBUSY; the caller still waits
  through all of it first. The seven D-state `io500` processes in W17
  are the same shape seen from the other end.

What to implement:

1. **Measure one truncate, idle and loaded.** `truncate -s 0` of a
   2 GiB file on an idle cluster and under a 9-client write, with
   `strace -f -tt -T` on the fuse daemon filtered to that worker, and
   the `inode-rpc: slow-recv` / `report-loop` lines. Record per-RPC wall
   (`REPORT`, `SETATTR`, `GETATTR`) and the server-side
   `raft-host: setattr … rc=` pair. This says whether the time is the
   REPORT loop, the SETATTR's commit chain, or the retry sleeps.
   `results/measure/<stamp>-truncate-wall/`.
2. **Bound the whole operation, not each RPC.** `efs_fuse_truncate_ino`
   gets one deadline (the same 8 s W17 step 1 gave `fsync`, measured
   from entry); `efs_dcache_flush_ino`, `efs_client_report_dirty_ino`,
   and the SETATTR RPC each receive the remaining budget and return
   `EFS_ERR_BUSY` when it is gone. `fuse_stat_errno` maps that to EBUSY
   (already). Do not retry a SETATTR whose reply was lost: the server's
   op-id window (I16) answers the replay. Gate: under a 9-client write,
   `truncate -s 0` of a 2 GiB file returns (EBUSY allowed) within 10 s;
   idle it completes in under 1 s; posix 1 `truncate*` and
   `peer_o_trunc_visible` unchanged.
3. **`host_truncate` should not need a read round for the size path.**
   The `host_read_index` at the top is what pays the 400 ms BUSY under
   election churn before the proposal even starts; the proposal itself
   is the linearization point. Check whether the pre-read is only
   fetching `row` for lane splitting; if so, read the row from the
   local KV without a read round and let the truncate entry's apply
   re-validate (it already CASes the tail). This is a server change; if
   the pre-read is load-bearing for a fence ordering, stop and say so.

- **Gate:** step 1's measurement committed; step 2's bounds; posix 1
  jobs=1 200/201 (the truncate tests), posix 2 63/63,
  `posix_persist` shrink/remount cases once they exist.
- **Forbidden:** lowering `EFS_IO_TIMEOUT_MS` to make the chain shorter
  (a slow server reply is not a dead peer); returning success for a
  truncate whose SETATTR did not commit; touching the 16-attempt budget
  (W16 forbids it).

#### W25 — `futimens` fails with EINVAL on any file with a lane in the other Raft group; atime loses its nanoseconds; `ftruncate` says EAGAIN where `stat` says EBUSY

**Source (Sep 29 2026 00:1x EDT, `ecopy --verify /data1/erbmi1/knouse/
→ /tmp/efs-mount/knouse/` and the `software/` copy, operator's
terminal).** Three error strings, thousands of times:

- `<path>: futimens: Invalid argument` — only on large files (R and boost
  tarballs, cuDNN `.a`/`.so`, a MATLAB tar).
- `ecopy: verification metadata mismatch: <path> (atime)` — only on small
  files (conda-meta `.json`, headers).
- `ftruncate: Resource temporarily unavailable` — during the same window
  in which the fuse log's SETATTR `exhausted` count went 50 → 136.

**Confirmed from `ecopy.strace.txt` (00:35 EDT, `strace -f`, 585
threads, 255 931 lines, the `software/` copy, ended by Ctrl-C).**
Non-ENOENT failures: `utimensat` EINVAL **40** (516 succeeded),
`ftruncate` EAGAIN **19** (43 succeeded), `fallocate` EOPNOTSUPP 62,
`copy_file_range` EXDEV 423; `pwrite64` 29 905 and `pread64` 4 635 with
**no short or failed call**, no `close` errors, no `fsync` at all
(ecopy does not call it). Every EINVAL `utimensat` is on an fd that had
just had `fallocate` → `ftruncate` to tens or hundreds of MB (e.g. fd 74:
`ftruncate(74, 104082560)` then `utimensat(74, …) = EINVAL`); every
successful `utimensat` is on a file that went through the small-file
`copy_file_range` fallback. That is the lane split below, seen from
userspace. **Consequence:** after `futimens: Invalid argument` and after
`ftruncate: Resource temporarily unavailable`, ecopy `close`s and
**`unlinkat`s the `.ecopy.tmp.*` file** (56 unlinks in the trace): each
of those files was *not copied*. On this tree that is every file larger
than a few MB. The `ftruncate` that failed for `fd 47`
(923 946 888 bytes) was issued at line 6 123 and returned at line
137 107 of the trace — it blocked for most of the run before failing
(W24's chain; no `-tt` in this trace, so no wall figure). The 303
`verification metadata mismatch` lines are all `(atime)` and all on
files whose `utimensat` succeeded, i.e. item 2. `fallocate`
EOPNOTSUPP and `copy_file_range` EXDEV are the kernel's answers for a
FUSE mount without those ops and a cross-device copy; ecopy falls back
correctly and they cost one syscall each — not defects, noted so nobody
chases them. **`posix_fallocate` returning `EINVAL` is the same gap,
seen through glibc (Oct 1).** `efs_ll_ops` has no `fallocate`; libfuse
replies `ENOSYS` and the kernel turns that into `EOPNOTSUPP` for the
`fallocate` syscall. glibc's `posix_fallocate` does not return that: on
`EOPNOTSUPP` it writes zeros itself, and it does **not** emulate
`EINVAL`. On an `O_DIRECT` fd the fallback `pwrite` is not 4 KiB-aligned,
so it returns `EINVAL` (`fuse_odirect_unaligned` does the same for an
application `O_DIRECT` write). A normal fd's `posix_fallocate(0, 4096)`
succeeds only because of that zero-fill — `opt_fallocate` PASS is not
the filesystem reserving blocks. There is no preallocation. A tool that
treats the `EINVAL` as fatal writes nothing. **Implement the handler:
W26.**

Each is a code path, read from the tree, not from a profile:

1. **EINVAL on `futimens` = cross-group lanes.** `host_utimens`
   (`src/server/raft_host.c` ~9100): when the mask has MTIME it walks
   `row.active_lanes` and, for every lane whose shard is in the other
   Raft group, sets `rc = EFS_ERR_INVAL` with the comment
   `/* cross-group lane fence later */`. Lane shards straddle both
   groups (every odd lane of an inode is in the other group, project
   state), so any file that has written into two or more lanes — any
   file past the first few chunks — cannot have its mtime set.
   `efs_rc_to_errno` turns that into EINVAL. Small files (one lane, or
   all lanes in the row's group) work, which is exactly the split in the
   ecopy output. `host_truncate` already handles the same case (it
   proposes a LANE_FENCE per cross-group lane, then the entry);
   `host_utimens` was left as a stub.
2. **atime mismatch = seconds only.** `efs_fuse_utimens_ino`
   (`src/client/efs_fuse.c` 3228) keeps `tv[0].tv_sec` and drops
   `tv[0].tv_nsec`; `efs_client_set_atime` / `efs_client_utimens_both`
   carry `atime` as seconds; `host_utimens` stores
   `u.atime = atime * 1000000000`. mtime keeps its nanoseconds
   (`msec`, `nsec`), atime does not. ecopy compares full timespecs, so
   every file whose `futimens` *succeeded* then fails the atime check.
   Reads do not touch atime anywhere in the tree, so this is the whole
   cause.
3. **`ftruncate` EAGAIN = the SETATTR BUSY budget (W24), mapped
   differently.** `efs_fuse_truncate_ino` returns
   `efs_rc_to_errno(EFS_ERR_BUSY)` = **EAGAIN**; W16 step 1's
   `fuse_stat_errno` maps the same BUSY to **EBUSY** for
   getattr/lookup/create. Two errnos for one condition; ecopy treats
   EAGAIN as a failure and moves on. The cause is W24/W22; the mapping
   is this item.

What to implement:

1. **`host_utimens` fences cross-group lanes the way `host_truncate`
   does** (propose the LANE_FENCE entries on the other group first, then
   the UTIMENS entry; the same helper). If the fence is unnecessary for
   a pure mtime set — the fence exists so an in-flight REPORT on another
   lane cannot re-stamp "now" over the set time — then the UTIMENS entry
   needs the `mtime_gen` bump the apply already checks, per lane group;
   decide by reading `efs_meta_apply` UTIMENS/REPORT ordering, and say
   which. Gate: `futimens` on a 2 GiB file returns 0 and `stat` shows
   the set mtime; posix 1 `utimens*` and posix 2 `peer_utimens_visible`
   unchanged; `cp -p` / `ecopy` of a 1 GiB file reports no `futimens`
   error.
2. **Carry `atime_nsec` end to end**: `efs_fuse_utimens_ino` →
   `efs_client_set_atime` / `_utimens_both` → `efs_msg_inode_setattr`
   (add the field; wire change, bump nothing else) → `host_utimens`
   `u.atime = atime*1e9 + atime_nsec` → `efs_meta_setattr` → row. Gate:
   `touch -a -d '@1500000000.123456789'` then `stat` shows
   `.123456789`; ecopy's atime check passes on the small-file tree.
3. **One mapping for BUSY.** `efs_rc_to_errno` returns `-EBUSY` for
   `EFS_ERR_BUSY` (keep EAGAIN for `EFS_ERR_AGAIN` only), so truncate,
   utimens, chmod, and the stat path agree. Gate: posix 1 jobs=1
   200/201; grep the suite for tests that assert EAGAIN on BUSY (there
   should be none; if one exists, that test is wrong about the
   contract, not this change).

- **Gate:** the three above plus posix 1 9-host not worse than the
  last run.
- **Forbidden:** setting mtime without fencing or bumping `mtime_gen`
  (a later REPORT would restore "now" — the exact bug
  `setattr_rpc_dual_apply`'s comment describes); rounding atime on the
  server to hide step 2; mapping BUSY to EIO.

#### W26 — FUSE `fallocate` handler — ASKED Oct 1 2026

`posix_fallocate` on an `O_DIRECT` file returns `EINVAL` because
`efs_ll_ops` has no `fallocate` (W25). The user asked for the handler.
`/home/erbmi1/git/direct_rw/ewrite.c` opens `O_DIRECT`, calls
`posix_fallocate(fd, 0, size)`, and falls back to `ftruncate` only for
`EOPNOTSUPP`/`ENOSYS`. `EINVAL` aborts the job with 0 bytes written.

**What.** Add `ll_fallocate`. Mode 0 (what `posix_fallocate` sends)
extends the file to `offset + length` the way `ftruncate` already does:
one size SETATTR, no fragments. A read of the new range is a hole and
returns zeros (a missing chunk already does). `FALLOC_FL_KEEP_SIZE`
leaves the size unchanged and succeeds when the range is inside the
file. Any other mode (`PUNCH_HOLE`, `ZERO_RANGE`, `COLLAPSE_RANGE`,
`INSERT_RANGE`, `UNSHARE_RANGE`) returns `EOPNOTSUPP`, so a caller can
fall back. This does not reserve NVMe blocks; a later write can still
fail if a disk or the quota is full.

- **Gate:** `fallocate(fd, 0, 0, size)` on an `O_DIRECT` fd returns 0;
  `stat` size is `size`; a read of the new range is zeros; no chunk
  object was stored for that range. `ewrite` from `direct_rw` gets past
  `prepare_file_space` and writes. posix jobs=1 stays **200/201**
  (`opt_fallocate` still PASS; it must no longer depend on glibc
  writing the 4 KiB). posix2 63/63.
- **Forbidden:** writing zero fragments to satisfy the call; a quota
  reservation; implementing punch-hole or zero-range in this item;
  treating glibc's `posix_fallocate` success on a normal fd as proof
  (that success is the zero-fill fallback until this handler exists).

#### W12 — Repo hygiene

`results/` holds only runs that a live document cites (this page, the
rules, `docs/`, `tests/`; `project-history.md` and `design-history.md` are
archives and do not count). Commit a run directory when it is cited as a
gate; otherwise delete it. Pruned to that rule on Oct 1 2026; `git log --
results/` keeps the rest.

Run `python3 docs/gen-architecture-full.py` (or `make docs-check`) after any doc edit: it regenerates
`architecture-full.md` **and** `architecture.html` from the markdown sources
and validates links plus every `I1..I25` reference. Never edit either
generated file. `make test` must be fully green — there is no
accepted-failure list.

---

**Idea checked Oct 1 2026 — synchronous encryption. Not a decision. Do not implement.**

The user asked how transport and at-rest encryption could work, and whether a key created with the cluster would let only the Linux user who mounts with that key see the contents. Conclusions, for a later design pass:

- Nothing encrypts today. `EFS_MSG_HELLO` checks the build id. Any host that can reach 19810 is a peer. Modes are enforced in the FUSE process; the client tells the server its uid (`docs/product-gaps.md`).
- One key, created at `raft-mkfs` and shown once. The cluster stores a verifier, not the key. A mount proves it knows the key. The key file is mode `0600` and the mount is not `allow_other`, so other uids on that machine see nothing at the mount point. Root on that machine can still read the key (ptrace is open here). Any other host that has the key can mount. Every holder of the key sees every byte it encrypts. Per-user secrecy is a different key per user.
- Fragments, on the client, after parity. Split the chunk, XOR the parity, then encrypt each of the three fragments with its own IV stored beside the fragment. `PUT` and the disk see only ciphertext. A read decrypts, then XORs if it must rebuild one fragment. XOR of ciphertext does not reconstruct, so encryption sits outside the parity. The object name stays a hash of the plaintext, so two clients still agree on one object; the IV is not part of that name. The server stores opaque fragments and does not need the key to hold them.
- Metadata is the open choice. Names and directory updates are applied by the servers. Without the key they cannot apply encrypted names, so either the namespace stays plaintext on disk (a pulled NVMe reveals names, not file bytes) or `efsd` is unlocked with the key at start and the KV and Raft log are encrypted on the way to disk. The key must not live in the Raft log, or the disk contains the key that decrypts the disk. A server that was not unlocked cannot serve.
- Transport is the same idea on the frame: encrypt before send, decrypt before the handler, on the RDMA path and on the TCP side channel large frames already use. A traffic key derived from the cluster key at connect. Ciphertext fragments on the wire still show sizes and which object was touched; the frame hides names and the access pattern.
- "Synchronous" means the `pwrite` and the wire only ever see ciphertext. There is no later encryption pass.

**Decisions only the user can make — bring the evidence, do not start.**
These came out of reviewing efs as an HPC parallel filesystem. Each one
changes what efs *is*, so an agent must not pick a side; but each has a
cheap measurement an agent can produce first, named here:

- **Per-file layout control.** Chunk size and the EC profile are per export;
  there is no per-file or per-directory equivalent of Lustre's `lfs
  setstripe`. A 4 KiB-record checkpoint and a 1 GiB-per-rank dump therefore
  get the same 128 KiB geometry, and the declared 32× small-write
  amplification ([architecture.md §9](../architecture.md)) has no opt-out.
  Evidence to bring: W6's IOR-hard/IOR-easy ratio and rw-4k from W4.
- **An interface beyond FUSE.** FUSE is the only client. libfuse 3.10.2 cannot
  negotiate `FUSE_MAX_PAGES`, so every request is ≤128 KiB regardless of
  `max_write`; it cannot emit `FOPEN_PARALLEL_DIRECT_WRITES`; and Linux takes
  the inode lock exclusively for extending direct writes and the parent
  directory lock for `O_CREAT`, **per mount** (already in
  [architecture.md §9](../architecture.md) as an open kernel-interface item).
  So 64 ranks on one node writing one file serialize in the kernel before efs
  is called. Every production PFS has a kernel client, a user-space library,
  or an MPI-IO ADIO driver. Evidence to bring: W4's per-client scaling and a
  1-node 8-rank IOR-easy vs 8-node 1-rank IOR-easy comparison from W6.
- **Whether `write()` is durable** — W2 closed: spec says `fsync`/`close`/
  `O_SYNC`. Do not reopen as publish-on-write. `O_SYNC` wiring is a later
  item, not a silent side-cut.
- Already listed before this review: C1 relaxed coherence; a pressure-triggered
  directory-spread bound (unspecified); cutover of a 36T `efs-test`; any new
  REPORT or SNAP wire shape.

**Bigger than this queue.** [product-gaps.md](../product-gaps.md) inventories
what is missing before efs is a filesystem anyone could run — including the
things that contradict a guarantee the spec already makes (no fragment
repair, no protection-debt tracking, no session/fencing on the client, and
W1/W2 above). Those are not queue items beyond what W1–W2 say; each needs a
design decision first. Do not start one without asking, and do not treat the
queue above as the whole distance to a product.

#### W22 — Server: the snapshot cadence makes InstallSnapshot the steady state, and a follower inside an import campaigns

**Status (Sep 29 2026, 04:27Z).** Steps 1 and 2 are in tree and rolled
(04:08Z, `54a500da9dc8-dirty`): a group snapshots at 512 MiB of log
commands past `snap_idx` (`EFS_RAFT_SNAP_BYTES`), `raft_group_snap`
keeps that window (`log_base`, recorded in the SNAP record so a rotated
log replays it), `send_ae` InstallSnapshots only below
`efs_raft_log_floor`; the import diff is applied 1024 keys per pump
cycle (`HOST_SNAP_SLICE`, state `SNAP_IMP_APPLY`) and the last-chunk
retry stays BUSY until the cursor finishes. First 20 minutes on the
cluster, with a 7-minute write in them: zero `raft-snap: start` on any
node, no import. The byte counter's first version walked the log on
every tick (1–1.25 % of every server); the O(1) version is in tree, not
rolled. Step 3's flag `--meta-storage` is in tree with the default
layout; its measurement is **closed by D11** — the per-thread `fsync`
histograms from that trace put the Raft log at 0.4 ms and the 100 ms
mode on the compactor's own segments. What the trace found instead is
W23. Order-table rows 8a, 8b done; 8d closed.

**Source (Sep 29 2026 02:35–02:41Z, `results/io500/20260929-023447-iorperf2/ana`,
9-client `run.sh ior`, perf + `strace -f -tt -T` on every daemon).**

- `host_maybe_snapshot` (`raft_host.c`, `HOST_SNAP_MIN` = 256) starts an
  export as soon as 256 entries have applied since the last one and the
  previous export has finished. The export is the whole KV: 2.67 GB,
  4.6–12 s each, so every node exports back to back for the whole run
  (`raft-snap: start/end` pairs 256 entries apart; fcstor005 twelve of
  them in six minutes). `efs_kv_lsm_view_export` `memcmp` +
  `vx_sift_down` is the top user symbol on all four servers (14 % of
  fcstor004's samples on the GC thread).
- `send_ae` (`raft.c:672`) sends InstallSnapshot to a peer whose
  `next_index ≤ snap_idx`; with the snap point moving every 256
  entries, a follower 257 entries behind gets the 2.67 GB file instead
  of 257 log entries. fcstor006 imported group 2 twice (`import diff
  n=685534 ms=12132`, `n=130398 ms=16467`), fcstor003 imported group 0
  once (`n=380716 ms=12041`).
- The import diff is applied on the follower's pump on the ack (W14
  step 1 moved the scan and sort off the pump; the apply stayed). For
  those 12–16 s the follower answers no heartbeat. Group 0 term moved
  9370 → 9415 and group 2 2792 → 2897 across the run with `drop=0` on
  every `raft-obs tx->` line on all four nodes — the outbox coalesce
  (W14.2 (a)–(b)) removed the drops and the terms still moved. The
  group-2 leader answered 15 REPORTs NOT_PRIMARY and fcstor005 answered
  32 as a transient leader.
- fcstor004's busiest thread (22 % of samples) is the compactor:
  sequential 8050-byte `pread` of one segment, 4 KiB `write`s of the
  new one, and a segment `fsync` at a flat 100 ms, 97 times.
  `persist_max` reached 254 ms. The twenty writer-pool threads create
  fragments on the same XFS (`/data1/01`, ~14 s of `openat`/`write`/
  `close` each). The Raft log, the KV WAL, the segments, and the data
  share one journal.
- Net effect on a client: one REPORT of 86 234 records held the RPC for
  20.5 s (`pack_ms=1431 push_ms=9869 finish_ms=9227 rc=-13`), which is
  the client's `recvfrom` of 19.36 s and the `rounds=1 rc=-13` fsync EIO
  that aborted IOR. The pump itself was not held: `pump_hold_max` 59 ms,
  `apply_max` 69 µs on the leader in this run.

**Steps, each its own change with its own gate (D4–D6 decided):**

1. **Snapshot by log bytes; keep a log window (D4).** In
   `host_maybe_snapshot`: replace `applied ≥ snap + HOST_SNAP_MIN` with
   "bytes appended to this group's log since `snap_idx` ≥ 512 MiB"
   (the store knows its size; count entry `clen` at append if it does
   not). In the store's `save_snap`: keep the log entries after the
   snapshot point instead of truncating at it — retain a window of the
   last `W` entries (start with the same 512 MiB worth; the window is
   internal, no knob) and drop only what falls out of it. In `send_ae`
   (`raft.c:672`): send InstallSnapshot only when `next_index` is below
   the retained window's first index, not when it is `≤ snap_idx`.
   `snap_ensure` / `send_snap` keep re-exporting on demand for a peer
   that is genuinely below the window (fresh node, wiped `mdraft/`).
   Gate: over a 9-client `run.sh ior`, `raft-snap: start` per group at
   most once per 512 MiB of log; `import start` count 0 on every node
   unless one was restarted; `efs_kv_lsm_view_export` under 2 % of
   every server's samples; `test_raft` OK; fcstor005 bounce-and-rejoin
   still catches up (from the log now, not a file).
2. **Slice the import-diff apply (D5).** Today the pump applies the whole
   diff batch on the ack. Make the import state machine resumable: the
   pump applies at most one `HOST_TICK_US` of diff keys per cycle
   (measure keys/µs once; a fixed count per cycle is fine), releases,
   drains the inbox and answers AppendEntries as a follower, and
   continues on the next cycle; the SNAP_REP that acks the last chunk
   goes out when the final slice is in (until then the retry of that
   chunk keeps getting BUSY, as it does now). Gate: force an
   InstallSnapshot under load (bounce one voter after wiping its
   `mdraft/`, with a 9-client write running); `apply_max` under 70 ms
   throughout, no term change on the importing group during the
   import, `tests/measure/i17_leader_freeze.sh` still passes; the
   follower's `commit == applied` catches up to the leader after the
   import.
3. **Metadata root off the fragment root (D6).** Measure first, on one
   server, with the cluster serving a 9-client write: `dd if=/dev/urandom
   of=<root>/fsync-probe bs=4k count=1 conv=fsync` 200 times on
   `/data1/01` (shared) and on a root with no writers (stop one path
   from `--storage` on a scratch efsd, or use a directory on `/` for the
   comparison only), p50/p99 of each into
   `results/measure/<stamp>-fsync-root/`. If the shared root shows the
   100 ms mode and the quiet one does not: add `--meta-storage <root>`
   to `efsd` (default the first `--storage` root — today's layout, no
   migration of existing `mdraft/`), then redeploy 19810 with `mdraft/`
   on a root none of the six `--storage` paths use (ask which device;
   this is a cluster layout change). Gate: compactor and pump `fsync`
   p99 under 10 ms in an `EFS_STRACE_EXPR='trace=fsync,fdatasync'`
   trace of a 9-client write; `persist_max` under 20 ms. If the quiet
   root shows the same 100 ms mode, this step is closed as "not the
   sharing" and the number goes to the user.

- **Gate:** the three above; posix 1 jobs=1 200/201 and 9-host, posix 2
  63/63; a 9-client `run.sh ior` that completes ior-easy-write with zero
  `report-loop … rc=-13` lines; 9-client 8 GiB dd not worse than 2551.5.
- **Forbidden:** raising the election timeout or `HOST_TICK_US`; widening
  the 400 ms apply budget or the 16-attempt RPC budget to hide the wait;
  putting the import scan/sort back on the pump (W14 step 1); changing
  `EFS_RAFT_SNAP_CHUNK`; a global or thread-local fd cache (W14 step 4).

**Two measurement notes from this run.** Both perf profiles carry the
concurrent strace (`ptrace_do_notify` 4–5 % on every host; ~40 % of the
client's `children` view is ptrace stops), so shares are inflated toward
syscall-heavy threads — profile with one recorder at a time when a share
matters. And an un-narrowed server `--strace` is ~2 GB per 6 minutes;
use `EFS_STRACE_EXPR`.

#### W23 — Server: the apply path blocks on L0 back-pressure, and compaction rewrites the table to absorb a few MiB

**Status (Sep 29 2026, 05:11Z).** In tree and rolled
(`a53b253f2455-dirty`, RDMA, `--perf --strace`). Step 1: the apply path
does not wait in `kv_maybe_flush_locked`; a publish batch returns BUSY
from `host_pub_batch_propose` while L0 is within one flush of the cap.
Step 2: a range is compacted when its L0 bytes are at least 1/8 of its
L1 bytes (or when L0 is at the file cap, whichever comes first);
`key[0]==0` is 16 subranges (`key[1]`'s top nibble) at flush and at
compaction output; compaction iterators read 1 MiB ahead. Not gated.
The 1 GiB L0 byte cap in place of the 64-file array is not in this
change — the file cap still forces a compaction, but of one subrange.

**Source (Sep 29 2026 04:07–04:27Z, `results/measure/20260929-040800-idle-trace/ana`,
perf + `strace -f -tt -T` on every daemon, a user `ecopy` 04:15–04:22Z
writing 408K fragments per server; §1b has the full list).**

- `kv_maybe_flush_locked` (`kv_compact.c:711`) waits on `l->cv` while
  `n_l0 + KV_LSM_RANGE_MAX > KV_LSM_MAX_SEGS` (i.e. L0 ≥ 48 files). The
  caller is the apply path on the pump under `h->mu`. On fcstor004 the
  pump's `futex` waits were 4.14, 1.68, **24.23**, 2.70, 3.57 s, each the
  length of one `kv-compact: end ms=`; `pump_hold_max` 4.4 s (history
  6.4 s), `apply_max` 4.4 s. fcstor005 2.14 s. W13 step 2 documented
  this wait as "the only stall left" and logged it (`kv-compact:
  backpressure n_l0=`); the log shows it continuously through the write.
- While the pump is held: `host_wait_applied` hits its 400 ms deadline
  (`raft-host: apply-sleep us=400xxx`, dozens per hold) → the REPORT
  answers BUSY (`report-split nrec=76757 … finish_ms=1881 rc=-13`; 12 of
  116 on 004) → the client's `fsync` returns EIO (W17.1) — the same
  abort the last two IORs died of. No heartbeat leaves either: group 0
  9418→9421 (04:15:43Z), group 2 2902→2904 (04:16:17Z), inside the
  holds. With W22.1–2 rolled and zero snapshot activity, this is the
  multi-second pump hold that is left.
- Why L0 is always near the cap: a flush writes one L0 file per
  `key[0]` range (W13 step 5, ≤ 16 files of ~256 KB from a 4 MiB
  memtable); compaction takes one range's L0 files (`inputs=` 5–11) and
  rewrites that range's whole L1 — 190–330 MB for ranges 1–15, **1.8 GB
  for range 0**. 433 compactions in 20 minutes on 004 (441 / 648 / 656
  on 005 / 003 / 006) summed to **159 GB** of `bytes=` against a 5.3 GB
  table; range 0's 34 rewrites are 61 GB of that and the 24 s hold. The
  compactor thread is 48 % / 55 % of fcstor004 / fcstor005's samples
  (`cm_sift_up`, `memcmp`, `cm_pop`, `kv_seg_probe`), with 5.25M 4 KiB
  `write`s (stdio default buffer) and 3.7M ~8 KB `pread`s (one per
  input block).
- The compactor's segment `fsync` is the 100 ms mode (299 of 695 on 004
  in 90–120 ms, avg 51 ms; 407 of 733 on 005); the pump's Raft-log
  `fsync` averages 0.38–0.40 ms. That closes D6 (D11): not the sharing.

**Steps (D9–D10 pending; write them as decided once the user answers):**

1. **The pump never waits for the compactor (D9).** In
   `kv_maybe_flush_locked`, when the caller is the apply path (pass a
   flag, or make the apply's flush a separate entry point), do not wait:
   flush what can be placed, or let the memtable grow past
   `memtable_max` and return OK; the memtable is bounded by what the log
   can commit ahead of the KV (512 MiB since W22.1). Back-pressure moves
   to admission: `host_propose` (or the REPORT handler before it)
   returns BUSY when the local L0 bytes are over the cap, so the client
   retries on its existing 16-attempt / 10 s budget and no follower's
   pump ever stalls. `kv_compact_locked` for tests and `--compact` keeps
   the synchronous path. Gate: a 9-client `run.sh ior` with `pump_hold_max`
   under 100 ms on every node throughout (`raft-obs`), zero
   `apply-sleep` lines above 100 ms, zero `report-split … rc=-13`, no
   term change on either group; `make test`.
2. **Compaction by bytes, and a bounded range (D10).** (a) A range is
   compacted when its pending L0 bytes reach 1/8 of its L1 bytes
   (`kv-compact: start` logs both); the L0 cap becomes a byte budget
   (1 GiB) and the 64-file cap goes — the read path already probes every
   L0 file, so the file count is not the constraint. (b) A range whose
   L1 exceeds ~256 MB is split on the next key byte at flush and at
   compaction output, so no single rewrite exceeds that. (c) The merge
   reads each input segment through a 1 MiB sequential buffer instead of
   one `pread` per block. Gate: over the same IOR, `kv-compact: end`
   `bytes=` sum under 10× the KV's growth for the run; no compaction over
   1 s; compactor under 15 % of any server's samples; `test_kv_lsm` and
   `test_partitioned_flush` pass; a KV copy from a live node reopens and
   verifies every key.
3. **Already in tree (8g), roll with the next build:** `disk_log_new_bytes`
   O(1); `kv_seg_w_open` `setvbuf` 1 MiB.

- **Read:** `src/kv/kv_compact.c` (`kv_maybe_flush_locked`, `compactor_main`,
  `kv_compact_locked`), `src/kv/kv_lsm.c` (`kv_flush_locked`), `src/kv/kv_seg.c`,
  W13 above, the "L1 compaction is a background thread" learning.
- **Forbidden:** raising `KV_LSM_MEM_DEFAULT` or `KV_LSM_L0_DEFAULT` as the
  fix (W13); raising the election timeout, `HOST_TICK_US`, or the 400 ms
  apply budget; any compaction step under `h->mu`; making `fsync` succeed
  on a merged-back dirty set; moving `mdraft/` to another device to hide
  the compactor's I/O (D11 says it is not the sharing).

Everything the §10 steps delivered (10.5c-1..35d, step 11's deletion of the old
engine, step 12 parts A–D) is landed and gated; the per-increment narrative is
in the commit history and in `.cursor/rules/efs-project-state.mdc`, not here.

**When the queue above is empty,** the next task comes from a measurement, not
from this page: run the gates in [testing.md](../testing.md), and take the
largest gap between what a gate reports and what the ceiling table at the top
of §1a says the hardware allows. If closing it needs a design decision the
spec does not contain, stop and ask (§4).

---

## 2. Routing: "I am changing X"

Read the row's **Read** column and nothing else first. The **Governs** column
is what your change must not break; the **Gate** column is what proves it.

| You are changing | Read | Governs | Gate |
| --- | --- | --- | --- |
| Any wire message | [architecture.md §7](../architecture.md) op matrix, `include/efs/protocol.h` | build-ID compat; restart all servers together | `make test`, solo posix |
| Path lookup / dentries | [protocols/directory.md](protocols/directory.md) | I5–I8 | posix, posix2 |
| create / unlink / rename / link | [protocols/directory.md](protocols/directory.md), [protocols/transactions.md](protocols/transactions.md) | I5–I9, I16, I17 | posix, posix2, posixstress |
| Anything about file size, mtime, ctime | [protocols/data.md](protocols/data.md) "lanes"/"times" | I21, I22 | posix (size-visibility tests), posix2 |
| Chunk write / publish / truncate / append | [protocols/data.md](protocols/data.md) | I11–I15, I20–I22, I24, I25 | posix, posixpersist, fio honest matrix |
| The read path, or read prefetch/caching | [protocols/data.md](protocols/data.md) "validated collect" | I24, I13 | posix, posix2 (cross-client visibility) |
| Client reconnect, leases, locks, open-unlinked | [protocols/sessions.md](protocols/sessions.md) | I19, I23, I16 | posix2, posixstress |
| Cross-shard anything | [protocols/transactions.md](protocols/transactions.md) | I16, I17, I9 | posix2, posixstress |
| Raft, KV, replication, membership | [architecture.md §7.1/§7.8](../architecture.md), [failure-tolerance.md](failure-tolerance.md) | I1–I4, I10, I18 | `tests/test_sim`, leaks |
| Production Raft host | `src/server/raft_host.c`, [architecture.md §10](../architecture.md) 10.5 | I1–I4, I16; never `efs_raft_snapshot()` until KV flush-through-applied; SNAP blob is the existing WAL item payload | `tests/test_kv_lsm`, `tests/test_wire`, `tests/stress/raft_host_smoke.sh` (scratch cluster; not live `efs-test`) |
| Simulator / applied KV SM | [verification.md](verification.md), `include/efs/sim.h`, `include/efs/meta_apply.h`, `include/efs/raft.h` | I1–I4, I9, I10, I13–I16, I20–I23, I25 | `tests/test_sim`, `tests/test_meta_apply`, `tests/test_raft` |
| Op-ID / idempotency window | [architecture.md §7.9](../architecture.md), `include/efs/opid.h` | I16 | `tests/test_sim` |
| A hot path, for speed | [performance.md](performance.md) | P1–P4, §8 contract | fio honest matrix — **never** the stock `perf` write column |
| FUSE client behavior | [architecture.md §7.7](../architecture.md) | I24, kernel-cache rules | posix, posix2 |
| Module structure / file layout | [development.md](development.md) | ~1000-line file cap; header-only deps | `make test` + the suite for whatever moved |
| The spec itself | [development.md](development.md) "machine gate" | one home per normative table | regenerate `architecture-full.md`; links + `I1..I25` resolve |

Invariant texts live in [architecture.md](../architecture.md) §4. Where state
lives and which shards an operation touches live in §5 and §6 — those two
tables are the single source of truth; satellites explain them and never
restate them.

---

## 3. Done means

A change is finished when all of these hold. Do not stop early and do not
substitute one for another.

1. **It builds on a node** — never in the NFS home
   (see the fcstor deploy rule; a local `make` produces AVX-512 objects that
   SIGILL on the AMD test nodes).
2. **Unit tests pass:** `make test` (`test_sim`, `test_meta_apply`,
   `test_raft`, `test_kv_lsm`, `test_stage_evict`, … — no accepted-failure
   list).
3. **The gate from your routing row passes**, run with
   `tests/run_tests.sh <suite>`, and the result directory is recorded.
4. **A failure is a failure.** A timeout is not a skip; an empty TSV is not a
   pass; a suite that ran against a dead mount (`findmnt` not
   `fuse.efs-fuse`) did not run at all.
5. **The measurement is honest.** If you claim a speedup, it came from the
   documented method in [performance.md](performance.md), not from a cache.

---

## 4. Never, without asking

- Invent a design decision the spec does not contain (see §1).
- Restate a normative table in a satellite — link to its one home instead.
- Add a component as new monolith code; it lands inside the carved
  boundaries ([development.md](development.md)).
- Weaken an invariant to make a test pass.
- Widen a timeout instead of removing the work that made it slow.

---

## 5. If you are an AI agent — or briefing one

This page exists so that a **less advanced model can produce a correct
change**. That works only if the task arrives pre-chewed: the model's job is
execution inside hard edges, not exploration. The briefer (human or
orchestrator) owns: picking the step (§1), decomposing it until every
instruction is mechanical, naming the exact files to read (§2 — the Read
column and nothing else), and reviewing the diff. The agent owns: staying
inside the named files, and the checklist in §3 — all of it.

A briefing that works, paste-able:

```text
You are making ONE behavior-preserving change to the efs repo.
Read first, in order, and read nothing else:
  docs/arch/START-HERE.md, then only the files your routing row names.
Task: <one step, decomposed until mechanical: what moves, what does
not, what is forbidden>
Hard rules: no logic changes outside the task; no renames; no new
dependencies; no files outside the ones named; if anything seems to
need a design decision the spec does not contain, STOP and report —
do not decide.
Done means: builds on a test node (never in $HOME), make test passes,
the routing row's gate passes via tests/run_tests.sh, and you record
the result directory. A timeout is a failure. Verify the mount with
findmnt before trusting any suite result.
```

Two traps a pasted briefing must name because an outside agent cannot
rediscover them (the other recurring ones are already in §3):

- **EEXIST on a unique, never-used name is a bug, never benign.** Do not
  swallow it as a race and move on.
- **`pgrep -x`, never `pgrep -f`** on this project's processes — the `-f`
  pattern matches your own ssh command line and kills your own session.
