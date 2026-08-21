# Phase 3 design — Real sharding

[Roadmap](scaling-roadmap.md) · [Phase 2](phase2-server-owned-metadata.md)

Status: **code complete (Aug 21).** Phase 2 owns the table on the server and
routes FUSE through RPC. Phase 3 splits that table so no op and no flush is
O(total inodes). This is the phase that raises the ~14M inode cap.
**Do not set `shard_bits>0` on the live cluster** until an extra-shard
restart + owner flush is proven on a throwaway export.

## Do we need Phase 2c?

**No.** 2c as written is a **full-table gen-check cache** for the unsharded
primary (one gen, one blob). That grain is wrong once shards exist: a create
in shard 7 must not invalidate shard 3, and a node must not pin the whole
export to validate a cache.

The *idea* behind 2c — do not RPC every getattr — is still required. It is
item 4 below: **per-shard, on-demand, LRU**. Same-fd POSIX zeros and
RPC-every-stat cost get fixed there, not by building 2c first.

Skip 2c. Do not implement a monolithic gen-check cache.

## Target

```
  create / lookup(parent, name)          getattr / GETCHUNKS(ino)
           │                                      │
           ▼                                      ▼
     parent shard owner                     inode shard owner
     (dentries live here)                   (inode + its chunks)
           │                                      │
           └────────────┬─────────────────────────┘
                        ▼
              per-shard table + gen + CoW pages
              per-shard flush / catchup / dirty
```

`shard = ino & (shard_count - 1)` (`efs_export_shard_of`; the root ino is
pinned to shard 0). Low bits, not `ino >> shard_bits`: the shift scheme gave
each shard only `2^bits` inos (shard 0 exhausted after two creates → every
mkdir returned EEXIST) and unbounded shard ids. Each shard allocates from its
own congruence class, so per-shard ino space is unbounded. Shard 0 is today's
single blob (`shard_bits = 0`, `shard_count = 1`). Each shard's pages live
under `efs_meta_shard_table_ino(shard)` (shard 0 == `EFS_META_TABLE_INO`).

Owner (4 nodes, ids 1..4): among **live** nodes, `sorted_live[shard % nlive]`
— the live list is sorted ascending inside `efs_shard_owner_of` so every
node and client computes the same owner regardless of list order.
When `shard_count <= 1` this is the current meta primary (lowest live id).
Until a shard has its own table+flush, **every RPC still goes to the export
primary** — routing to a replica would mutate a copy that does not flush.

## Work (order)

1. **Helpers + ino allocation (done)**
   - `efs_shard_owner_of` — tested; client will use it once tables split.
   - `efs_export_alloc_ino(ex, parent)` — `shard_bits == 0` is today's
     `next_ino++`. When bits > 0, allocate in the **parent's shard**.
2. **Per-shard tables (in-memory, this cut)**
   - `efs_export_table` / `efs_export_table_for_ino`: shard 0 is the export;
     shard > 0 is allocated on demand (no 4096 empty tables).
   - RPC lookup/create/getattr/readdir/GETCHUNKS/REPORT_CHUNKS use the
     owning table. `shard_bits == 0` is unchanged.
   - `efs_ino_is_meta_table` — quota/direct-io/placement treat every
     `efs_meta_shard_table_ino(s)` as metadata.
3. **Per-shard flush (pages, this cut)**
   - Flush writes pages under `table_ino` (shard 0 == today's
     `EFS_META_TABLE_INO`). Extra shards flush pages then install a
     **local** root — they must not PUT_META (that is the export EFSR).
   - **v8 (this cut):** cluster EFSR lists extra-shard roots
     (`extra_shard_count` + nested EFSRs). Extra pages flush first, then
     shard 0 PUT_META. Catchup installs extras and rebuilds from
     `table_ino`. v7 loads as extra_count=0. Do not set `shard_bits>0`
     on the live cluster until an extra-shard restart is proven.
4. **Route RPCs to the shard owner (done)** — client `rpc_owner_conn`
   computes `live[shard % nlive]` when bits>0. Server accepts mutations
   on that shard's owner. Extra owners flush their pages and PUT_META
   extras (no shard-0 rewrite). Primary still flushes shard 0.
5. **On-demand load + LRU (done)** — `efs_export_table` reloads an extra
   from the v8 root; `efs_export_evict_cold_shards` drops cold extras;
   `efs_client_load_shard` instantiates the client table. getattr skips
   RPC when dcache has the ino (writer read-your-writes).
6. **Spread creates (done)** — files round-robin across shards with
   `create_stride = nlive` (same owner as the parent). Dentry stays on
   the parent shard; inode+chunks live on the child shard. Dirs stay
   with the parent.
7. **Online re-shard (done)** — `efs-mgmt upgrade` calls
   `efs_export_rehash` to move rows and keep dentries on the parent
   shard.

## What stays

- Data path (chunk PUT/GET, 2+1 EC, RDMA) is already parallel.
- v7 CoW page placement, catchup-vs-GC, build-id gate.
- `shard_count = 1` exports keep working with no format change.

## Milestone

`shard_count > 1` serves create/lookup/readdir across shards (unit-tested);
a flush of an extra shard does not rewrite shard 0; RAM tracks the hot set
via LRU. POSIX same-fd zeros closed by the dcache overlay (not 2c). Live
cluster stays `shard_bits=0` until extra-shard restart is proven.
