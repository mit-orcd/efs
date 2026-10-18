# W124 — Fast delete: immediate space, batched reclaim and fragment slot reuse

**Class:** performance / capacity management. **Recorded:** 2026-10-18.
**Status:** PROPOSED; design review before implementation.
Implementation, private A/B and orcd-mini acceptance: **NOTRUN**.
Evidence: [orcd-mini deletes, GC and crawl](../status/orcd-mini-gc-delete-profile-5d673efa8-20261018.md)
and the space accounting below.

## Problem

Deleted data takes hours or days to come back as usable space.
- **Reclaim rate:** orcd-mini on `5d673efa8fca` reclaims 0.18–0.21 GiB/min
  per node (48–57 fragments/s), whatever the backlog or load.
- **Backlog:** on 2026-10-18 about 6.1 M deleted fragments per node, about
  398 GiB, 53% of the disk, were still waiting. That is about 33 hours at
  the current rate.
- **Effect:** twice that day the 720 GiB quota stayed full for hours after
  large deletes, and users saw ENOSPC.

The hardware is not the limit. A fragment unlink averages 27 µs (about 37 K/s
on one thread), the disk was 54–81% utilised, and the GC thread was busy only
about a third of the time. The limit is the reclaim protocol.

### Where the time goes

Per dead inode, today's path is strictly serial (`host_gc_reap_pass`,
`src/server/raft_host.c`):
1. The leader proposes `LANE_SWEEP_BATCH` for each active lane and waits for
   the commit.
2. `host_gc_inode_nodes` makes a synchronous RPC to every storage node.
   `server_gc_inode` (`src/server/store.c`) then:
   - writes the inode's death bit (`inode_death_set`);
   - unlinks its fragment files and syncs the inode directory;
   - removes up to five levels of empty scaffolding directories, syncing
     each parent.

   That is about 8 directory fsyncs per removed fragment: 19.2 M fsyncs for
   2.41 M unlinks per node.
3. The leader proposes `REAP_DONE` and waits for the commit.
4. Separately, the fragment pass deletes GC-record fragments and proposes
   `GC_ACK` batches.

A pass reaps at most 32 markers per group (`REAP_SCAN_MAX`) and repeats after
1 s (`GC_LOOP_MS`). The opt-in `EFS_GC_BACKLOG_IDLE_MS` only shortens the
wait between passes, at best a few times faster, against the roughly 600×
headroom.

### Space accounting on orcd-mini (2026-10-18, node 1; the other nodes match)

`ecrawl` reports 108.6 GB (101 GiB) apparent, but the node holds 11,560,476
fragment files of exactly 69,632 bytes each (64 KiB payload plus header and
digest): 749.7 GiB on disk, 706 GiB counted against quota.

| Part | Fragments | On disk | Share |
| --- | --- | --- | --- |
| Deleted, not yet reclaimed | ~6.14 M | ~398 GiB | 53% |
| Live tiny files: 4.6 M files of 0.2–0.3 KiB, about 1–1.5 GB of data | 4.61 M | ~298 GiB | 40% |
| `big-files/001.dat`, 100 GiB with 2+1 coding | 0.82 M | ~53 GiB | 7% |

The deleted share is derived (total minus the live files' expected
fragments), not counted directly. The orphan scan reports none.

## Requirements

- **POSIX delete never waits for physical reclaim.** `unlink`/`rmdir`
  remain metadata-only Raft operations, as today. No fragment I/O, death
  fence or fsync of fragment directories enters their latency.
- **Space returns to users when the delete commits,** not when fragments
  are gone.
- **The physical disk can never be overfilled** by reclaim lagging behind.
- **Unchanged safety:**
  - no fragment is removed or reused while any live or open-unlinked inode
    (lease), retained generation or in-flight publication can reference
    it;
  - writes to a dead inode stay refused;
  - every step is idempotent and survives crash and leader change.
- **No foreground regression:** reclaim must not raise create/stat/read
  latency or lower throughput beyond agreed limits.

## Proposal

The parts are listed in implementation order:
1. batched reclaim;
2. quota credit at delete commit;
3. default small-file packing, tracked as W125;
4. fragment slot reuse, re-assessed after packing;
5. instant subtree delete.

### 1. Batched, node-local reclaim

Replace the per-inode serial chain with batches.
- **Leader:** collect up to N dead inodes per group per pass (N in the
  thousands, not 32). Propose one batched lane-sweep entry and one batched
  reap-done entry for the whole batch, as `GC_ACK` already batches 128
  items. That is one Raft commit per batch instead of 2–3 per inode.
- **Nodes:** one RPC per node per batch. The node:
  - records the batch durably with a single fsync: the death bitmap pages
    for all inodes, plus a small local reclaim journal;
  - acknowledges at once;
  - deletes the fragment directories asynchronously with a small thread
    pool.

  Directory durability is amortised to one fsync per touched directory per
  batch (or one `syncfs` per batch). Empty scaffolding directories are
  removed lazily by a periodic sweep, not five levels per inode.
- **Safety:** the death bit is durable before the acknowledgement, so the
  write guard is unchanged. The journal makes the asynchronous deletes
  crash-safe: on restart the node replays it, and deleting is idempotent.
- **Target:** reclaim bound by XFS unlink throughput, 50–100 K
  fragments/s per node with a few threads. The 6.1 M backlog above would
  clear in minutes.

### 2. Quota credit at delete commit

- **Credit:** when the inode's removal commits (`nlink` reaches 0 and no
  lease holds it open), credit its fragment bytes back to the quota.
- **Pending pool:** track reclaim-pending bytes separately (in `gc-status`
  and `efs-mgmt status`).
- **Physical guard:** admission keeps a physical check on filesystem free
  space with the existing reserve (20%, at least 1 GiB per filesystem). So
  pending reclaim plus new writes can never fill the disk; if that guard
  binds, it returns ENOSPC with its own reason.
- **Effect:** users get space back as soon as the delete returns, whatever
  the state of reclaim.

### 3. Small-file packing as the default (W125)

Every file, however small, currently gets at least one full fragment per node.
A 200-byte file therefore costs about 800× its size in raw disk, and one
fragment per node to reclaim when it is deleted. The 298 GiB per node of live
tiny files above would become a few GiB.

The user wants packing to be the default:
- files up to about 64 KiB packed into shared segments of ordinary
  fragments, never stored in metadata rows;
- larger files unchanged.

Its fragmentation rules are designed in from the start:
- pack files that die together;
- count live bytes per segment;
- compact survivors forward below a live-share threshold, so no fragment
  stays pinned while mostly dead.

It is restored from the archived implementation (D37, tag
`archive/packing-containers-20261016`) after parts 1–2. Packing replaces
the current small-file path outright, with no switch; the old way is kept
only as a tag. Its segment
compaction reuses part 1's batched reclaim. Design decisions, order and
gates are in [W125](small-file-packing-default.md).

### 4. Fragment slot reuse (re-assess after W125)

Every fragment file is the same size (69,632 bytes on orcd-mini), so a dead
fragment is a ready-made slot. Once W125 packs small files, most
tiny-file fragments disappear. Then decide whether reuse is still worth it,
mainly for large files.
- **Pool:** after the death is durable (the point where step 1 may delete),
  a node may move a dead fragment file into a per-node free-slot pool
  directory instead of unlinking it.
- **Writes:** a new or rewritten fragment takes a slot from the pool,
  overwrites it completely (header, payload and digest, never partially),
  and renames it into its new path. This skips file creation and block
  allocation, the expensive XFS steps, and the space is in use again
  immediately.
- **Durability:** unchanged. The new content is durable before it is
  published, as for a fresh fragment today.
- **Stale readers:**
  - a reader holding an old chunk map finds the old path gone (ENOENT)
    and re-fetches the map;
  - the per-file digest also rejects any content that is not the expected
    fragment.
- **Pool limits:** the pool is physically used but logically free (with
  part 2). Its size is capped, by default a few tens of GiB per node, or
  a share of filesystem free space.
- **Scheduled deletion:** the background thread deletes slots beyond the
  cap, when there is no write demand, or when the physical guard nears its
  limit.
- **Scope:** keep the file-per-fragment model. Packing fragments into
  large preallocated files at fixed offsets is essentially the container
  engine removed in D37 and is out of scope.
- **Open question:** whether a reused slot needs a fresh inode-number-based
  directory placement, or can be renamed across the inode scaffolding. The
  latter is preferred, and needs one rename plus a directory fsync batched
  with the write barrier.

### 5. Instant subtree delete (optional)

- **Proposal:** a server-side tree delete, an operator command or `.efs`
  verb. It detaches a subtree in one operation (rename into a hidden trash
  directory, quota credited by part 2), then unlinks it in the background.
- **Effect:** the tree disappears instantly instead of after one client
  unlink per file. `edelete` of the two evict trees took about 9 minutes on
  2026-10-18.

## Acceptance gates

All gates are NOTRUN. Each step gets an evidence page per AGENTS.md before
orcd-mini.

**Parts 1 and 2:** private 3-node RDMA cluster on node9901, using a
delete/reclaim A/B script committed under `scripts/` with the change. A
draft from the 2026-10-17 GC investigation exists outside the repo and
must be committed before it is cited.
1. Create 1 M tiny files plus 4 × 64 MiB files, then delete them. Measure:
   - `unlink` p50/p99 (must not regress);
   - time until the quota shows the space free (target: when the deletes
     return);
   - time until `data_usage_bytes` reaches 0 (target: minutes, not hours);
   - fragments/s per node.
2. Foreground impact during reclaim: create/stat/read p50/p99 and
   throughput with reclaim running versus idle, within agreed limits.
3. Crash tests:
   - kill a storage node mid-batch: the journal replays, nothing is lost or
     double-deleted;
   - change the leader mid-batch: idempotent restart;
   - an open-unlinked file keeps its data until last close.
4. Physical guard: fill to the reserve while reclaim lags; ENOSPC is
   reported with its distinct reason and the disk is never overfilled.
5. `make test`, plus the private GC, POSIX and expiry suites, PASS.

**Part 3:** see [W125](small-file-packing-default.md).

**Part 4:** the same as parts 1 and 2, plus:
- write latency and throughput into reused slots versus fresh files;
- stale-reader and digest-mismatch tests;
- pool cap and scheduled deletion behaviour.

**Part 5:** to be defined in its own design.

## Non-goals

- Changing POSIX semantics of unlink, open-unlinked files or leases.
- Reintroducing the container engine (D37). Packing returns separately
  under W125.
- Weakening durability: no fragment is reused or deleted before its death
  is durable on that node.
