# Client staging table — bound it (step 12 part A)

Status: **done Sep 27.** The Sep 23 recommendation (pin rules as
written, `EFS_CLIENT_META_MB` = 256) is on the clients. The open-fd
pin covers every open fd, not only ghosts, because local getattr of
an open file does not refetch. 9-host jobs=1 is 200/201
(`results/posix/20260927-033723`). Posix 2 is 59/63. The leak gate
is clean (`results/leaks/20260927-035622`). A cold stat of 1M files
leveled at 233 MB RSS (`results/measure/20260927-w9-walk`). **D18 (Oct 1):**
the per-shard-tab floor sat above the cap, so after the row LRU the
evictor now drops whole cold tabs whose every ino passes the pin rules
(`src/client/stage_evict.c`, `evict_cold_tabs`); readers peek and do not
rebuild a dropped tab. Where this doc and the spec
([architecture.md](../../how-it-works/architecture.md)) disagree, the spec wins.

The other three parts of step 12 have landed: low-level (inode-based) FUSE
(part B), exact self-invalidation with timeouts still 0 (part C), and
`FOPEN_DIRECT_IO` plus the sequential read prefetch (part D). This is the
remaining part, and the only one with zero protocol surface.

## 1. The problem

`g_client.export` (a full `struct efs_export` in the client) grows by one row
per distinct inode this client has created, stat'ed or looked up, plus one
chunk-map entry per chunk it has written or pulled. Nothing is ever evicted:
rows survive unlink as `nlink=0` ghosts, and only shutdown frees the table. A
client that walks a large namespace (`find`, `du`, rsync) therefore
accumulates the whole tree in RAM — at the 2^32-object goal the server-side
memory wall reappears inside every client.

The write cache is *not* the problem: `dcache` is already bounded (65536
slots, 2 GiB soft reclaim via `EFS_DCACHE_BYTES`, ~8 GiB hard), and the TTL
caches (`.stats` 1 s, features 2 s, `.find` 5 s over 16 slots, rdcache
tick-LRU) are small.

## 2. What the table is still used for

- **Writes:** create dual-apply, adopt-on-first-sight (`adopt_rpc_inode`),
  setattr/truncate/rename/link/unlink dual-apply, chunk-map staging after
  PUT/GETCHUNKS, write-path size/mtime.
- **Reads:** local-first getattr for open fds, the size/pack overlay in
  `lookup_walk`, `fill_stat`, `.stats` rollups, and a `statfs` fallback that
  scans `inode_count`.
- **Durability:** the dirty ino/chunk sets snapshot into the report, and the
  report builds its records by reading rows out of this table. A missing row
  is *skipped* — correct today only because rows never vanish except by
  unlink. This is what makes eviction a data-loss risk if the pin rules are
  wrong.
- **Ghosts:** unlink keeps the row at `nlink=0` so an open fd can still
  resolve the inode.

## 3. Pin rules

**Rule: the table is a cache of what this client is *doing*, not a replica of
what exists.** Everything authoritative is re-fetchable — rows by GETATTR +
adopt, chunk maps by GETCHUNKS, names by LOOKUP. So a row may be evicted
whenever no in-flight or owed work can observe its absence.

A row (with its chunk maps) is unevictable iff any of these hold:

1. its ino is in the dirty set, in the publishing set (`pub_ino_keys`), or has
   dirty dcache slots — evicting it would silently drop a report record;
2. it is a ghost (`nlink=0`) with an open fd — the per-ino open-description
   refcount (`efs_open_note` / `efs_close_note`) is the liveness signal;
3. an in-flight operation pinned it (a bounded per-op pin dropped at op end) —
   this covers the create and rename/link/unlink dual-apply windows;
4. it has a live append reservation or lock record.

Everything else is LRU by last-touch tick, evicting chunk maps of clean closed
files first (cheapest to refetch, largest volume), then rows.

## 4. Rework the bound requires

- `statfs`: stop deriving used bytes and `f_files` from the local table; the
  server-reported numbers are authoritative, and the full-table scan goes.
- `efs_export_fits_page_cap`: a local-table quota heuristic. Under a bounded
  table it must not treat cached rows as the universe — quota is enforced
  server-side, so this becomes a best-effort early-out on staged growth.
- Live ghost reclaim is new (`export_drop_zero_inodes` runs on deserialize
  only), driven by rule 2 expiring at last close.
- **Cap:** `EFS_CLIENT_META_MB`, default 256 MB, over rows + chunk maps + name
  arena. On pressure evict unpinned LRU; if everything is pinned, grow and log
  once — pinned data is real work, not cache. A client under memory pressure
  must degrade to more RPCs, never to lost writes.

## 5. Gate

`make test`; POSIX suite 1 and suite 2 with no new failures; a single-client
namespace-walk RSS gate (walk a multi-million-file tree and show client RSS
staying near the cap where it previously grew linearly); and the valgrind leak
gate, since eviction is a new free path.

## 6. Decisions requested

1. Ratify the pin rules (§3) and the `statfs`/quota rework as the contract.
2. Ratify the 256 MB default cap, or pick another number.
