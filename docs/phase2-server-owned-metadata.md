# Phase 2 design — Server-owned metadata

[Roadmap](scaling-roadmap.md) · [Design](design.md) · [Failure tolerance](failure-tolerance.md)

Status: **design only — nothing here is implemented.** This is the detailed
plan for Phase 2 of the [scaling roadmap](scaling-roadmap.md). Phase 2 makes
metadata mutations server-owned RPCs so clients stop holding and flushing the
full table. It does **not** raise the inode cap by itself (that is Phase 3);
it removes the client-side O(table) RAM and O(table) flush, and puts metadata
writes under server control so Phase 3 sharding can bound them.

## 1. Why — the current model is the scaling killer

Today every `efs-fuse` client:

1. Holds a **full in-memory copy** of the export table (`g_client.export`).
2. Mutates that local copy (`efs_client_create`/`unlink`/...), tracking dirty
   inodes in a dirty-set (`dirty_ino_keys`, `dirty_chunk_count`, `meta_dirty`).
3. On `fsync`/periodic flush, wins a **cluster-wide election**, serializes the
   **entire blob**, and `PUT_META`s it to every server.

Costs at scale (see roadmap §1): client RAM ~384 B/inode, flush is O(total
inodes), and all metadata writes funnel through one client-driven election.
At 4.5M files the blob is ~1.1 GB; at 2³² it is ~660 GiB — unworkable.

## 2. Target architecture

```
        create/unlink/rename/...              lookup/readdir/getattr
               │                                    │
   ┌───────────▼───────────┐            ┌──────────▼───────────┐
   │   efs-fuse (thin)      │            │  efs-fuse read cache │
   │  efs_client_rpc_*()    │            │  (invalidation-based)│
   └───────────┬───────────┘            └──────────┬───────────┘
               │ INODE_RPC                         │ INODE_RPC (or cache hit)
               ▼                                    ▼
        ┌─────────────────────────────────────────────┐
        │        metadata PRIMARY (one per export)     │
        │  authoritative in-memory table (s->lock)     │
        │  applies ops, batches, flushes               │
        └───────────────┬─────────────────────────────┘
                        │ server-side flush (blob, then op-log)
                        ▼
        ┌─────────────────────────────────────────────┐
        │        REPLICA servers (catchup/install)     │
        └─────────────────────────────────────────────┘

   Data path (chunk PUT/GET) is unchanged — client→server direct.
```

- **Clients are thin**: no full-table RAM, no blob serialize, no election.
  Mutations are RPCs; reads are RPCs or cache hits.
- **Servers own the table**: one primary per export applies ops and persists +
  replicates. Replicas install the primary's snapshots (existing catchup).
- **Phase 3** then shards the primary role (`shard = ino >> shard_bits`) so
  write concurrency scales; Phase 2 keeps a single primary per export.

## 3. The metadata primary (single writer per export)

The hard requirement: **all mutations for an export funnel to one server**,
else replicas diverge. Options:

- **(a) Fixed primary** — lowest live node id owns the export. Simple;
  failover = next-lowest live node. No election traffic.
- **(b) Elected primary** — reuse the existing flush-election machinery to
  pick a writer. More moving parts; the election was designed for clients.
- **(c) Per-shard primary** — Phase 3. Out of scope here.

**Recommendation: (a) fixed primary = lowest live node id**, computed from the
same live-node set the catchup already uses. The primary applies RPC ops and
flushes; replicas serve reads from their installed copy and **forward**
mutations to the primary (or the client routes to the primary directly — see
§7). On primary failure, the next-lowest live node takes over after installing
the latest committed root (existing catchup path); its `next_ino` high-water
comes from that root, so ino allocation stays unique.

Open question (§9): primary failover must not lose acknowledged-but-unflushed
ops. See §5.

## 4. Flush batching (server-side)

A per-op flush is a full O(table) serialize — far too slow. The server must
**batch**:

- The primary applies each RPC op to its in-memory table under `s->lock` and
  sets `export_meta_dirty`.
- A **server flush thread** (new) wakes when dirty and coalesces: serialize +
  replicate at most every `EFS_META_FLUSH_MS` (e.g. 100 ms) **or** after
  `EFS_META_FLUSH_OPS` dirty ops (e.g. 1000), whichever first.
- This reuses the existing `server_flush_fragmented_meta` (CoW, gen++, GC) —
  the flush itself is already crash-safe and incremental at the page level.

This is the same batching the client does today, moved server-side. The
O(table) serialize cost remains until Phase 3 bounds it per shard; Phase 2's
win is taking it (and the RAM, and the election) off every client.

`fsync` semantics: today `fsync` forces a flush so the mutation is durable.
With batching, `fsync` must **wait for the next flush to commit** (condvar on
gen advance), not trigger a serialize itself. This keeps `fsync` correct
(durable after return) without serializing per fsync.

## 5. Op-log vs blob (persistence/replication format)

- **(A) Blob** (status quo, server-side): serialize the whole table, PUT_META
  to replicas. Simple, reuses everything, but O(table) per flush.
- **(B) Op-log**: log each mutation (create/unlink/rename/...), replicate the
  log; replicas replay. O(op) per flush, but needs a replay path, idempotent
  ops, and periodic compaction to a blob (for catchup of a fresh replica).

**Recommendation: (A) blob for Phase 2.** It is correct today and the point of
Phase 2 is *where* the flush runs (server, batched) and *who pays* (not the
client), not the wire format. Phase 3 sharding bounds the blob to O(shard),
which is the real fix for flush cost. Revisit (B) only if the server-side
blob flush is measured to be the bottleneck after Phase 3.

**Durability window:** batched flush means an acknowledged op can be lost if
the primary crashes before the next flush. This matches today's client-driven
model (a client batches, then flushes). To bound it: the flush thread commits
at least every `EFS_META_FLUSH_MS`, and `fsync` waits for commit (§4). A
write-ahead op-log (B) would close the window fully — deferred.

## 6. Read path & cache invalidation

Reads (`lookup`/`readdir`/`getattr`) must stay fast — they dominate most
workloads.

- **Phase 2b (correctness first)**: route reads through INODE_RPC to the
  primary (or any replica — reads are safe from a slightly stale replica for
  most uses; see below). No client cache yet. Slower per-op but correct.
- **Phase 2c (fast path)**: add an invalidation-based client read cache.
  Options:
  - **(i) Server push** — primary pushes invalidations to mounted clients on
    mutation. Needs a server→client channel and per-client state. Most
    complex, fewest stale reads.
  - **(ii) Gen-check / TTL** — client caches with the table generation; on
    use, cheaply validate gen against the server (or a short TTL). Simple;
    bounded staleness.
  - **(iii) No invalidation** — pure TTL. Simplest; can serve stale data.

**Recommendation: 2b = route reads via RPC (no cache). 2c = (ii) gen-check
caching** — the client already tracks the committed generation; a
`GET_META_ROOT`-style gen probe is cheap, and a matching gen means the cached
entries are valid. Push invalidation (i) only if gen-check proves too chatty.

Stale-replica reads: a replica may lag the primary by up to one flush. For
Phase 2 route *reads* to the primary too (strong consistency, simplest
correctness), and only spread reads to replicas later if primary read load
requires it.

## 7. RPC surface & routing

Existing skeleton (`src/client/inode_rpc.c`, `src/server/handler.c:1278+`):
`lookup`, `create`, `getattr`, `readdir`, `unlink`. Handlers mutate the
in-memory table but **do not set `export_meta_dirty` or flush** — that is the
first thing to fix (§8, 2a).

**Missing ops to add** (client stub + server handler + wire msg):
`mkdir` (create with `S_IFDIR`), `rmdir`, `rename`, `setattr` (chmod/chown/
truncate/utimes), `link`, `symlink`, `readlink`. `write`/`read` data path is
unchanged (chunk PUT/GET).

**Routing:** `rpc_first_conn` currently picks the *first live* node — wrong
for Phase 2 (mutations must reach the primary). Change to route mutations to
the primary (§3); reads may go to the primary (2b) or any replica (later).
Client learns the primary from the node table (lowest live id) and re-resolves
on `EFS_ERR_NET` / a `NOT_PRIMARY` status.

**`next_ino` allocation** moves server-side: the primary allocates from the
committed root's high-water mark (monotonic, survives failover via the root).

## 8. Sub-phases & milestones

- **2a — server-side mutation durability.**
  - RPC mutation handlers set `export_meta_dirty`.
  - Add the server flush thread (batched, §4) calling
    `server_flush_fragmented_meta`.
  - `fsync` waits for flush commit (condvar on gen).
  - *Milestone:* a create via `efs_client_rpc_create` survives a primary
    restart and is visible on all replicas. Client behavior unchanged (still
    blob-flushes) — both paths coexist via the election.
- **2b — route FUSE mutations through RPC.**
  - Add missing RPC ops + handlers (§7).
  - Switch `efs_fuse_create/mkdir/unlink/rmdir/rename/setattr/link/symlink`
    from `efs_client_*` to `efs_client_rpc_*`.
  - Client stops dirty-tracking + blob-flushing for mutations; reads via RPC.
  - *Milestone:* `efs-fuse` mounts and passes `tests/` with metadata served by
    RPCs; client RSS no longer grows with table size.
- **2c — client read cache** (gen-check, §6).
  - *Milestone:* lookup/readdir/getattr hit the cache; read throughput back to
    local-table levels; invalidation correct under concurrent mutation.

## 9. Risks & open questions

- **Primary failover durability**: acknowledged-but-unflushed ops are lost on
  primary crash (§5). Acceptable for Phase 2 (matches today); op-log closes it
  later. `fsync`-then-crash must not lose data — `fsync` waits for commit.
- **Dirty-set/rebase**: today the client rebases dirty ops onto a resynced
  table. Server-owned metadata removes the client dirty-set entirely (the
  server table is authoritative) — simpler, but the server must apply
  concurrent RPC ops correctly under `s->lock` (already the case) and the
  flush must snapshot a consistent gen (existing CoW flush does).
- **Election during transition**: old clients (blob flush) and new servers
  (RPC) coexist; the existing election serializes client flushes against
  server flushes. Once all clients are on RPC (2b), the client election is
  dead code — remove it then, not before.
- **Backward compat / migration**: a v7 single-blob export loads as
  `shard_count = 1`; server-owned metadata works on it unchanged. No on-disk
  format change in Phase 2 (the blob/root format is the same; only *who*
  writes it changes).
- **Read consistency**: 2b routes reads to the primary (strong). If primary
  read load is the bottleneck, relax to replica reads + gen-check later.
- **Rename across the table**: rename is a multi-inode op; it must be atomic
  under the primary's `s->lock` (it is, being a single RPC handler).

## 10. What Phase 2 does NOT do

- Does not raise the inode cap (Phase 3 sharding does).
- Does not shrink the in-memory inode (Phase 4).
- Does not change the data path (chunk PUT/GET) or the on-disk blob/root
  format.
- Does not add op-log durability (deferred; batched blob flush first).
