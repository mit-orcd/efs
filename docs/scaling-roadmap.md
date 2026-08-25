# Scaling roadmap: toward ≥ 2³² files/folders

[Design](design.md) · [Failure tolerance](failure-tolerance.md) · [Operations](operations.md)

This is the plan for raising the inode ceiling from the current ~14M to at
least 2³² (4.29 billion) files/folders. It is a **multi-phase** effort. Phase 1
(shipped) makes the current model robust; Phase 2 (shipped: server-owned
RPCs + RPC reads) is in place; Phase 3 (proven on throwaway bits=3 —
see [phase3-sharding.md](phase3-sharding.md)) splits the table. Phase 4 is density.
**Phase 3b** (below, from the Aug 24 architecture review) shards chunk
metadata by extent and adds threshold-based hot-directory spread.
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

## Phase 3b — extent sharding + hot dirs (Aug 24 architecture review)

An external design review (Ceph/DAOS-style "everything is a distributed
object, no MDS") was evaluated against what efs already is. Most of it we
already have (identical nodes, client-computed placement, client-side 2+1
EC, data path bypasses metadata ordering, grow-only size apply, batched
dirty-ops → CoW page flush). **Apply exactly the three items below; the
rest is rejected** (see the rejected list at the end — do not revive).

Written to be executed by an agent without design judgment: follow the
steps in order, run the gates after every step, do not improvise.

### Item 0 — the design rule (documentation only, do first)

Every metadata operation is one of three classes. When adding or touching
an op, record its class in a comment and handle it accordingly:

| Class | Rule | Existing examples |
|---|---|---|
| **Commutative** | apply without ordering; max/newer wins | REPORT size grow-only + mtime newer-only (`EFS_MSG_REPORT_CHUNKS` handler), rollup counters |
| **Independent** | shard-local commit, no cross-shard talk | create/unlink inside one dir, disjoint-chunk writes |
| **Conflicting** | owner-serialized RPC + dual-apply | rename, unlink-vs-open (HOLD), O_APPEND reserve, flock |

If a new op does not obviously fit Commutative or Independent, it is
Conflicting. Never "optimize" a Conflicting op into a lock-free one.

### Item 1 — extent-sharded chunk metadata

**Problem.** Phase 3 shards *inode rows* by `efs_export_shard_of(ino, bits)`
(low bits of ino, `src/common/metadata.c`). Chunk mappings follow the inode:
every `efs_chunk_rec` for a file lives on `shard_of(ino)`. A 100 TB file at
128 KiB chunks is ~800M chunk recs (~100 B each ≈ 80 GB) on **one** shard
table — that breaks the per-shard bound (~1M inodes, ~150 MB blob) the whole
phase relies on. Fix: shard chunk records by `(ino, chunk_group)`, not `ino`.

**Current routing map (all keyed on `shard_of(ino)` — touch all of these):**

- Client report partition: `efs_client_report_dirty` (`src/client/write.c`,
  the `for (s = 0; s < sc; s++)` loop partitioning `crecs`/`irecs`).
- Server report apply + ownership drop: `EFS_MSG_REPORT_CHUNKS` handler
  (`src/server/handler.c`) → `efs_export_set_chunk(table_for_ino(...))`.
- Table routing: `efs_export_set_chunk` / `efs_export_get_chunk` /
  `efs_export_drop_chunks_from` (`src/common/metadata.c`) via
  `efs_export_table_for_ino(ex, ino)`.
- Read-side pull: `pull_chunks_range` (`src/client/ops.c`) →
  `efs_client_rpc_getchunks` (routes by ino); server
  `EFS_MSG_INODE_GETCHUNKS` handler ensures `shard_of(req->ino)` and reads
  `table_for_ino`. Also `efs_client_pull_layout_miss` (read-miss self-heal)
  and `pull_file_layout` (adopt-time pull) — both funnel through
  `pull_chunks_range`.

**Steps:**

1. **Pure refactor, no behavior change.** Add
   `uint32_t efs_export_chunk_shard_of(efs_ino_t ino, uint32_t chunk_index, uint32_t bits)`
   in `src/common/metadata.c` (+ decl in `include/efs/metadata.h`), body
   `return efs_export_shard_of(ino, bits);`. Route every site in the map
   above through it. Run the full gate (below). This must change nothing.
2. **The change.** `#define EFS_CHUNK_GROUP_SHIFT 6` (64 chunks = 8 MiB per
   group). Body becomes: group = `chunk_index >> EFS_CHUNK_GROUP_SHIFT`;
   return `efs_export_shard_of(ino, bits) ^ (mix32(group) & shard_mask)`
   where `mix32` is any fixed integer mix (reuse the `hash_mix` style
   already in metadata.c; **group 0 mixes to 0** so the first 8 MiB stays
   on the inode shard) and `shard_mask = (1u << bits) - 1`. Must be a
   pure function of `(ino, chunk_index, bits)` — every node computes it
   identically. **Not backward compatible with grown tables: fresh
   `edelete` + `mkfs` only.** Inode rows and `efs_ino_size_rec` stay on
   `shard_of(ino)` — do not move them.
3. **GETCHUNKS windows.** Client `pull_chunks_range`: split `[start, end)`
   at group boundaries; send one GETCHUNKS per group routed to that group's
   owner (add a shard-explicit routing variant — do **not** fake it by
   passing a crafted ino through `rpc_owner_conn`). Server handler: ensure
   the request's group shard is ready, serve only that group's range from
   that shard's table, and loud-`NOT_PRIMARY` (like REPORT) when the group
   isn't owned — never silently return an empty/partial list.
4. **Truncate / last-link unlink fan-out.** `efs_export_drop_chunks_from`
   (truncate, `src/client/ops.c`) and the unlink chunk-drop path must drop
   across all group shards. Follow the existing `UNLINK_SHARD` fan-out
   pattern (`server_peer_inode_rpc` in `src/server/handler.c`). These are
   rare ops; a simple loop over shards is fine.

**Wire format:** unchanged. `efs_chunk_rec` already carries `(ino,
chunk_index)`; routing is computed, never carried. No new opcodes.

**Gates (after every step):** `make test`; posix_suite 188/188 and posix2
34/34 with **0 EFS bugs on bits=0 and bits=3**; mc_stress VERIFY-OK on both
clients including kill -9 of a shard owner + restart + cold mount read;
fresh-mkfs multi-9 sw-1m within 5% of the pre-change number (grown tables
lie — always perf-test on a fresh mkfs).

### Item 2 — threshold-based hot-directory spread

**Problem.** Dentries always live on the parent's shard
(`efs_export_create_target` returns the parent shard for dirs;
`create_sharded` writes the dentry row to the parent table and the full
inode row to the child table). readdir is one local RPC
(`efs_client_rpc_readdir`) — keep that for normal dirs; the rsync wins this
week came from exactly that locality. But a 100M-entry directory puts 100M
dentries in one shard table: unbounded. Fix: spread a directory's dentries
by `hash(parent, name)` **only after it crosses a threshold**.

**Steps:**

1. **Spread predicate with no new state.** Do not add an inode field or
   wire flag. Derive it from the rollups every node already maintains:
   `is_spread(dir) = (dir->imm_files + dir->imm_dirs) >= EFS_DIR_SPREAD_MIN`
   with `#define EFS_DIR_SPREAD_MIN (1u << 16)` (65k so tests can reach it).
   Counts are fuzzy during in-flight ops, so the boundary rule below keeps
   correctness when nodes disagree by a few.
2. **Create.** When the parent is spread, the dentry row goes to shard
   `hash_name_key(parent, name) & shard_mask` (function already exists in
   `src/common/metadata.c`); the inode row still round-robins. Server
   CREATE handler must compute the identical target.
3. **Lookup.** Parent not spread: unchanged. Parent spread: LOOKUP to the
   hash shard first; on NOT_FOUND there, fall back to the parent shard
   (covers the boundary window). Never answer ENOENT from one shard alone
   when the dir is spread.
4. **Readdir.** Client-side merge: READDIR the parent owner plus one
   spread-READDIR per shard owner (add a flag on the existing
   `EFS_MSG_INODE_READDIR` request meaning "return dentries with this
   parent from *your* shard table"), dedupe by name.
   `efs_fuse_readdir` already collects the full dir per call, so merging
   is local and simple. Slow is acceptable; wrong is not.
5. **Unlink / rename.** Unlink: try the hash shard first when spread, then
   the parent shard. Rename inside a spread dir can move the dentry between
   shards — extend the `RENAME_AT` path to create the dentry on the
   destination shard then delete it on the source shard, following the
   existing `CREATE_SHARD`/`UNLINK_SHARD` fan-out pattern. **This is the
   risk item** (see the Aug 23 dual-apply rename data-loss history): a
   crash between the two steps may leave a duplicate dentry; document the
   reconcile rule (parent-shard row loses; hash-shard row wins) in a
   comment and cover it with the suite's rename-over tests.
6. **rmdir.** `efs_export_dir_empty` only sees the local child vec. For a
   spread dir, also issue spread-READDIR (max 1 entry) to each shard until
   any entry is found. Slow is fine.

**Gates:** same suite gates as Item 1, plus a new stress (add to
`tests/stress/`): 300k creates in one dir on bits=3, readdir count equals
create count, peer client sees them without remount, rename churn clean,
rmdir succeeds after emptying.

### Item 3 — (optional, orthogonal) replica-3 metadata pages

Meta pages currently ride the data 2+1 EC path (each CoW page = fresh ci =
2 data + 1 parity fragments). Change: meta-designated pages store **3 full
copies** instead. Buys double-failure durability (metadata loss kills the
filesystem; data loss kills files — over-protect meta) and simpler catchup
(fetch 1 copy, no XOR decode). Costs 2× metadata storage — negligible at
metadata scale. **Hard requirement:** implement as a replica-3 bit in the
v8 page descriptor on the *same* chunk PUT/GET/checksum/GC path — never a
parallel storage path (two-paths-diverge is the bug class that has bitten
us twice). The commit/fence/gen/GC protocol is untouched; expect no bug-rate
change. Gate: kill -9 mid-flush remounts clean + the 76753ef catchup-vs-GC
reproducer.

### Rejected from the review (do not implement)

- **Millions of tiny consensus groups.** Our 8 shards already produced the
  catchup storm, dual-writer root-gen, extras-loss-on-adopt, and
  catchup-vs-GC bugs. More groups = more of exactly that.
- **MVCC / per-inode epochs.** CoW gen pages already give crash recovery;
  the cost is GC, and GC is where the races were.
- **SPDK / bypassing XFS.** Server CPU is ~2.6 cores under 9-way write;
  the local FS is not the bottleneck. Lock contention and RPC RTTs are.
- **Universal hash dentries.** Regresses readdir/stat locality for every
  directory to fix the rare huge one — Item 2's threshold is the fix.
- **Distributed range leases.** Revisit only if sub-chunk cross-client RMW
  becomes a measured need; `append_rsv` covers today's cases.

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

- **`efs-mgmt` kick-clients (ops, not started):** a management call that
  tells every `efs-fuse` to disconnect so an upgrade / wipe / rolling
  restart cannot leave a live mount REPORT/flushing onto a new table
  (the 232 MB leftover-client adopt). Today there is **no mount census**
  — HELLO is node-join only; `flock_token` is per-open-hold, not a
  registered mount; servers only see anonymous `efs_conn` slots mixed
  with peer efsd and mgmt. A first cut can close non-peer client conns
  on every efsd (enough to fence leftovers). A proper command
  (`efs-mgmt disconnect-clients` / `list-clients`) needs a client
  HELLO that records host + mountpoint + token, then a push
  (unicast RPC or conn close) that makes `efs-fuse` umount/exit
  rather than silently reconnect. Use before `upgrade`, mkfs, and
  build-id rolls. Do not implement in this cut.

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
