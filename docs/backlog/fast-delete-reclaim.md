# W124 — Fast delete: quota at delete, batched reclaim and fragment slot reuse

**Class:** performance / capacity management. **Recorded:** 2026-10-18.
**Status:** PROPOSED; revised after design review (2026-10-18, see
[Review](#review-2026-10-18)). Implementation, private A/B and orcd-mini
acceptance: **NOTRUN**.
- **Proceed:** part 1 (bounded batched reclaim) and part 2 (explicit
  accounting), then W125.
- **Deferred:** parts 4 (slot reuse) and 5 (subtree delete).
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

The protocol, not raw hardware, appears to be the first limit:
- the GC thread was busy only about a third of the time;
- the disk was 54–81% utilised by everything combined;
- a bare fragment unlink averages 27 µs.

How far batching can go is unmeasured. The unlink timing excludes durable
fences, journal writes, directory synchronisation and foreground
interference.

### Where the time goes

Inodes are processed one after another (`host_gc_reap_pass`,
`src/server/raft_host.c`). For each inode:
1. The leader proposes one `LANE_SWEEP_BATCH` per active lane, or
   `SWEEP_NEXT` for a lane it does not host, to the lane's own Raft group.
   That may be a different group from the inode's. It waits for each
   commit.
2. `host_gc_inode_nodes` sends the storage fence to every remote node
   first, runs the local one, then collects the replies. The nodes work in
   parallel for one inode, but the next inode starts only after all reply.
   `server_gc_inode` (`src/server/store.c`) then:
   - writes the inode's death bit (`inode_death_set`): a temp-file write,
     fsync and rename of the bitmap page, plus parent-directory syncs;
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
- **Logical quota returns when the delete commits,** not when fragments
  are gone. Writable space is bounded separately by physical availability
  (part 2). The two are reported separately, never conflated.
- **The physical disk can never be overfilled** by reclaim lagging behind,
  including under concurrent writers.
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
1. bounded batched reclaim;
2. explicit space accounting at delete commit;
3. default small-file packing, tracked as W125;
4. fragment slot reuse (deferred, re-assessed after packing);
5. instant subtree delete (deferred).

### 1. Bounded batched, node-local reclaim

Replace inode-at-a-time processing with bounded batches, per owning group.

**Leader side:**
- Collect up to N dead inodes per group per pass (N in the thousands, not
  32), bounded by the Raft entry size.
- Lane sweeps go to each lane's owning group, so a batch becomes one
  bounded `LANE_SWEEP` entry per owning group. A large batch splits into
  several entries.
- One reap-done entry per inode group after its storage fences succeed.
- The result is a constant number of entries per batch and group, not "one
  commit per batch", and not 2–3 per inode.
- **Partial results:** each entry reports per-inode outcomes. Failed or
  BUSY inodes keep their durable REAP marker and are retried with the
  existing round-robin cursor; successful ones are not repeated.

**Node side: a journal-first protocol.** One RPC per node per batch,
carrying the batch's (inode, generation) list. The node:
1. Appends the batch to a local reclaim journal: one write, fsync of the
   journal file, and a directory sync when the journal file is created.
   This is the commit point. No multi-page bitmap update is claimed to be
   covered by "one fsync".
2. Establishes the write fences for every inode in the batch in memory:
   new PUTs for those inodes are refused from this point.
3. Acknowledges. The node has now **accepted the batch for reclaim**, which
   is not yet **physically reclaimed**. The two are tracked and reported
   separately.
4. Checkpoints the death-bitmap pages from the journal asynchronously,
   using the existing write-temp, fsync, rename and parent-sync sequence,
   once per touched page rather than once per inode. Journal records are
   truncated only after their pages are durable.
5. Deletes fragment directories with a small bounded thread pool. Directory
   durability is batched: one fsync per touched directory per round.
   Empty scaffolding directories are removed by a periodic sweep.
6. Marks an inode **physically reclaimed** only after its unlinks and their
   directory syncs are durable. Physical-reclaim credit comes from this
   step.

**After restart:** the node replays the journal before it accepts any
writes. That re-establishes the fences for all accepted inodes, then
resumes checkpointing and deletion. Every step is idempotent.

**Rate:** batching is expected to raise reclaim far above today's 48–57
fragments/s, but the achievable rate is a hypothesis to measure. A bare
unlink is 27 µs, but the fences, journal writes, directory syncs and
foreground interference are not yet measured. The private A/B decides the
batch size, the thread count and the expected rate.

### 2. Explicit space accounting at delete commit

Three quantities, each reported separately in `gc-status` and
`efs-mgmt status`:
- **Logical quota used:** live files' bytes. A delete credits it when the
  inode's removal is final: `nlink` reaches 0 and the last lease is
  released. For an open-unlinked file that is the final lease release, not
  the unlink.
- **Reclaim-pending:** bytes credited logically but not yet physically
  reclaimed (part 1 step 6).
- **Physical availability per filesystem:** statvfs free space minus the
  reserve (20%, at least 1 GiB per filesystem), minus outstanding
  admission reservations.

**Rules:**
- **Atomic reservations:** a write must hold both a logical-quota
  reservation and a physical reservation on the target filesystem. Each is
  taken atomically per filesystem (a per-filesystem reservation counter),
  so concurrent writers cannot both pass a free-space check for the same
  bytes. A free-space check alone is not enough.
- **Exactly-once credit:** the logical credit is applied once per (inode,
  generation). That holds across journal replay, leader change, overwrite
  (the old generation's bytes are credited when the new generation
  publishes) and the final lease release. The credit is part of the Raft
  entry that finalises removal, so replays are idempotent.
- **Honest effect:** users see quota freed as soon as the delete is final.
  Whether new data can be written immediately depends on physical
  availability, which falls behind while reclaim is pending. If physical
  space binds, the write gets ENOSPC with that reason and the pending
  amount, not a quota error.

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

### 4. Fragment slot reuse (deferred; re-assess after W125)

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
- **Open readers (required before any reuse):**
  - Renaming the old path does not invalidate a reader that already has
    the file open. Overwriting the same inode would change what that reader
    sees.
  - Today fragment reads do not take `server_inode_gc_lock`, although the
    comment at `store.c:729` says all paths do; only the fragment writes and
    `server_gc_inode` take it.
  - Reuse therefore requires explicit reader retirement first, for example
    a per-inode reader count or epoch that readers hold for the life of
    their open, with a slot reused only after it drains.
  - A digest error is detection, not preservation of valid read behaviour;
    it is not a substitute.
  - A reader with an old chunk map that opens after the rename gets ENOENT
    and re-fetches the map, which is acceptable.
- **Pool limits:** the pool is physically used but logically free (with
  part 2). Its size is capped, by default a few tens of GiB per node, or
  a share of filesystem free space.
- **Scheduled deletion:** the background thread deletes slots beyond the
  cap, when there is no write demand, or when the physical guard nears its
  limit.
- **Scope:** keep the file-per-fragment model. Packing fragments into
  large preallocated files at fixed offsets is essentially the container
  engine removed in D37 and is out of scope.
- **Rename durability:** moving a slot from the pool into an inode's
  directory is a cross-directory rename. Both the pool directory and the
  destination directory must be durable before the new fragment is
  acknowledged; the removal from the pool matters as much as the arrival.

### 5. Instant subtree delete (deferred)

- **Idea:** a server-side tree delete, an operator command or `.efs` verb.
  It detaches a subtree in one operation (rename into a hidden trash
  directory), then unlinks it in the background, so the tree disappears
  instantly. `edelete` of the two evict trees took about 9 minutes on
  2026-10-18.
- **Not part of part 2's accounting:**
  - Files renamed into trash are still linked.
  - Hard links elsewhere in the namespace, and open files, can keep their
    data.
  - Part 2 credits quota only when each inode's removal is final.
- **Deferred** until subtree accounting (what the trash holds, how hard
  links and leases retain it, when credit happens) and retention semantics
  are defined.

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
   - kill a storage node after the journal fsync but before the
     acknowledgement, during checkpoint and during deletion. The journal
     replays before writes are accepted; nothing is lost, double-deleted
     or left writable;
   - change the leader mid-batch, and inject a partial batch failure: an
     idempotent restart, with only failed inodes retried;
   - an open-unlinked file keeps its data until last close, and is
     credited exactly once, at final lease release.
4. Accounting:
   - logical quota, reclaim-pending and physical availability are reported
     separately and agree with on-disk counts after reclaim;
   - exactly-once credit across replay, overwrite and lease release.
5. Physical guard:
   - concurrent writers race for the last physical space: reservations
     never over-admit;
   - fill to the reserve while reclaim lags: ENOSPC is reported with the
     physical reason, and the disk is never overfilled.
6. `make test`, plus the private GC, POSIX and expiry suites, PASS.

**Part 3:** see [W125](small-file-packing-default.md).

**Part 4 (deferred):** the same as parts 1 and 2, plus:
- an open reader during reuse sees only its original bytes, or a clean
  retirement, never new content;
- cross-directory rename crash tests;
- write latency and throughput into reused slots versus fresh files;
- pool cap and scheduled deletion behaviour.

**Part 5 (deferred):** to be defined with its accounting and retention
design.

## Review (2026-10-18)

An external review of the first draft raised these points. Each was checked
against the code, and the plan above reflects them.

| Priority | Finding | Disposition |
| --- | --- | --- |
| P1 | "One fsync" cannot durably commit several bitmap files and a journal. The current code also syncs directories (`inode_death_set`: write temp, fsync, rename, parent syncs). | **Accepted.** Part 1 is now journal-first: a commit point, fences before the acknowledgement, replay before writes, asynchronous bitmap checkpoints. "Accepted for reclaim" and "physically reclaimed" are separate states. |
| P1 | Quota credit is not writable physical space, and a free-space check does not reserve against concurrent writers. | **Accepted.** Part 2 separates logical quota, reclaim-pending and physical availability, with atomic per-filesystem reservations and exactly-once credit. The user-facing promise is now "quota freed at delete", not "space back whatever the state of reclaim". |
| P1 | Slot reuse ignores already-open readers; a rename does not invalidate an open descriptor. | **Accepted.** Confirmed that fragment reads do not take `server_inode_gc_lock`, contrary to its comment. Reader retirement is required, cross-directory rename durability covers both directories, and part 4 is deferred. |
| P2 | The baseline was partly outdated: `host_gc_inode_nodes` already pipelines node fences, and lane sweeps can belong to other groups. "One commit per batch" contradicted two entry kinds. | **Accepted.** The baseline now says inodes are processed serially, not that node RPCs are. Batching is per owning group, with bounded entries, partial results and retry. |
| P2 | Subtree delete cannot claim quota credit: renamed files stay linked, and hard links or open files retain data. | **Accepted.** Part 5 is deferred until accounting and retention semantics are defined. |
| — | 50–100 K fragments/s was presented as capacity. | **Accepted.** It is now a hypothesis for the A/B to measure. |
| — | W125's 1/*t* bound excludes queued candidates and retained segments. | **Accepted.** W125 now states the bound for sealed non-candidate segments and adds backlog and headroom limits. |

## Non-goals

- Changing POSIX semantics of unlink, open-unlinked files or leases.
- Reintroducing the container engine (D37). Packing returns separately
  under W125.
- Weakening durability: no fragment is reused or deleted before its death
  is durable on that node.
