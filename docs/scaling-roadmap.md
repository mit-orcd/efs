# Scaling roadmap: toward ≥ 2³² files/folders

[Design](design.md) · [Failure tolerance](failure-tolerance.md) · [Operations](operations.md)

This is the plan for raising the inode ceiling from the current ~14M to at
least 2³² (4.29 billion) files/folders. It is a **multi-phase** effort. Phase 1
(already shipped) makes the current model robust; Phases 2–4 change the
metadata architecture. Nothing here is implemented yet beyond Phase 1 — this
document exists to store the design.

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

1. Implement per-shard paged metadata tables: each shard is an independent
   blob with its own page range, checksums, and generation
   (`efs_meta_shard_table_ino`).
2. **Per-shard flush / election / resync** — the single writer becomes N
   independent shard writers, so metadata concurrency scales with shard count.
3. Shard ownership / routing: `shard = ino >> shard_bits`; route each op to the
   owning server/shard.
4. **On-demand shard loading + LRU eviction** on servers (and for any
   client-side cache), so a node holds only hot shards — complete
   `efs_client_load_shard` and add the server-side equivalent.
5. **ino allocation across shards**: replace the single global `next_ino` with
   per-shard ranges (or an allocator) so creates distribute across shards.
6. **Online re-shard**: `efs-mgmt upgrade <shard-bits>` rehashes live inodes
   into shards without downtime. Backward compat: v5/v6 single-blob loads as
   `shard_count = 1`.

**Sizing example:** target ~1M inodes/shard. 2³² inodes / 2²⁰-per-shard ⇒
`shard_bits = 20` gives up to 2^(32−20) = 4096 shards, each ~150 MB blob —
a per-shard flush/resync is then bounded and fast.

**Depends on:** Phase 2 (clean shard ownership needs server-owned metadata).

**Milestone:** an export with `shard_count > 1` serves creates/lookups/readdir
across shards; per-shard flush cost is independent of total inode count;
memory per node tracks the hot working set, not the table.

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
