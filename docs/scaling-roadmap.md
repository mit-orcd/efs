# Scaling roadmap: toward ≥ 2³² files/folders

[Design](design.md) · [Failure tolerance](failure-tolerance.md) · [Operations](operations.md)

This is the plan for raising the inode ceiling from the current ~14M to at
least 2³² (4.29 billion) files/folders. It is a **multi-phase** effort. Phase 1
(shipped) makes the current model robust; Phase 2 (shipped: server-owned
RPCs + RPC reads) is in place; Phase 3 (started — see
[phase3-sharding.md](phase3-sharding.md)) splits the table. Phase 4 is density.
**Skip Phase 2c** (full-table gen-check cache); cache as Phase 3 item 4
(per-shard, on-demand, evict). Write-throughput next steps (lock partition,
N pollers, bits>0 data path) live in **Performance next** below.

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
   **Do not enable bits>0 on the live cluster until extra-shard restart
   is proven.**
4. On-demand load + LRU (`efs_export_table` / `evict_cold_shards` /
   `efs_client_load_shard`) — **done**.
5. Spread creates (parent dentry + child inode, stride=nlive) — **done**.
6. Online re-shard (`efs_export_rehash` via `efs-mgmt upgrade`) — **done**.

**Sizing example:** target ~1M inodes/shard. 2³² inodes / 2²⁰-per-shard ⇒
`shard_bits = 20` gives up to 2^(32−20) = 4096 shards, each ~150 MB blob —
a per-shard flush/resync is then bounded and fast.

**Depends on:** Phase 2 (clean shard ownership needs server-owned metadata).

**Milestone:** an export with `shard_count > 1` serves creates/lookups/readdir
across shards; per-shard flush cost is independent of total inode count;
memory per node tracks the hot working set, not the table.

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
  rebuild, concurrent writers on disjoint shards).

## Phase ordering rationale

Phase 2 first because sharding (Phase 3) is far cleaner when a server owns the
table rather than every client blob-flushing. Phase 4 last because it only
matters once sharding bounds the per-node working set. Phase 1 (shipped) keeps
the current system usable until 2–4 land.
