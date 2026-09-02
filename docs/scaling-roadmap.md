# Scaling roadmap: toward ≥ 2³² files/folders

[Architecture](architecture.md) · [Design](design.md) · [Failure tolerance](failure-tolerance.md) · [Operations](operations.md)

> **Read [architecture.md](architecture.md) first.** It is the specification
> (goal, failure model, consistency model, invariants, and the target
> metadata design: one Raft group per shard over an on-disk KV). This roadmap
> is the increment plan for the *current* implementation. Where the two
> disagree, the architecture document is the target and this document is the
> path.

This is the plan for raising the inode ceiling from the current ~14M to at
least 2³² (4.29 billion) files/folders. It is a **multi-phase** effort. Phase 1
(shipped) makes the current model robust; Phase 2 (shipped: server-owned
RPCs + RPC reads) is in place; Phase 3 (proven on throwaway bits=3 —
see [phase3-sharding.md](phase3-sharding.md)) splits the table. Phase 4 is density.
**Phase 3b** (below, from the Aug 24 architecture review) shards chunk
metadata by extent and adds threshold-based hot-directory spread.
**Skip Phase 2c** (full-table gen-check cache); cache as Phase 3 item 4
(per-shard, on-demand, evict). Write-throughput next steps (N pollers,
PUT_CHUNK off the global lock, bits>0 already live) live in **Performance
next** below, alongside two Aug 24 workstreams: the **metadata op storm**
(many small files) and the **streaming-write barrier** (ewrite vs NFS).
**All data traffic goes via RDMA** (client + server↔server chunk/fragment
payloads; auto first-inode hang **closed Aug 30** — see below).

**Status at a glance (Sep 1):** Phase 1 **shipped**; Phase 2 **shipped**
(server sole metadata writer, 2PC root commit Aug 26); Phase 3 **done**
(bits=3 is the live default); Phase 3b Items 0–2 **done**, Item 3 optional;
Phase 4 **not started**. **The next work is [Phase M](#phase-m--carve-the-monolith-first-the-dev-cycle-lever)
— carve the monolith into `raft/ kv/ meta/ wire/ data/ client/` behind
interfaces.** It is the dev-cycle lever and the hard prerequisite for the
architecture migration's simulator step; everything else forward-looking
queues behind it. Per-op drop of `g_server->lock` for hot inode RPCs
**gated** (LINK/RENAME/HOLD still `lock_all`; `EFS_LOCK_PROF` shows they
are not the 9×4 ceiling). 9×4 posixstress is still ~95% `timeout after
15s`; LOCK-PROF split (`20260830-160943`) is **H3 client/wire RTT** —
`1_wait` 14 ms/run, `global_hold` 59 ms/run, `busy` ≈ APPEND. **Next
lever is client `rpc_send_recv_shard`, not more server lock drops.**
Open perf: N pollers, streaming-write barrier (ewrite),
PUT_CHUNK still takes the global lock for `export_acquire`.
Flush O(table) encode+blake3 **gated Aug 30/31** (last-blob memcmp +
incremental serialize; ecopy 9-way **209→177 files/s** vs Aug 29
**84→37**; posix 196/201). Raise `shard_bits` only after that. RDMA
data+control **landed Aug 30** (poller re-arm; peer-pool QP; default auto).
Auto first inode RPC after mount **gated** (`mkdir` + solo posix 196/201
0 EFS bugs, `results/posix/20260830-154329`). Open
correctness: cross-client byte-range locks (`peer_fcntl_range_conflict`),
load-dependent posix2 flakes. Open client rewrite: low-level FUSE API.
Open product feature: **expiry dates** (export sunset + per-inode scratch
TTL) — see that section; not started. Open product feature: **server-side
`.stats` / `.find` refresh** (not user `cat`; lag + time thresholds) —
see that section; not started. Open test harnesses: Layer 4 fault
injection, fsck, hot-dir 300k. Each step below is annotated
**DONE / PARTIAL / NOT DONE**.

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

## Phase M — carve the monolith FIRST (the dev-cycle lever)

**This is the next work, ahead of every other forward-looking item in this
document.** The architecture migration ([architecture.md](architecture.md)
§10) lands the simulator, the ordered KV, and Raft as new components. They
must be **born modular** ([architecture.md](architecture.md) §5.14). But the
carve-up is also the dev-cycle lever in its own right: it is
**behavior-preserving refactor, gated by the existing suites**, and it pays
off on the *current* code before a single Raft line is written.

**Why first.** Dev-cycle time is the project's bottleneck, and four files
hold ~45% of the 36.5k-line tree — `metadata.c` (6042 lines), `efs_fuse.c`
(3720), `meta_server.c` (3654), `handler.c` (3648). Every change today loads
a whole 3–6k-line file into context and every test links the world. Worse,
migration step 1 (the simulator) *requires* a pure state machine behind
transport/storage interfaces — which is exactly what this phase extracts.
Doing Raft first inside the monolith would only create the next 6000-line
file. The carve-up needs no new design and no new test infrastructure, so it
is the only forward work that is both immediately useful and a hard
prerequisite for everything after it.

**Target boundaries** (architecture.md §5.14): `raft/ kv/ meta/ wire/ data/
client/`. Depend on interface **headers**, never another module's `.c`. State
machines are pure: no globals, no inline I/O — all I/O behind the
transport/storage interfaces.

**Steps — each behavior-preserving, each gated, in order:**

1. **DONE (`wire/` — the protocol boundary).** `src/wire/wire.c` +
   `include/efs/wire.h`: versioned encode/decode only (length-prefixed frame
   + identity memcpy of the existing C structs — that *is* the current
   encoding; no new serializer). I/O stays in `src/common/protocol.c`.
   Unit test: `tests/test_wire` (frame round-trips + pack/unpack of every
   `struct efs_msg_*`). Gate: on-node `make` + `test_wire` + solo posix
   `results/posix/20260902-050828` **196/201, 0 EFS bugs**. No wipe.
2. **DONE (`data/` + the two interfaces).** `include/efs/store.h` +
   `include/efs/transport.h` are the vtables the simulator will implement
   (mem disk + message queue). `src/data/`: EC moved from `src/common/erasure.c`;
   `store_mem.c` (in-memory fragment store); `transport_loop.c` (paired queues);
   `transport_conn.c` (adapter over existing `efs_conn_send/recv`, symbols
   stay in `protocol.c`). NVMe `server_*_fragment*` is unchanged — handler
   dispatch through the store vtable is step 4. Unit test: `tests/test_data`
   (mem CRUD, loop send/recv, EC through the store, socketpair conn adapter).
   Gate: on-node `test_data` + `test_erasure` + `test_meta_v6` +
   `test_integration` / `test_directio` / `test_rejoin` / `test_drop_chunks`
   OK. Solo posix `results/posix/20260902-082249` **196/201, 0 EFS bugs**
   (TCP remount; efsd not restarted — production fragment I/O unchanged).
   Pre-existing flakes: `test_quota`, `test_migrate`. No wipe.
3. **DONE (`meta/` + `kv/` seam).** Table implementation moved
   `src/common/metadata.c` → `src/meta/metadata.c` (same code, no sockets/
   FUSE in the file). `include/efs/kv.h` + `src/kv/kv_mem.c`: ordered
   put/get/del/scan — the persist interface serialize/flush will call once
   step 4 wires it; production still uses the EFSM blob. Unit tests:
   `tests/test_kv` (ordered mem KV) + existing `test_meta_v6` (table ops,
   no cluster). File still ~6k lines — split by responsibility is follow-on.
   Gate: on-node `test_kv` + `test_meta_v6` OK; solo posix
   `results/posix/20260902-082735` **196/201, 0 EFS bugs**. No wipe.
4. **DONE (`server/` — store dispatch on GET/PUT).** `src/server/store_nvme.c`
   binds the existing `server_*_fragment*` I/O to `efs/store.h`; handler
   GET_CHUNK / PUT_CHUNK go through `efs_store_get` / `efs_store_put` (1:1,
   already-acquired export). Opcode switch and metadata RPCs stay in
   `handler.c`; `meta_server.c` flush/rebuild is not yet a kv driver
   (follow-on — do not invent a second persist path). Gate: `test_integration`
   OK; solo posix `results/posix/20260902-114722` **196/201, 0 EFS bugs**;
   cluster 4 up Heal idle gen=27. No wipe.
5. **DONE (`client/` — existing adapter).** `src/client/` already splits
   path ops (`ops.c`), data (`read.c`/`write.c`), and RPCs (`inode_rpc.c`);
   `efs_fuse.c` is the FUSE translation layer. Remaining inlines (dcache
   checks in getattr, rpc_readdir in readdir) are follow-on splits of the
   3720-line file, not a new design. No behavior change this step. Gate:
   step-4 posix `results/posix/20260902-114722` **196/201, 0 EFS bugs**.

**Phase M complete.** §10 steps 1–3 are in: simulator, op-ID / I16 window,
ordered KV applied state (`kv_key` + `meta_apply` + atomic `efs_kv_batch`).
The simulator metadata path is the KV SM; production `efsd` still uses the
in-memory table until step 4 (Raft apply). Next is step 4. Do not build
Raft as new monolith code.

**Rules while carving:** no behavior change within a step; no new features
mixed in; a file that crosses ~1000 lines splits by responsibility; every
step keeps `make test` + solo posix green on the live cluster.

**Gates (per step):** `make test`; solo posix at the 196/201-class (0 EFS
bugs); the valgrind leak gate for anything touching ownership. **No cluster
wipe needed** — these are refactors on the live table.

**Milestone:** each module builds and unit-tests in isolation in ms; a change
to one module rebuilds only its dependents; the same compiled `meta/` state
machine links into both `efsd` and a simulator harness stub. Only then does
migration step 1 (the simulator) start — against interfaces that already
exist.

---

## Phase 2 — Server-owned metadata

**Goal:** clients stop holding and flushing the full table. Metadata mutations
become RPCs to the server that owns them; the client keeps only a read cache.

Today `efs-fuse` mutates a local table copy and periodically serializes +
replicates the *entire* blob through a cluster-wide election. That is the
scaling killer: every client pays O(table) RAM and O(table) flush.

**Work:** *(status Aug 27)*

1. **DONE (2b, d7d2c7c).** Route FUSE metadata mutations (`create`/`mkdir`/`unlink`/`rename`/
   `setattr`/`link`) through the `inode_rpc.c` path instead of local-table
   mutation + blob flush.
2. **DONE (2a cffb138 + 2b + Aug 26 2PC root commit).** Servers become the
   metadata writers: own the authoritative in-memory table,
   apply RPC ops, and handle persistence + server-to-server replication
   internally (the election/blob-flush becomes a server concern, off the
   client critical path). *Aug 26: client-side metadata write path fully
   DELETED; server is the sole writer; PUT_META is 2PC PREPARE+META_COMMIT.*
3. Client keeps an invalidation-based **read cache** for lookup/readdir/getattr
   (not the authoritative copy), so read-heavy workloads stay fast.
   **Superseded (Aug 24):** no client metadata cache — immediate cross-client
   visibility (posix2) forbids TTL/negative caching, so the metadata op storm
   is fixed by *round-trip elimination* instead (whole-path `LOOKUP_PATH`,
   `fi->fh` fast paths). See **Performance next — metadata op storm**.
   *Aug 27 update: the client local table is now authoritative-ONLY for dirs
   this client created + dirty files; every clean-file lookup does
   LOOKUP+GETATTR (created-set short-circuit removed, 5525d49 — it broke
   posix2 cross-client visibility).*
4. **DONE.** Data path (chunk PUT/GET) is unchanged — it already scales and stays
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
   `efs-s3` bits=3 (Aug 24). *Aug 25: `mkfs` now defaults to bits=3
   (`EFS_DEFAULT_SHARD_BITS=3`); live `efs-test` is bits=3, bits=0 is not a
   product mode.*
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
*Aug 25: live `efs-test` is now bits=3 by default (mkfs stamps it).*

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

### Item 0 — the design rule (documentation only, do first) — **DONE (Aug 24)**

Every metadata operation is one of three classes. When adding or touching
an op, record its class in a comment and handle it accordingly:

| Class | Rule | Existing examples |
|---|---|---|
| **Commutative** | apply without ordering; max/newer wins | REPORT size grow-only + mtime newer-only (`EFS_MSG_REPORT_CHUNKS` handler), rollup counters |
| **Independent** | shard-local commit, no cross-shard talk | create/unlink inside one dir, disjoint-chunk writes |
| **Conflicting** | owner-serialized RPC + dual-apply | rename, unlink-vs-open (HOLD), O_APPEND reserve, flock |

If a new op does not obviously fit Commutative or Independent, it is
Conflicting. Never "optimize" a Conflicting op into a lock-free one.

### Item 1 — extent-sharded chunk metadata — **DONE (Aug 24 night, all 4 steps + gates)**

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

### Item 2 — threshold-based hot-directory spread — **DONE (Aug 24 night, steps 1–6; 300k stress gate still to write)**

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

### Item 3 — (optional, orthogonal) replica-3 metadata pages — **NOT DONE (left optional / not in this cut)**

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

**Next levers, in order:** *(status Aug 30)*

1. **Partition `g_server->lock`. — PARTIAL (inode RPCs gated Aug 30; PUT still global).**
   Hot metadata ops (CREATE/LOOKUP/GETATTR/REPORT/APPEND/UNLINK/SETATTR+SIZE/
   GETCHUNKS/READDIR/LOOKUP_PATH) drop the global lock and run on shard
   locks. LINK/RENAME/HOLD still `lock_all`; after 3× 9×4, `all_hold_us` is
   5.6–17 ms for the whole run (~1–2 µs/call) so they are **not** the
   remaining timeout. LOCK-PROF 9×4 `20260830-160943`: `1_wait_us=14ms`,
   `global_hold_us=59ms`, `global_wait_us=2.85s` (28 µs/call), `busy` ≈
   APPEND; 0 primary `meta-ensure` rebuilds. Saturation is **H3
   client/wire RTT**, not shard mutex, not residual `s->lock` I/O, not
   BUSY backoff. Do **not** spend another cut dropping LINK/RENAME or
   `ensure_shard_ready`. Remaining of *this* lever:
   every `PUT_CHUNK` still takes the global lock just to `export_acquire`.
   Split export-lookup / inflight from table mutate so data-path PUTs
   never wait on a report or flush. Expected for the write ceiling: most
   of the 870 µs → 3.9 ms inflation goes away.
   *Aug 27 suspected this lock for posixstress saturation; Aug 30 measured
   that the metadata half is gone and saturation is not `lock_all`.*
2. **N recv CQs / pollers. — NOT DONE.** One shared CQ + one poller harvests every
   incoming PUT. `ibv_poll_cq` is 30% of server CPU under 9 writers —
   not yet saturated, but will be as (1) raises completion rate. One
   poller (or CQ) per NIC / per NUMA / per N conns.
3. **Phase 3 data-path sharding (`bits>0` on the live write path). — DONE (Aug 23–25).**
   The real fix: `REPORT_CHUNKS` and table apply become per-shard-owner,
   so 9 clients no longer serialize on one primary. *Landed: per-shard
   REPORT + create round-robin + extent sharding (Item 1). bits=3 is the
   live default. Residual multi-write gap is extras catchup (fixed Aug 24)
   + per-chunk server latency under contention (lever 1).*
4. **Table-growth cost. — NOT DONE.** `set_chunk` / REPORT apply get more expensive
   as the live table grows (the silent 15–40% drop). Hash/index the
   chunk table or evict cold mappings (Phase 3 on-demand LRU already
   exists for *shards*; bits=0 still holds the whole table).
5. **Client CPU (lower priority). — NOT DONE.** At 5.6 GB/s single, blake3 + memmove
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

**Levers, in order (no caches — see note):** *(status Aug 27)*

1. **`efs-bench --meta` + baseline FIRST. — DONE.** New metadata mode
   (mkdir/create/stat/setattr/readdir/unlink phases, ops/s per phase,
   multi-worker, no caching in the bench) driving the same client op
   functions FUSE uses; `tests/run_tests.sh meta`, results in
   `results/meta/`. Baseline on the current build before any fix; the
   bench stays as the permanent metadata regression gate.
2. **Client-only round-trip cuts: — PARTIAL.** `fi->fh` fast paths in
   release/getattr/utimens/chmod/truncate, single SETATTR for
   atime+mtime, leaf-only GETATTR in the walk, fuse `check_search_path`
   into the lookup walk (halves non-root cost).
   *Aug 27: the created-set removal (5525d49) went the OTHER way for
   correctness — every clean-file lookup is now LOOKUP+GETATTR. Reclaiming
   those RTTs without re-introducing staleness is open.*
3. **`LOOKUP_PATH` RPC** (opcodes 79/80) — **DONE.** client sends the whole path;
   the server walks all components under one lock hold (dirs never leave
   the parent-shard chain, so one server resolves everything); reply
   carries ancestor mode/uid/gid so `check_search_path` costs 0 extra
   RPCs; symlink/deep-path statuses fall back to today's walk. Any path
   resolve becomes 1–2 RPCs regardless of depth. Full-cluster
   rebuild+restart (build-id gate).
4. **POSIX parent-dir mtime/ctime bumps** on create/unlink/rename/link — **NOT DONE.**
   a known POSIX gap, and the per-dir validator if a gen-checked name
   cache is ever revisited.
5. **Flush skip-clean + incr serialize — DONE (Aug 30/31).**
   `server_flush_fragmented_meta_locked` caches the last committed blob
   and memcmps pages (skip encode+blake3 on match). `serialize_dirty`
   packs only dirty compact slots / dentries when `!flush_full`. Gate:
   `results/ecopy/20260830-flush-incr3` 9-way ImageNet
   184|209|196|186|184|177 files/s (was 84→37); posix 196/201
   `results/posix/20260830-174926` and `20260831-030124`. Residual
   9-way CREATE EIO (~0.06% files) is joiner catchup/GC, not the
   O(table) hash. Next: RAM+unlink remeasure, then bits>3.

**Note:** kernel entry/attr timeouts stay 0; no TTL/negative/attr caches —
immediate cross-client visibility (posix2) is a hard requirement, and
caches are ruled out as the fix. A true 10k files/s needs kernel dentry
caching and is explicitly **not** the target.

**Milestone:** meta-bench walk-dominated phases (stat, setattr) ≥ 5–10×
baseline, create/unlink ≥ 2–3×, no phase regresses; posix_suite + posix2
stay 0-EFS-bug on bits=3 (current: 201 + 63; re-run XFS baseline after
new names); ecopy defaults improve several×.

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

**Levers, in order:** *(status Aug 27: all NOT DONE — ewrite still ~49 MiB/s/stream vs NFS ~600)*

1. **Multi-chunk `try_patch`: — NOT DONE (the lever).** split >128 KiB writes into per-chunk
   patches (full-chunk pieces `have_base=0` fully-dirty, no GET);
   `write()` ACKs after the patch for *all* sizes — the ≤128 KiB path
   already has exactly these semantics. Bounded writeback (2 GiB soft /
   4 GiB hard dirty-bytes) stays the only throttle; durability still
   lands at flush/fsync/close (POSIX). Not a cache.
2. **Flush throughput: — NOT DONE.** reclaim threads 4 → 16 and/or pipeline
   `dcache_flush_slot` PUTs through the put pool (`EFS_WRITE_PIPELINE`=32)
   so async flush sustains the stream rate.
3. **First-write amplifiers: — NOT DONE.** bigger chunk-table growth strides,
   dirty-set batching, per-ino dirty list so close stops scanning 0..nci.

**Milestone:** `tests/run_tests.sh ewrite` ≥ NFS parity (~1.2 GiB/s agg
at 2 jobs; baseline run 20260824-140120 = 98.5/152/198/203 MiB/s at
2/4/8/16), stretch toward the fio ~5 GiB/s single-client ceiling at 8–16
jobs; posix write-path tests + mc_stress green.

---

## Performance next — all data traffic via RDMA

*Status Aug 30: client PUT/GET **and** inode/REPORT control (except unbounded
GET_META / GET_META_ROOT) ride RDMA. Shared-CQ poller now arms after drain
(the old arm-then-spin-then-`get_cq_event` loop dropped the next CQE after a
quiet gap — that was the Aug 25 "first SEND after upgrade" hang). Server↔server
peer pool upgrades the same way, so verify-heal / migrate / meta-page
GET/PUT_CHUNK use the QP when the HCA is usable. Default `EFS_TRANSPORT` is
auto (try RDMA, TCP fallback). `EFS_TRANSPORT=tcp` still forces TCP.
**First inode RPC after mount gated Aug 30:** `stat` of the root is still
local (GET_META is one-shot TCP). First `mkdir`/`ls` upgrades the pool
conn and the CREATE/READDIR frame rides RDMA. Shared wait is
`efs_conn_wait_request` (handler + `test_rdma_xprt` share it; the test
sleeps 500ms then sends a CREATE-sized frame). Poller harvests after
comp-channel POLLIN instead of ack-and-loop. Solo posix auto RDMA
`results/posix/20260830-154329`: **196/201, 0 EFS bugs.** `EFS_RDMA_FIRST=1`
prints the old six-site trace. Do not quote 9×4 posixstress as an RDMA
correctness gate (still ~95% 15s timeouts = saturation).*

Client chunk PUT/GET is RDMA (RC QP after `RDMA_SETUP`). Payload that is a
chunk or EC fragment uses the same path, including traffic that used to ride
the TCP peer pool:

- File-data **verify-heal** (`src/server/verify.c`)
- **Drain / migrate** and **node-left orphan heal** (`src/server/migrate.c`)
- **Meta-page** GET/PUT_CHUNK during CoW flush, catchup rebuild, and
  meta-heal (`src/server/meta_server.c`)

Control that must stay TCP: `GET_META` / `GET_META_ROOT` (unbounded replies).
HELLO/heartbeat/PUT_META go RDMA when they fit the pool buffer; oversized
PUT_META still uses the TCP side-channel.

---

## Phase 4 — Slim the in-memory inode

**Goal:** raise per-shard density and cut RAM further.

`struct efs_inode` is ~384 B, dominated by `name[EFS_MAX_NAME]` (256 B inline)
and 80 B of directory rollups.

**Work:** *(status Aug 27: NOT DONE — Phase 4 not started; depends on Phase 3)*

1. **NOT DONE.** Move `name` out of the inline struct into a separate **dentry store** (the
   v6 wire format already packs dentries separately — mirror that in memory).
2. **NOT DONE.** Make the 80 B of rollups **optional/lazy**: maintain only where the
   `.stats`/`.find` features are on. Do **not** compute them on a user `cat`
   of `.stats`/`.find` — that trigger is rejected; see **Server-side
   `.stats` / `.find` refresh** below.
3. **NOT DONE.** Re-audit field widths (`pack_off`/`pack_len` already 32-bit).

**Target:** ~96–128 B/inode in memory → 2³² × 128 B ≈ 512 GiB total, ~128 MB
per shard at 4096 shards.

**Depends on:** Phase 3 (shrinking the per-node working set only pays off once
sharding bounds how much any node holds).

**Milestone:** bytes/inode measured on a large export drops ~3×; per-shard
memory and serialize time drop accordingly.

---

## Server-side `.stats` / `.find` refresh

**NOT DONE.** Today both virtual files are **user-triggered**. `cat dir/.stats`
runs `efs_export_ensure_rollups` on the FUSE client (TTL-cached ~1 s).
`cat dir/.find/<term>` memcpy's the whole client inode table and rebuilds
`g_find_idx` on every result-cache miss (5 s). Create/unlink/flush never
touch the index. That is the wrong trigger: a monitoring loop stalls the
write path, a quiet tree never refreshes, and two clients disagree.

**Goal:** the **server** owns rollups and the `.find` name index. `cat` only
reads the last committed snapshot. No FUSE client rebuilds the index because
someone opened the magic path.

**Visibility in `.stats`.** `cat dir/.stats` must report how stale **both**
the rollups (`.stats` itself) and the `.find` name index are — not only
`as_of=` for the last rollup compute (today). Readers need the lag against
the two refresh thresholds, not a wall-clock that they have to interpret.
Required fields (names flexible):

- when each snapshot was last built (`stats_as_of=`, `find_index_as_of=`;
  `never` if that side has never run)
- object lag vs the live watermark (`stats_behind_inos=`,
  `find_behind_inos=` — current `max(ino)` minus the watermark baked into
  that snapshot)
- age (`stats_age_s=`, `find_age_s=`) so a rename-only tree that does not
  bump `next_ino` is still obviously stale
- the configured thresholds **N** and **T**, so a monitor can tell “behind
  but within budget” from “refresh overdue”

`.find` query output stays a path list; staleness lives on `.stats`.

**Watermark.** The server always knows how many objects exist, or a cheap
upper bound that moves when objects are created. Candidate: per-export
`next_ino` / `max(ino)` (holes from unlink mean this is not a live count —
if a true live count is needed, maintain it on create/unlink, do not scan).
The index/rollup generation records the watermark it was built at.

**Triggers (either fires a refresh; both are required):**

1. **Object lag.** Last built watermark is more than **N** behind current
   `max(ino)` (or live object count). A create burst must catch the index
   up without waiting for a timer. N is tunable; start large enough that a
   quiet tree is not constantly hashing, small enough that `.find` is not
   tens of thousands of names behind.
2. **Time lag.** Last successful refresh is older than **T** seconds even
   if the watermark has not moved (renames, unlinks, and setattr do not
   bump `next_ino`). A tree that only churns names still converges.

Refresh is a **server** thread (owner of the table / shard, not the FUSE
client, not under `g_server->lock` for the whole walk). Same yield/batch
discipline as `REPORT_CHUNKS` / the expiry sweeper. Clients learn the new
snapshot the same way they learn any other metadata (commit/catchup), not
by local table walk on `cat`.

**Rejected:** rebuild-on-`cat`; client-local index as the source of truth;
full-table rebuild on every create (threshold 1 exists so we batch).

**Depends on:** Phase 3 sharding (per-shard owner can refresh its own
names; do not funnel a cluster-wide index through shard 0). Phase 4 dentry
store, if landed first, is what the index should scan.

**Gates:** posix `virt_stats_readable` / `virt_find_query` still pass with
no client-side rebuild in the `cat` path; `.stats` includes build time,
object lag, and age for **both** rollups and the `.find` index; a create
storm of >N files updates without any `.find` read; a rename-only idle
tree updates within T.

---

## Client architecture — low-level (inode-based) FUSE API

**Goal:** replace the high-level (path-based) `fuse_operations` API with the
low-level (inode-based) API. This is a **client-glue rewrite, not a scaling
phase** — it does not raise the inode cap. It eliminates two known live issue
classes that are *both* high-level-API limitations, and removes a redundant
path-resolution layer. Consistent with the **stay-on-FUSE** decision (see
Cross-cutting concerns): this is the way to get kernel-client semantics
without a kernel module.

**Why — the two correctness wins:**

1. **Silly-rename / `.fuse_hidden` (root-caused Aug 26).** The high-level API
   silly-renames an unlinked-while-open file to `.fuse_hidden<hex>` (libfuse
   `hide_node`, in `fuse_lib_unlink` / `fuse_lib_rename`); we currently hide
   it in `efs_fuse_readdir`. Low-level `unlink` removes the name and keeps the
   inode alive via the kernel's reference until `forget` — no artifact, no
   filter. (`hard_remove` is NOT the answer: it makes read/write/fsync/fstat
   on the still-open fd fail ENOENT, breaking unlink-while-open.)
2. **Hardlink flakiness (known live issue).** The high-level API has no
   daemon-controlled `.lookup`, so the kernel intermittently resolves a link's
   source path from a stale cached dentry without calling the daemon
   (`efs_fuse_link` never invoked). Low-level is inode-native (one inode, many
   names) — hardlinks work by construction.

**Secondary wins:**

3. **No double path resolution.** Today every op pays libfuse's tree walk
   (under a lock) to rebuild the path string, then efs-fuse's
   `lookup_path_fuse` walks its own tables. Low-level sends
   `(parent_nodeid, name)` and efs does ONE lookup in its own metadata.
4. **Explicit inode lifecycle.** `lookup`/`forget` give precise refcounting —
   cleaner than HOLD-on-open/release, and tells efs exactly when the kernel
   drops an inode (client cache eviction).
5. **Finer cache control.** Per-entry `entry_timeout`/`attr_timeout`/negative
   caching per inode (today blanket 0). Useful if a relaxed-coherence mount
   mode is ever added.

**Work:** *(status Aug 27: NOT DONE — large rewrite, not started. The
`.fuse_hidden` artifact is currently handled by the readdir filter
(efs_fuse.c); the hardlink flake is latent. Land this alone on a fresh
cluster when scheduled.)*

1. **NOT DONE.** Implement `lookup` (parent_ino + name → ino + attr + timeouts) and
   `forget` (inode refcount dec; evict from the client cache at 0). The
   daemon owns the nodeid space — map nodeid ↔ efs ino (can be 1:1).
2. **NOT DONE.** Migrate every handler in `efs_ops` (`src/client/efs_fuse.c`) from
   path-based to inode-based: open/read/write/flush/release/fsync take
   nodeid + `fi->fh`; create/mkdir/unlink/rename/link/symlink take
   (parent_nodeid, name). Most handlers already resolve to an ino
   internally — the change is dropping the path round-trip, not new logic.
3. **NOT DONE.** Rework path-based conveniences: `.find`/`.stats` virtual files and
   daemon-side ancestor access checks must be re-expressed as
   (parent_ino, name) + walking efs's own parent pointers.
4. **NOT DONE.** Keep the data path (chunk PUT/GET, dcache, REPORT) untouched — it is
   already ino-keyed.

**Risks / watch-items:**

- nodeid/refcount bugs leak or prematurely evict inodes — cover with the
  valgrind leak gate (`tests/run_tests.sh leaks`) plus a lookup/forget
  balance check.
- unlink-while-open and rename-over-open must keep working (posix
  `unlink_open_file`, `unlink_open_then_recreate`, rename-over tests) — these
  are exactly the cases the high-level silly-rename used to paper over.
- Large, high-touch rewrite: land it alone on a fresh cluster, not mixed
  with other changes; full gate behind it.

**Gates:** `make test`; posix_suite 191 + posix2 63 with 0 EFS bugs on
bits=3; mc_stress VERIFY-OK; `tests/run_tests.sh leaks` clean; unlink-storm
9×4000; `.fuse_hidden` never appears (the suite's strict `listdir == []`
already catches it) and a `ln` hardlink storm passes.

**Milestone:** efs-fuse mounts on the low-level API; the two issue classes
above are gone by construction; no perf regression on the single-client
posix suite or sw-1m.

---

## Cross-cutting concerns

- **Unprivileged on-demand clusters (goal, Aug 24) — PARTIAL (no-root already works today; the 4 missing items below are NOT DONE):** users must be able to
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
  server-driven invalidation / lease RPCs) — not a kernel module. Separately,
  the high-level → **low-level (inode-based) FUSE API** migration (its own
  section above) fixes the silly-rename / hardlink gaps without a kernel
  module.

- **`efs-mgmt` kick-clients (ops) — NOT DONE (not started; no mount census today):** a management call that
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

- **Expiry dates — NOT DONE:** export sunset + per-inode `expire_at` (scratch
  TTL). See **Expiry dates** below. Not a POSIX timestamp; server clock;
  lazy LOOKUP + per-shard sweeper. Do not implement as atime purge or
  client-side delete.

- **readdir/lookup across shards — DONE (Phase 3)**: the name index is per-export today; with
  sharding, dentries live in the parent's shard. `efs_export_foreach_child`
  and the name index need a shard-aware form.
- **Rollups/tree stats — DONE (Phase 3)**: `.stats`/`.find` rollups must aggregate across shard
  boundaries (or be computed per-shard and merged).
- **Backward compatibility / migration — DONE (Phase 3)**: every phase must load existing v5/v6
  single-blob exports (`shard_count = 1`) and upgrade in place. *Aug 25:
  mkfs defaults to bits=3; bits=0 is not a product mode.*
- **Failure tolerance — DONE (Phase 3)**: per-shard dual-slot paging and the rebuild/repair
  path (`server_rebuild_export_from_pages`, meta-repair) extend per shard.
- **Testing**: each phase keeps `make test` green plus a multi-shard smoke
  (create/lookup/rename/readdir across shard boundaries, server restart
  rebuild, concurrent writers on disjoint shards). Live gates: posix_suite
  **201** + posix2 63 (0 EFS bugs vs XFS; re-run baseline after new names),
  mc_stress, unlink-storm, `tests/run_tests.sh perf` / `meta` / `ewrite`.
  **Tests still to write** are listed in the next section — do not dump
  crash/EC/partition cases into `posix_suite.py`.

## Testing still to write (Aug 25)

POSIX layers 1–3 are in `tests/posix/posix_suite.py` (**201** — Aug 26 added
seeded-random content + strange-name/depth tests) and
`posix_2client.py` (63). Catalog: `tests/posix/TEST_CATALOG.txt`.
Those are syscall + peer-visibility + same-file races. They do **not**
cover crash, EC, partition, or several product-specific checks. XFS PASS
is not the oracle for any of the items below.

*Aug 30 gate (TCP, per-op lock drop): solo posix 195/201, 1 flake
`dir_rename_over_existing`. 3× 9×4 still ~95% `timeout after 15s` on
saturated hosts; clean 196/0 is now common on hosts that keep up.
`lock_all` hold is not the ceiling. LOCK-PROF classify 9×4
`20260830-160943` (36 tsv, 6× 196/0): **H3** — next is client RTT, not
more server lock drops. Auto RDMA first-inode **closed**
(solo posix `20260830-154329` 196/201 0 EFS bugs). posix2 remaining:
`peer_fcntl_range_conflict` (byte-range lockf is per-client in-memory,
NOT coordinated cross-client).*

### Already written — do not reinvent

`make test`; posix + posix2; `mc_stress` (2 clients + kill -9 shard owner
+ restart + cold mount); unlink-storm 9×4000; `shard_spread_probe` /
`hotdir_spread_probe`; `phase3_extra_restart`; perf / meta / ewrite
sweeps.

### Layer 4 — fault-injection harness (new, not posix_suite) — **NOT DONE (harness not written; cases 1–7 below all open)**

Need `tests/run_tests.sh crash` (or `fault`) that can kill `efsd` /
`efs-fuse`, partition a node, and inject on-disk checksum errors. Cases:

1. **Crash after fsync** — `fsync` returns; kill client and/or primary;
   remount (same and other host) sees the durable bytes and size.
2. **Kill between EC / meta stages** — mid-PUT (1 of 3 fragments acked),
   mid-CoW page write, mid-extras commit; remount / peer must not see a
   torn page or a half-applied dentry (or must recover via the documented
   reconcile rule).
3. **Degraded rebuild** — one node down; reads reconstruct from 2/3;
   writes still quorum; node returns; heal fills the missing fragments;
   cold read matches.
4. **Silent checksum repair** — corrupt one fragment on disk; next read
   skips or repairs it; file content intact; no zero-fill of a live page.
5. **Network partition / fencing** — split one server or one client; no
   dual-writer on the same shard; leftover `efs-fuse` must not
   REPORT/flush onto a fresh `mkfs` (the 232 MB leftover-client adopt).
6. **Lock-holder crash** — holder dies; lease expires; new holder wins;
   a late write from the dead holder is fenced.
7. **Kill mid-rename on a spread dir** — Item 2 risk: duplicate dentry;
   hash-shard row wins, parent-shard row loses. Cover after the 300k
   hot-dir stress exists.

Layer 4 is efs-specific. Do not require an XFS PASS row.

### POSIX / VFS cases still missing from the suites — **NOT DONE (cases 1–5 below all open)**

1. **Second uid** — other-user / other-group / sticky-cannot-delete-others.
   Needs two uids on the mount (root-only suite no-ops most `perm_*`).
2. **mmap `MAP_SHARED` across clients** — document as unsupported, or add
   a posix2 case if the product claims it. Local mmap is already in
   posix_suite.
3. **1 GiB pwrite** — posix2 races use 8 MiB sparse; a large write is a
   different REPORT / size / layout path.
4. **100-client nlink / hardlink storm** — scale, not a 2-client race.
5. **Parent-dir mtime/ctime** on create/unlink/rename/link — product gap
   listed under **Performance next — metadata op storm**; write the tests
   when the bumps land (local + peer).

### Suite determinism + taxonomy (Aug 26 external review) — **PARTIAL (seeded-random content + strange names/depth DONE Aug 26; the 5 determinism/taxonomy items below NOT DONE)**

The four-layer model, contract tags, and `("ab", (fn_a, fn_b))` mode are
validated as the right direction. The race *coverage* the review asks for is
already present (overlap-pwrite, high/low extend, create-excl, rename/unlink,
open-unlink-recreate, hardlink/nlink, cross-client append + locks — see
TEST_CATALOG PART 2 Layer 3). The highest-value remaining work is making those
races **deterministic** — today they create concurrency but do not force the
collision:

1. **Force the collision, don't fork-and-pray.** The posix2 `ab` runner
   (`posix_2client.py`) and the local threaded tests use bare `thread.start()`
   + `sleep()` hints, so whether the two ops actually overlap is
   timing-dependent. Add a `threading.Barrier` (or event pairs) so the
   critical ops start simultaneously / in a forced order:
   - `concurrent_extend_high_then_low`: the 8 MiB write must **commit**
     (fsync + event) before the 1 MiB write starts — the current
     `sleep(0.05)` only delays the low thread's *start*, not the high write's
     size commit, so the stale-size bug can slip through.
   - posix2 `ab` runner: barrier-start both sides so `peer_overlap_*`,
     `peer_creat_excl_race`, `peer_rename_*` actually collide.
2. **`trunc_zero_then_high_pwrite` is too weak.** The original data is 9 bytes
   (sub-chunk), so it never exercises multi-chunk stale resurrection. Make the
   original 512 KiB of patterned data, `truncate(0)`, `pwrite` at 1 MiB, then
   verify zeros straddling the 128 KiB boundaries: offsets 0, 127 KiB,
   128 KiB, 129 KiB, 255 KiB, 256 KiB, 511 KiB. Add a variant where the new
   write lands *inside* a formerly-allocated chunk.
3. **Cross-client mtime monotonic (new test).** `mtime_monotonic_many_writes`
   is local rapid writes (one clock). Add the authority-change case: rapid
   writes to one file from client A (shard X) and client B (shard Y);
   `mtime_ns` must not regress when the metadata authority / clock differs.
   (`peer_mtime_no_regress` is sequential A→B; this is the concurrent version.)
4. **Taxonomy: split layer 3 into 3a/3b.** Intra-client concurrency
   (threads/processes on ONE mount) vs inter-client concurrency (independent
   mounts) exercise different paths — a FUSE mount can serialize/coalesce
   intra-mount ops while two clients go through separate cache/lease/RPC
   paths. Update `TEST_CATALOG.txt` (the "Concurrent / race" line).
5. **Propagate the annotations.** Today only the 3 newest tests carry
   "(layer N, contract)" tags. Tag every test with (layer, contract, feature
   area) so the catalog becomes a requirements matrix — this is what lets a
   failure be classified as a correctness blocker vs an intentional semantic
   choice (cf. the 5 XFS target-better quirks).

### Invariant / ops harnesses still missing — **NOT DONE (items 1–4 below all open)**

1. **`efs fsck --verify-only`** (or equivalent) after randomized load and
   after every Layer 4 case — inode/dentry/chunk/EC invariants, not
   another syscall.
2. **Hot-dir spread stress** (Item 2 gate, not written): 300k creates in
   one dir on bits=3, readdir count == create count, peer sees them
   without remount, rename churn clean, rmdir after empty.
3. **Leftover-client fence** once `efs-mgmt disconnect-clients` exists —
   wipe+mkfs with a live mount on another host must fail closed.
4. **Stuck-catchup joiner** — fcstor004-class gen lag must restart-or-fail
   the gate, not timeout-skip.

### Contract

- Append `@test` functions for syscall cases; runner auto-registers.
- Concurrent peer cases use `("ab", (fn_a, fn_b))`.
- Re-run the XFS baseline after adding posix / posix2 names so
  `compare.py` sees them.
- Crash/EC/partition stay out of `posix_suite.py`.

## Expiry dates (export sunset + per-inode TTL) — NOT DONE

**Goal:** an object or an export can carry an **absolute expiry instant**. After
that instant, new lookups do not see it, and the shard owner reclaims the
inode, dentries, and chunks. This is a **product feature, not a scaling
phase**. It does not raise the inode cap. Do not start it until the live
posix/posix2 gates are green on the bits=3 tree you will ship it on.

Scratch clusters (Engaging-style) want “this run dies on DATE” and “files
under `/tmp/...` last N days.” There is no POSIX `st_expire`. Inventing a
fourth timestamp that `utimens`/`rsync` can clobber is how you lose data.
Expiry is a **separate inode field**, set only by an explicit API, compared
only to **server** time.

### Semantics

- `expire_at` is Unix seconds (`time_t` / `CLOCK_REALTIME`). **0 = never.**
- Comparison is `server_now >= expire_at`. Not client `utime`, not `atime`
  (atime is never bumped on read today — `tmpwatch -a` would never fire).
- Expired means **already unlinked** for any new namei: LOOKUP / GETATTR /
  OPEN / CREATE-under return `ENOENT` (or `ESTALE` only if an fd is still
  held — prefer `ENOENT` on the path).
- Open fds match today’s unlink-while-open (`keep_last` / HOLD): the name
  is gone; the fd still reads/writes until close; last close drops chunks.
- **Root never expires.** A nonempty directory does not expire its children
  as a side effect of the dir’s own `expire_at`. Recursive sunset is an
  admin walk that stamps children, then expires the dir when empty.
- New creates **inherit** the parent’s `expire_at` when the parent has one
  and the create did not set its own. Export default (below) is applied
  when the parent has 0.
- A late `REPORT_CHUNKS` must not resurrect an expired inode (same class as
  the post-trunc grow-only REPORT bug). Reject REPORT if the row is gone or
  `expire_at` is in the past on the owner.

### Two layers (ship in this order)

1. **Export sunset (smaller).** `efs_export_root` grows `expire_at` (0 =
   never). `efs-mgmt expire-export <name> --at SECS|--ttl DAYS` sets it.
   After the instant: creates/mkdir fail `EROFS` (or `EPERM`); lookups of
   existing names still work until layer 2 exists; `efs-mgmt` can refuse
   mount with a loud line. Persistence is a root-field bump (v8 extras
   already merge descriptors — add the u64 next to `features`, default 0
   on old roots). No sweeper, no inode scan. This is the “the allocation
   ends on DATE” knob.
2. **Per-inode expire (the real TTL).** `struct efs_inode` grows
   `uint64_t expire_at`. Compact pack / wire size bump; old rows read as 0.
   Fresh `mkfs` or an explicit table upgrade — do not silent-extend a live
   420-byte `EFS_INODE_WIRE_SIZE` in place. Phase 4 (slim inode) must keep
   this field; it is not optional rollup data.

Do not implement layer 2 as xattrs-only. `opt_xattr` is allowed to be
`EOPNOTSUPP`; LOOKUP must see expiry without a xattr get. Xattr
`user.efs.expire` (decimal Unix time, or empty to clear) is the **user
surface** that writes the inode field via a SETATTR-class RPC.

### How to implement (layer 2)

**Clock.** The inode **owner** (Phase 3 shard owner) samples
`clock_gettime(CLOCK_REALTIME)` on LOOKUP/OPEN and in the sweeper. Clients
do not decide “is it expired.” If the server clock steps backward, the
sweeper waits (do not expire early). If it steps forward, lazy LOOKUP
catches it on the next namei. Expiry is coarse (seconds), not a security
erase. Assume NTP; document that a years-wrong clock will purge or never
purge.

**RPC.** Add `EFS_MSG_INODE_SET_EXPIRE` (or a SETATTR flag that is **not**
`UTIME_*`). Owner applies `expire_at`, bumps ctime, marks `shard_dirty`.
`chmod`/`utimens`/`REPORT` must not clear or rewrite it. Inherit on
CREATE/MKDIR in the owner’s create path (parent row already loaded).

**Lazy expire (correctness).** On the owner, LOOKUP/GETATTR/OPEN of a row
with `expire_at && now >= expire_at` runs the same last-link unlink path
already used by `efs_export_unlink_name` (dentry, nlink, `INODE_DROP_CHUNKS`
fan-out, spread-dir hash-shard rule). Then return `ENOENT`. Readdir skips
expired names (and may kick lazy expire). This is the path that must work
even if the sweeper is off — otherwise a file lives forever if no scan
runs.

**Sweeper (space).** Per-shard owner thread, not the metadata primary, not
the client. Period ~30–60 s; scan the **local** shard table only (owner
already materializes it). Batch like `REPORT_CHUNKS` (yield every N inodes;
do not hold `g_server->lock` across the scan). Delete expired files first;
dirs only when `efs_export_dir_empty` (spread-aware) is true. Never scan
from a FUSE client; leftover `efs-fuse` after wipe is how we re-adopt a
232 MB table — a client-side purge would be that class of bug.

**Index.** v1 is the linear scan. At the Phase 3 sizing target (~1M
inodes/shard) a 60 s walk is acceptable if it yields. Add an expire-bucket
(day → ino list) only if the scan shows up in CPU. Do **not** put a
cluster-wide expire index on shard 0 (re-centralizes the metadata storm).

**FUSE.** `getxattr`/`setxattr`/`removexattr` for `user.efs.expire`.
`listxattr` may omit it when 0. Daemon-side `check_access` W on the inode
to set, R to read. Do not use `default_permissions`. Virtual `.expire`
files are unnecessary (`.stats`/`.find` are enough magic).

### Rejected

- **atime/mtime purge as the only mechanism.** atime is not maintained;
  mtime is user-settable (rsync, `touch`). That is a different policy
  (`efs-mgmt prune --mtime-older`) and must not be conflated with
  `expire_at`.
- **Client-local expiry.** Clients are not authoritative (Phase 2).
- **Per-chunk expire.** Chunks die with the inode; REPORT/layout stay
  inode-scoped.
- **Sub-second / lease-style expire.** Wrong tool; we already have write
  leases and HOLD. This feature is days-to-months scratch TTL.
- **MVCC “expired versions.”** CoW pages already recover crash; do not add
  a second time axis.

### Work

1. **NOT DONE.** Export-root `expire_at` + `efs-mgmt expire-export` + create
   `EROFS` after sunset. Gate: `efs-mgmt` round-trip; create fails after
   `--at` in the past; existing files still read.
2. **NOT DONE.** Inode field + pack/unpack + SET_EXPIRE RPC + inherit on
   create. Gate: `test_meta_v6` (or successor) + isolated setattr/xattr.
3. **NOT DONE.** Lazy expire on LOOKUP/OPEN/readdir; no REPORT resurrection.
   Gate: set `--at` in the past; path is `ENOENT`; open fd still reads;
   peer (posix2) does not see the name.
4. **NOT DONE.** Owner sweeper + chunk reclaim (`used` drops). Gate: create
   N files with past `expire_at`, wait one sweep, `efs-mgmt status` used
   down, cold remount empty.
5. **NOT DONE.** Tests — **not** vs XFS (`compare.py` would mark EFS-BUG).
   Add `tests/posix/expire_suite.py` or `tests/stress/expire_probe.sh`
   (efs-only). Cases: inherit; xattr set/clear; past-at lazy ENOENT;
   unlink-while-open; nonempty dir does not delete children; late REPORT
   does not revive; export sunset `EROFS`.

**Depends on:** Phase 2 (owner applies the op) and Phase 3 (sweeper is
per-shard). Fine to land on live bits=3. Does **not** depend on Phase 4 or
low-level FUSE.

**Milestone:** an unprivileged user can `setfattr -n user.efs.expire` on a
file, a peer lookup misses after that instant, and the shard owner has
dropped the chunks without a client walking the tree.

## Phase ordering rationale


Phase 2 first because sharding (Phase 3) is far cleaner when a server owns the
table rather than every client blob-flushing. Phase 4 last because it only
matters once sharding bounds the per-node working set. Phase 1 (shipped) keeps
the current system usable until 2–4 land.
