# Client cache design — architecture migration step 12

Status: **PROPOSAL, not ratified.** Where this doc and the spec disagree, the
spec ([architecture.md](architecture.md)) wins. Anything marked **DECISION**
is a design point the spec does not contain — per
[arch/START-HERE.md](arch/START-HERE.md) §4 those are stopped-and-asked, not
invented; they are collected in §7 for ratification before implementation.

Step 12 in [architecture.md §10](architecture.md): *"FUSE cache-coherence
optimization only after zero/stale-cache semantics are demonstrably correct
(data path starts as direct-I/O, §7.7)."* The project-state open follow-up
couples in the client staging table: *"client local staging table
(`g_client.export`) shrink is post-step-11 client-cache design / spec step
12."*

## 1. Problem

Three coupled caches live in the client (`efs-fuse`). All three are at
correctness-safe but unscalable settings:

1. **The staging table is unbounded.** `g_client.export` (a full
   `struct efs_export`, metadata.c) grows by one row per distinct inode this
   client has ever created, stat'ed, or looked up, plus one chunk-map entry
   per chunk this client has written or pulled. Nothing is ever evicted
   (rows survive unlink as nlink=0 ghosts; only shutdown frees the table).
   A client that walks a large namespace (`find`, `du`, rsync, ecopy verify)
   accumulates the whole tree in RAM: at the 2^32-object goal a single
   walking client would need ~512 B/row x N plus ~40 B/chunk-rec — i.e. the
   server-side 2^32 wall reappears on every client. This is the named
   follow-up.
2. **Kernel metadata caches are forced off.** `.init` sets
   `entry_timeout = attr_timeout = negative_timeout = ac_attr_timeout = 0`
   (src/client/efs_fuse.c:3273-3277). Correct — a cached dentry/attr ghosts
   a peer's unlink/write (posix2 `peer_open_rename_fd`) — but every path
   resolution is RPCs: the measured `dir_deep_nesting_beyond_64` multiplier
   (450k walks, per-RPC inflation to 63 µs, 13/16 timeouts at jobs=16) is
   this choice's price. Per-op cost is already near-optimal; only caching
   removes the factor of depth.
3. **The data path is not yet the spec's direct-I/O.** §7.7 decides
   direct-I/O ("the kernel page cache is bypassed"); today `fi->direct_io`
   is set only for `O_DIRECT` opens (efs_fuse.c:1457-1461, 2349). Buffered
   I/O rides the kernel page cache in front of the userspace dcache, with
   cross-client read visibility relying on `attr_timeout=0` + kernel
   auto-invalidation on mtime change. That works (it is the
   zero/stale-cache semantics step 12 says must be demonstrably correct
   first), but it is not the decided end state, and it caps honest write
   accounting (see .cursor/rules/efs-fio-honest.mdc).

## 2. What the spec already fixes (not open)

- §7.7: kernel metadata caches are `0` **plus explicit invalidation through
  the low-level (inode-based) FUSE API**. Today's client uses the
  high-level (path-based) API, which cannot invalidate by
  (parent_nodeid, name) — so the invalidation half of the spec's answer is
  not even expressible yet. The low-level migration is its own roadmap
  section ([scaling-roadmap.md](scaling-roadmap.md), "high-level →
  low-level FUSE API": also fixes silly-rename and the hardlink flake by
  construction) and is a hard prerequisite for Part C below.
- §7.7: `FOPEN_DIRECT_IO`, `FOPEN_PARALLEL_DIRECT_WRITES`,
  `FUSE_CAP_ASYNC_DIO`, `FUSE_CAP_PARALLEL_DIROPS`, large
  `max_write`/`max_pages`, sufficient `max_background` are requirements.
  Direct-I/O disables kernel readahead, so **the client owns an adaptive
  asynchronous prefetch pipeline** — a performance requirement, not an
  option.
- §3: immediate cross-client visibility is the default contract. Any cache
  that weakens it is an explicitly documented, opt-in deviation — never the
  default mount.
- [protocols/data.md](arch/protocols/data.md): a multi-chunk read is a
  validated collect; per-lane chunk-map windows are **prefetch, not a
  cache** — usable without revalidation only inside the read whose
  linearization interval covers them. The client's chunk-map staging must
  obey the same rule: a staged map may serve the read/write that fetched
  (or wrote) it, and is revalidated or refetched across operations.
- I24 (read/write atomicity vs. each other) governs any data-cache change.

## 3. Current state (surveyed, src/client/)

What the staging table is actually for, post-step-11:

- **Writes:** create dual-apply (`efs_export_create_with_ino` +
  `efs_export_upsert_inode`, ops.c:724-764), adopt-on-first-sight
  (`adopt_rpc_inode`, ops.c:338-404), setattr/truncate/rename/link/unlink
  dual-apply (ops.c:768-1121), chunk-map staging after PUT/GETCHUNKS
  (ops.c:190-215, write.c:1544-1557), write-path size/mtime
  (write.c:1839-1858, 2709-2715).
- **Reads:** local-first getattr for open fds (`efs_client_stat_ino`,
  ops.c:432-460 — RPC+adopt on miss already exists); lookup_walk uses RPC
  for names and overlays only size/pack from local (ops.c:585-690);
  readdir is server-paged RPC; fill_stat reads mode/nlink/size/times
  (efs_fuse.c:1046-1061); `.stats` rollups; statfs fallback scans
  `inode_count` (efs_fuse.c:2513-2519, 2551).
- **Durability path:** the dirty ino/chunk sets snapshot into
  `REPORT_CHUNKS`; the report builds its records by reading rows out of the
  staging table (write.c:595-621). A missing row is *skipped* — correct
  today only because rows never vanish except by unlink.
- **Ghosts:** unlink keeps the row at nlink=0 so an open fd can still
  `get_inode` (ops.c:1090-1097).
- **Dead weight:** the `created_*` set is write-only —
  `efs_client_ino_created_recent` has no callers since Cut 4
  (write.c:230-280, ops.c:764).
- **dcache** (write cache) is separate and already bounded: 65536 slots,
  2 GiB soft reclaim (`EFS_DCACHE_BYTES`), ~8 GiB hard. Not part of the
  staging-table problem.
- **TTL caches** (`.stats` 1 s, features 2 s, `.find` 5 s/16 slots,
  rdcache tick-LRU) are small and bounded. Not the problem.

## 4. Part A — bound the staging table (no protocol change)

**Rule: the staging table is a cache of what this client is *doing*, not a
replica of what exists.** Anything authoritative is re-fetchable: rows via
getattr RPC + adopt, chunk maps via GETCHUNKS, names via LOOKUP. So a row
may be evicted whenever no in-flight or owed work can still observe its
absence.

**Pin rules — a row (and its chunk maps) is unevictable iff any hold:**

1. its ino is in the dirty set, the publishing set (`pub_ino_keys`), or has
   dirty dcache slots — eviction would silently drop a REPORT record
   (write.c:595-621 skips missing rows) = data loss;
2. it is a ghost (nlink=0) with an open fd — open-fd stat/read must keep
   working (ops.c:1090-1097); the per-ino open-description refcount
   (`efs_open_note`/`efs_close_note`, landed with the flock fix) is the
   liveness signal;
3. it is pinned by an in-flight op (a bounded per-op pin, dropped at op
   end) — covers the create dual-apply window and rename/link/unlink
   dual-apply;
4. it has a live append reservation or lock record (ino-keyed client
   tables).

Everything else is LRU by last-touch tick. Eviction order inside a class:
chunk maps of clean closed files first (cheapest to refetch, largest
volume), then rows.

**Required rework (the survey's breakers):**

- `statfs` (efs_fuse.c:2513-2519, 2551): stop deriving used bytes / f_files
  from the local table. It already prefers node usage when set; make the
  server-reported path authoritative and drop the full-table scan.
- `efs_export_fits_page_cap` (ops.c:176): a local-table quota heuristic.
  Under a bounded table it must not treat cached rows as the universe —
  quota enforcement is server-side; the client check becomes a best-effort
  early-out on *staged* growth only.
- The `created_*` set: delete it (write-only, no readers) or fold its one
  intended use into pin rule 3. Deleting is preferred — less state.
- Bootstrap dump (efs_fuse.c:3635-3649): debug print only; make it print
  cache occupancy, not the table.
- `export_drop_zero_inodes` runs on deserialize only; live ghost reclaim is
  new (rule 2 expiry at last close).

**Cap:** `EFS_CLIENT_META_MB` (default 256 MB) over rows + chunk maps +
name arena; on pressure, evict unpinned LRU; if everything is pinned, grow
(pinned data is real work, not cache) and log once. A correct client under
memory pressure degrades to more RPCs, never to lost writes.

**Gate:** `make test`; posix + posix2 (0 EFS bugs); a single-client
namespace-walk RSS gate (scale_probe/find over a multi-million-file tree —
client RSS stays near the cap, previously grew linearly); valgrind leak
gate (eviction frees correctly).

## 5. Part B — low-level (inode-based) FUSE API

Prerequisite for Part C; specified in
[scaling-roadmap.md](scaling-roadmap.md) ("high-level → low-level FUSE API"
section, all items NOT DONE). Daemon owns nodeid ↔ ino (1:1), `lookup` /
`forget` give exact inode lifetimes (forget = the kernel's eviction notice
= the natural trigger dropping pin rule 2/3 state), every handler migrates
from path strings to (parent_nodeid, name). Silly-rename and the hardlink
flake die by construction. Large rewrite; lands alone on a fresh cluster
behind the full gate (posix 201 + posix2 63 + leaks + unlink-storm +
hardlink storm).

## 6. Part C — kernel metadata caches

**Default mount: unchanged.** `entry/attr/negative_timeout = 0` stays the
default — §3 immediate visibility is the contract, and no server-driven
invalidation or lease protocol exists in the spec. What the low-level API
(Part B) unlocks without any protocol change:

- **Self-invalidation becomes exact.** Today `fuse_invalidate_path` on the
  parent covers local mkdir/rmdir/rename; low-level adds
  `notify_inval_entry(parent, name)` / `notify_inval_inode(ino)` for every
  local mutation — removing the residual cases where this client's own
  kernel view lags its own RPCs.
- **Per-entry timeouts become expressible** (per-inode `entry_timeout` /
  `attr_timeout` in the `lookup` reply) — the mechanism, not the policy.

**DECISION C1 (policy):** is an opt-in relaxed-coherence mount mode in
scope for step 12 — `entry_timeout>0` + server-driven invalidation or lease
RPCs (the roadmap's "mdtest-class" escape hatch)? That *is* a coherence
protocol (who tracks which client caches which name; how invalidation
races an in-flight unlink) that the spec deliberately does not specify.
Proposal: **not in step 12.** Step 12 delivers the mechanism (B + exact
self-invalidation) and keeps timeouts 0 by default; a relaxed mode gets its
own spec section and review when a benchmark actually demands it. The
deep-nesting kernel multiplier is meanwhile bounded by the batched
LOOKUP_PATH walk (already landed) and by Part D's prefetch not applying to
metadata.

## 7. Part D — data path to direct-I/O + client prefetch

Spec-decided end state (§7.7), deliberately sequenced last: it changes the
hot path and its honest-measurement baseline.

1. Set `fi->direct_io = 1` unconditionally (and the FUSE capability set of
   §7.7: `PARALLEL_DIRECT_WRITES`, `ASYNC_DIO`, `PARALLEL_DIROPS`,
   `max_pages`). Buffered-I/O users keep working — direct-I/O is a
   coherence contract, not an API break; `MAP_SHARED` cross-client becomes
   the documented unsupported case, `MAP_PRIVATE` unaffected.
2. **First** land the client-owned adaptive async prefetch pipeline
   (§7.7 makes it a requirement of the direct-I/O decision): per-lane
   batched range chunk-map fetches ahead of the data window
   ([performance.md](arch/performance.md)), prefetch-not-a-cache per the
   validated-collect rule. Without it, sequential reads lockstep.
3. The userspace dcache stays the write-coalescing layer; direct-I/O moves
   the kernel out of the coherence path, it does not remove client
   write buffering (write()=durable is unchanged — REPORT still gates
   visibility).

**Gate:** the honest fio matrix (`.cursor/rules/efs-fio-honest.mdc`) —
reads must not regress (prefetch works), writes stay at the shared
ceiling; posix + posix2 + posixpersist.

## 8. Sequencing

| Part | Scope | Spec decisions needed | Gate |
|---|---|---|---|
| A — staging-table bound | client only | none (§7 ratify the pin rules) | posix, posix2, walk-RSS, leaks |
| B — low-level FUSE API | client only | none (roadmap section exists) | full suite set, alone on fresh cluster |
| C — exact self-invalidation | client only | C1: relaxed mode deferred (ratify) | posix, posix2 |
| D — direct-I/O + prefetch | client + perf contract | none (§7.7) | honest fio matrix, posixpersist |

A is the named open follow-up and the only part with zero protocol
surface; it is the first implementation slice. B is large and independent.
C-after-B is small. D is the perf-contract change and goes last.

## 9. Decisions requested

1. **Pin rules §4.1-4.4 and the statfs/quota rework** — ratify as the
   Part-A contract.
2. **C1:** confirm relaxed-coherence mount mode is *out* of step 12
   (mechanism only, timeouts stay 0 by default).
3. **Cap default:** `EFS_CLIENT_META_MB=256` per client — ratify or pick
   another number.
4. Confirm the sequencing A → B → C → D, and that B (the large rewrite) is
   wanted at all in this cycle vs. deferring to after more raft hardening.
