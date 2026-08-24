# Scaling roadmap: toward ≥ 2³² files/folders

[Design](design.md) · [Failure tolerance](failure-tolerance.md) · [Operations](operations.md)

This is the plan for raising the inode ceiling from the current ~14M to at
least 2³² (4.29 billion) files/folders. It is a **multi-phase** effort. Phase 1
(shipped) makes the current model robust; Phase 2 (shipped: server-owned
RPCs + RPC reads) is in place; Phase 3 (proven on throwaway bits=3 —
see [phase3-sharding.md](phase3-sharding.md)) splits the table. Phase 4 is density.
**Skip Phase 2c** (full-table gen-check cache); cache as Phase 3 item 4
(per-shard, on-demand, evict). Write-throughput next steps (lock partition,
N pollers, bits>0 data path) live in **Performance next** below, alongside
two Aug 24 workstreams: the **metadata op storm** (many small files) and the
**streaming-write barrier** (ewrite vs NFS). **All data traffic goes via
RDMA** (client path already does; server↔server chunk/fragment payloads
must follow — see below).

## Why the current model caps at ~14M

The metadata is a **single monolithic blob**, fully replicated and fully
held in RAM by every server *and* every client:

| Resource | Now (4.5M files) | At 2³² (4.29B) |
|---|---|---|
| Serialized inode region | ~703 MB | **~670 GiB** |
| Blob per flush / resync | 1.148 GB | **~660+ GiB** |
| In-memory table / node | ~1.7 GB | **~1.65 TB** (384 B `struct efs_inode`) |
| Hard cap | **~13.9M files** | goal: 4.29B (~309×) |

Binding constraints (see `include/efs/common.h`, `include/efs/metadata.h`):

- `EFS_META_MAX_PAGES = 32768` × 128 KiB = **4 GiB** blob cap.
- `EFS_META_INO_PAGE_MAX = 16384` × 128 KiB = **2 GiB** inode region.
- Full-blob flush, full-blob resync, and a **single cluster-wide metadata
  writer** (the flush election) are all O(total inodes).
- `efs_ino_t` is already 64-bit — the *numbering* space is fine; the
  *storage/replication/concurrency* model is the wall.

Phase 1 (shipped) removed the acute pathologies — resync/rebuild stall the
global locks, and a wedged writer can monopolize the election — but does **not**
raise the cap. Phases 2–4 do.

## Existing seeds to build on

- **EFSR v6 shard fields**: `root.shard_count` / `root.shard_bits`
  (`shard = ino >> shard_bits`), per-shard table inos
  `efs_meta_shard_table_ino(shard)` (`include/efs/metadata.h`,
  `src/common/metadata.c`).
- **Server-side inode RPC skeleton**: `src/client/inode_rpc.c`
  (`efs_client_rpc_create/unlink/...`) — currently unused by `efs-fuse`.
- **`efs-mgmt upgrade <node> <export> <shard-bits>`** sets shard_bits
  (`src/server/handler.c`).
- **Stub**: `efs_client_load_shard` returns `EFS_ERR_NOT_FOUND` for shard > 0
  (`src/client/inode_rpc.c`) — multi-shard is not wired up.

---

## Phase 2 — Server-owned metadata

**Goal:** clients stop holding and flushing the full table. Metadata mutations
become RPCs to the server that owns them; the client keeps only a read cache.

Today `efs-fuse` mutates a local table copy and periodically serializes +
replicates the *entire* blob through a cluster-wide election. That is the
scaling killer: every client pays O(table) RAM and O(table) flush.

**Work:**

1. Route FUSE metadata mutations (`create`/`mkdir`/`unlink`/`rename`/
   `setattr`/`link`) through the `inode_rpc.c` path instead of local-table
   mutation + blob flush.
2. Servers become the metadata writers: own the authoritative in-memory table,
   apply RPC ops, and handle persistence + server-to-server replication
   internally (the election/blob-flush becomes a server concern, off the
   client critical path).
3. Client keeps an invalidation-based **read cache** for lookup/readdir/getattr
   (not the authoritative copy), so read-heavy workloads stay fast.
   **Superseded (Aug 24):** no client metadata cache — immediate cross-client
   visibility (posix2) forbids TTL/negative caching, so the metadata op storm
   is fixed by *round-trip elimination* instead (whole-path `LOOKUP_PATH`,
   `fi->fh` fast paths). See **Performance next — metadata op storm**.
4. Data path (chunk PUT/GET) is unchanged — it already scales and stays
   client→server direct.

**Why:** clients become thin (no full-table RAM, no blob serialize, no
election). Metadata write throughput is no longer funneled through one
client-driven blob flush.

**Decouples/risks:** must preserve today's dirty-set/rebase correctness
(or replace it with a server-side op log); read-cache invalidation correctness;
`next_ino` allocation moves server-side.

**Milestone:** `efs-fuse` mounts and passes `tests/` with metadata served by
RPCs; client RSS no longer grows with table size.

---

## Phase 3 — Real sharding + on-demand load/evict

**Goal:** bound every metadata op to O(shard), and no node holds more than a
working set. **This is the phase that actually raises the cap.**

**Work:**

1. Per-shard paged tables (`efs_meta_shard_table_ino`) — **done**.
2. Per-shard flush + v8 extra roots; extra owners flush their pages and
   PUT_META extras; primary flushes shard 0 — **done**.
3. Owner routing (`rpc_owner_conn` + server `NOT_PRIMARY`) — **done**.
   Extra-shard restart + owner-only rebuild proven on throwaway
   `efs-s3` bits=3 (Aug 24). Live `efs-test` stays bits=0.
4. On-demand load — **pages are the DB (Aug 24):** extras PUT_META
   updates descriptors and evicts a stale copy; only the shard owner
   materializes the table. Not a journal — CoW pages + root commit.
   `evict_cold_shards` is a RAM cap, not the load path. Unlink/nlink
   no longer instantiate every extra table on the parent. CoW flush
   reuses unchanged pages; shard 0 flushes only when dirty.
5. Spread creates (parent dentry + child inode, stride=nlive) — **done**.
6. Online re-shard (`efs_export_rehash` via `efs-mgmt upgrade`) — **done**.

**Sizing example:** target ~1M inodes/shard. 2³² inodes / 2²⁰-per-shard ⇒
`shard_bits = 20` gives up to 2^(32−20) = 4096 shards, each ~150 MB blob —
a per-shard flush/resync is then bounded and fast.

**Depends on:** Phase 2 (clean shard ownership needs server-owned metadata).

**Milestone (proven Aug 24, throwaway `efs-s3` bits=3):** create/lookup
across 8 shards; extra-owner kill/restart rebuilds only owned extras;
cold remount + peer md5; same-gen extras merge from `GET_META_ROOT`.
Live `efs-test` remains bits=0.

---

## Performance next — bits=0 write ceiling

The CPU/lock chain through Aug 23 (shared-CQ poller, generation-tagged
`wr_id`, conn-thread leak, server/client spin caps, O_DIRECT aligned
recv, `REPORT_CHUNKS` yield) raised fresh-cluster peaks to:

| Workload | Now | Notes |
|---|---|---|
| Single 007 sw-1m | **5656 MiB/s** | was 4993 |
| Multi-9 sw-1m agg | **9342 MiB/s** | was 8270; 1.65× single, not 9× |
| Multi-9 sr-1m agg | **30513 MiB/s** | was 27931 |
| Multi-9 rw-4k agg | **13934 MiB/s** | flat vs 13817 |

Ceilings: HDR 200 Gb/s (~25 GB/s), NVMe 17–21 GB/s per server. Reads
already scale; **writes do not.** Always measure on a fresh `edelete` +
`mkfs` — a grown chunk table silently costs 15–40%, so A/B on a live
table is meaningless.

**Ruled out (measured, do not re-try as the first lever):**

- `EFS_WRITE_PIPELINE` 64 is *worse* than 32 (single sw-1m 3780 vs 4686).
  Single-client is bound by per-chunk latency + client CPU (blake3 /
  memmove), not pipeline depth.
- `EFS_RDMA_BUFS` 32 ≈ 4. Recv depth is not the limiter.

**Remaining write ceiling:** under 9-way write, per-chunk server latency
inflates ~870 µs → ~3.9 ms (8.8 ms avg / 447 ms max fio clat after the
REPORT yield; was 12 ms / 1.5 s). Residual `g_server->lock` sharing
between REPORT / flush / PUT plus a single `recv_poller` at ~30% CPU.

**Next levers, in order:**

1. **Partition `g_server->lock`.** Today every `PUT_CHUNK` takes the
   global lock just to `export_acquire`, and `REPORT_CHUNKS` still holds
   it across 1024 recs (was 8192). Split export-lookup / inflight from
   table mutate so data-path PUTs never wait on a report or flush.
   Expected: most of the 870 µs → 3.9 ms inflation goes away; multi
   sw-1m should climb toward N × single until the next wall.
2. **N recv CQs / pollers.** One shared CQ + one poller harvests every
   incoming PUT. `ibv_poll_cq` is 30% of server CPU under 9 writers —
   not yet saturated, but will be as (1) raises completion rate. One
   poller (or CQ) per NIC / per NUMA / per N conns.
3. **Phase 3 data-path sharding (`bits>0` on the live write path).**
   The real fix: `REPORT_CHUNKS` and table apply become per-shard-owner,
   so 9 clients no longer serialize on one primary. Do this after (1)
   unless a sharded export is already the target of the next validation.
4. **Table-growth cost.** `set_chunk` / REPORT apply get more expensive
   as the live table grows (the silent 15–40% drop). Hash/index the
   chunk table or evict cold mappings (Phase 3 on-demand LRU already
   exists for *shards*; bits=0 still holds the whole table).
5. **Client CPU (lower priority).** At 5.6 GB/s single, blake3 + memmove
   are ~33% of efs-fuse. Worth it only after (1)–(3): the multi write
   ceiling is the server lock, not client hash.

**Milestone:** multi-9 sw-1m agg ≥ 3× single-client (~17 GB/s) on a
fresh cluster, with 99th-percentile 1 MiB write latency under 20 ms.
That is still well under the NVMe/IB ceilings; (3) is what takes it
the rest of the way.

---

## Performance next — metadata op storm (many small files)

Measured Aug 24: `ecopy` of ImageNet into a bits=3 mount runs at
**~70 files/s**. One depth-3 file costs **~15–20 synchronous metadata RPCs**:
`efs_client_lookup` walks one LOOKUP RPC per path component
(`src/client/ops.c:329-365`), plus on bits>0 a GETATTR per regular-file
component; `release` re-walks the full path for `pack_seal` although
`fi->fh` is set; `utimens` walks then sends two SETATTRs; `getattr`
ignores `fi->fh`; non-root callers double every walk via
`check_search_path`. Kernel `attr/entry/negative_timeout=0` re-resolves
every op. Server side each LOOKUP is O(1) but all inode RPCs serialize
under `g_server->lock`.

**Levers, in order (no caches — see note):**

1. **`efs-bench --meta` + baseline FIRST.** New metadata mode
   (mkdir/create/stat/setattr/readdir/unlink phases, ops/s per phase,
   multi-worker, no caching in the bench) driving the same client op
   functions FUSE uses; `tests/run_tests.sh meta`, results in
   `results/meta/`. Baseline on the current build before any fix; the
   bench stays as the permanent metadata regression gate.
2. **Client-only round-trip cuts:** `fi->fh` fast paths in
   release/getattr/utimens/chmod/truncate, single SETATTR for
   atime+mtime, leaf-only GETATTR in the walk, fuse `check_search_path`
   into the lookup walk (halves non-root cost).
3. **`LOOKUP_PATH` RPC** (opcodes 79/80): client sends the whole path;
   the server walks all components under one lock hold (dirs never leave
   the parent-shard chain, so one server resolves everything); reply
   carries ancestor mode/uid/gid so `check_search_path` costs 0 extra
   RPCs; symlink/deep-path statuses fall back to today's walk. Any path
   resolve becomes 1–2 RPCs regardless of depth. Full-cluster
   rebuild+restart (build-id gate).
4. **POSIX parent-dir mtime/ctime bumps** on create/unlink/rename/link —
   a known POSIX gap, and the per-dir validator if a gen-checked name
   cache is ever revisited.

**Note:** kernel entry/attr timeouts stay 0; no TTL/negative/attr caches —
immediate cross-client visibility (posix2) is a hard requirement, and
caches are ruled out as the fix. A true 10k files/s needs kernel dentry
caching and is explicitly **not** the target.

**Milestone:** meta-bench walk-dominated phases (stat, setattr) ≥ 5–10×
baseline, create/unlink ≥ 2–3×, no phase regresses; posix 162 + posix2 24
stay 0-EFS-bug on bits=0 **and** bits=3; ecopy defaults improve several×.

---

## Performance next — streaming-write barrier (ewrite vs NFS)

Measured Aug 24: `ewrite.sh <mnt> 1 2` (2 × 100 GiB, 1 MiB O_DIRECT
writes) = **~49 MiB/s/stream** (98 agg); the same binary on NFS scratch =
**~600 MiB/s/stream** (1.2 GiB/s agg). Root cause is a synchronous
durability barrier per write, not net/disk: a 1 MiB write arrives as one
`write_buf` (`max_write` = 4 MiB), `efs_dcache_try_patch` rejects spans
crossing a 128 KiB chunk (`src/client/write.c:3703-3719`), and the
fallback `efs_wb_enqueue_owned` **blocks the FUSE thread until 2-of-3 PUT
quorum** (`src/client/efs_fuse.c:1749-1789`). iodepth=1 per process ⇒
~20 ms/MiB. fio reaches ~650 MiB/s/job through the same path only by
keeping 8 requests in flight. First-write amplifiers:
`export_reserve_chunks_locked` takes all 64 dir locks + the global lock
on chunk-table growth; close-time `efs_dcache_flush_ino` scans
ci=0..~800k on a 100 GiB file.

**Levers, in order:**

1. **Multi-chunk `try_patch`:** split >128 KiB writes into per-chunk
   patches (full-chunk pieces `have_base=0` fully-dirty, no GET);
   `write()` ACKs after the patch for *all* sizes — the ≤128 KiB path
   already has exactly these semantics. Bounded writeback (2 GiB soft /
   4 GiB hard dirty-bytes) stays the only throttle; durability still
   lands at flush/fsync/close (POSIX). Not a cache.
2. **Flush throughput:** reclaim threads 4 → 16 and/or pipeline
   `dcache_flush_slot` PUTs through the put pool (`EFS_WRITE_PIPELINE`=32)
   so async flush sustains the stream rate.
3. **First-write amplifiers:** bigger chunk-table growth strides,
   dirty-set batching, per-ino dirty list so close stops scanning 0..nci.

**Milestone:** `tests/run_tests.sh ewrite` ≥ NFS parity (~1.2 GiB/s agg
at 2 jobs; baseline run 20260824-140120 = 98.5/152/198/203 MiB/s at
2/4/8/16), stretch toward the fio ~5 GiB/s single-client ceiling at 8–16
jobs; posix write-path tests + mc_stress green.

---

## Performance next — all data traffic via RDMA

Client chunk PUT/GET is already RDMA (RC QP after `RDMA_SETUP`). Any
**payload** that is a chunk or EC fragment must use the same path —
including traffic that today rides the TCP peer pool:

- File-data **verify-heal** (`src/server/verify.c`: GET two peer
  fragments, XOR-reconstruct, write local)
- **Drain / migrate** and **node-left orphan heal**
  (`src/server/migrate.c`: GET/PUT fragments between servers)
- **Meta-page** GET/PUT_CHUNK during CoW flush, catchup rebuild, and
  meta-heal (`src/server/meta_server.c`)

Control stays TCP: HELLO, heartbeat, `PUT_META` / `GET_META` roots,
inode RPCs, status. Those are small messages, not data.

**Work:** give the peer pool RDMA QPs (or reuse the existing server
accept path) and send `GET_CHUNK` / `PUT_CHUNK` payloads over them.
Same 2+1 placement and quorum rules; only the transport changes.

**Milestone:** heal, migrate, and meta-page replicate no longer use
`efs_send_msg` TCP for fragment bytes; a drain or verify-heal under
load does not regress client RDMA write/read. HELLO/heartbeat/PUT_META
may remain TCP.

---

## Phase 4 — Slim the in-memory inode

**Goal:** raise per-shard density and cut RAM further.

`struct efs_inode` is ~384 B, dominated by `name[EFS_MAX_NAME]` (256 B inline)
and 80 B of directory rollups.

**Work:**

1. Move `name` out of the inline struct into a separate **dentry store** (the
   v6 wire format already packs dentries separately — mirror that in memory).
2. Make the 80 B of rollups **optional/lazy**: maintain only where the
   `.stats`/`.find` features are on, or compute on demand.
3. Re-audit field widths (`pack_off`/`pack_len` already 32-bit).

**Target:** ~96–128 B/inode in memory → 2³² × 128 B ≈ 512 GiB total, ~128 MB
per shard at 4096 shards.

**Depends on:** Phase 3 (shrinking the per-node working set only pays off once
sharding bounds how much any node holds).

**Milestone:** bytes/inode measured on a large export drops ~3×; per-shard
memory and serialize time drop accordingly.

---

## Cross-cutting concerns

- **Unprivileged on-demand clusters (goal, Aug 24):** users must be able to
  create and use a cluster from clients with no root anywhere. Already true
  today: `efsd` runs as the unprivileged user (user port, user-owned storage,
  unprivileged uverbs), and `efs-fuse` mounts via setuid `fusermount3` with
  daemon-side POSIX checks against the caller's real uid/gid. Missing:
  (1) **wire authentication** — the protocol has none (HELLO gates on
  version/build-id only; anyone on the fabric can mkfs/join/mount/delete);
  add a per-cluster capability token (HMAC on HELLO) + per-export tokens at
  minimum; (2) on-demand bring-up/teardown tooling (`efs-cluster start
  --nodes ... --dir /scratch/$USER`, the BeeGFS-on-demand/GekkoFS job
  pattern); (3) per-user storage layout on server nodes; (4) optional
  `-o allow_other` for shared mounts (needs `user_allow_other` in
  /etc/fuse.conf).
- **FUSE vs kernel module (decided Aug 24):** stay on FUSE. Data plane is
  proven (5.6 GiB/s single-client write, ~30 GiB/s multi read); the metadata
  gap is the wire protocol (RPCs per op), not FUSE upcalls, and a kernel
  client would face the same RPC cost. If mdtest-class numbers are ever
  needed, add an opt-in relaxed-coherence mount mode (entry_timeout>0 +
  server-driven invalidation / lease RPCs) — not a kernel module.

- **readdir/lookup across shards**: the name index is per-export today; with
  sharding, dentries live in the parent's shard. `efs_export_foreach_child`
  and the name index need a shard-aware form.
- **Rollups/tree stats**: `.stats`/`.find` rollups must aggregate across shard
  boundaries (or be computed per-shard and merged).
- **Backward compatibility / migration**: every phase must load existing v5/v6
  single-blob exports (`shard_count = 1`) and upgrade in place.
- **Failure tolerance**: per-shard dual-slot paging and the rebuild/repair
  path (`server_rebuild_export_from_pages`, meta-repair) extend per shard.
- **Testing**: each phase keeps `make test` green plus a multi-shard smoke
  (create/lookup/rename/readdir across shard boundaries, server restart
  rebuild, concurrent writers on disjoint shards). Perf gates: posix 162 +
  posix2 24 (0 EFS bugs), mc_stress, `tests/run_tests.sh perf` (throughput),
  `tests/run_tests.sh meta` (metadata ops/s vs baseline) and
  `tests/run_tests.sh ewrite` (streaming-write sweep vs baseline) — the last
  two are the regression harnesses for the two Aug 24 workstreams above.

## Phase ordering rationale

Phase 2 first because sharding (Phase 3) is far cleaner when a server owns the
table rather than every client blob-flushing. Phase 4 last because it only
matters once sharding bounds the per-node working set. Phase 1 (shipped) keeps
the current system usable until 2–4 land.
