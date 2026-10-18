# W124 — Fast delete: quota at delete, batched reclaim and fragment slot reuse

**Class:** performance / capacity management. **Recorded:** 2026-10-18.
**Status:** parts 1 and 2 IMPLEMENTED on branch `w124-reclaim`
(2026-10-18, see [Implementation](#implementation-2026-10-18)); the old
per-inode leader path is removed, not kept behind a switch. Unit gates
PASS; private and orcd-mini acceptance: see the evidence page linked from
[status](../status/README.md). Design reviews: [Review](#review-2026-10-18).
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
1. Appends the batch to the current journal segment (format below): one
   write and an fsync of that segment, plus a directory sync when a new
   segment file is created. This is the commit point. No multi-page bitmap
   update is claimed to be covered by "one fsync".
2. Establishes the write fences for every inode in the batch, draining
   writers that are already in flight:
   - **Under the guard:** each fence is installed under that inode's write
     guard (`server_inode_gc_lock`), as `server_gc_inode` does today. A PUT
     already holding the guard finishes before the fence. Its fragment is
     then simply part of what step 5 deletes.
   - **Check inside the guard:** the only fragment write paths
     (`server_write_fragment_sync` and
     `server_write_fragment_with_sum_sync`, `src/server/store.c`) already
     check `inode_is_dead` and write while holding the guard. That must
     stay, so a PUT that passed authority checks earlier, or was paused
     before taking the guard, still sees the fence. No write path may check
     death outside the guard.
   - **The check must see fences not yet checkpointed:** `inode_is_dead` reads
     the bitmap pages from disk, but those are checkpointed *after*
     acceptance (step 4). It must also consult an in-memory fence set
     populated at step 2 and rebuilt from the journal on restart.
     Otherwise a write between acceptance and checkpoint would be
     admitted.
   - **No late publication:** a fragment written after reclaim began would
     be an orphan. Its publication is refused anyway, because the inode is
     dead in metadata. The fence prevents the write itself.
3. Acknowledges. The node has now **accepted the batch for reclaim**, which
   is not yet **physically reclaimed**. The two are tracked and reported
   separately.
4. Checkpoints the death-bitmap pages from the journal asynchronously,
   using the existing write-temp, fsync, rename and parent-sync sequence,
   once per touched page rather than once per inode.
5. Deletes fragment directories with a small bounded thread pool. Directory
   durability is batched: one fsync per touched directory per round.
   Empty scaffolding directories are removed by a periodic sweep.
6. Marks an inode **physically reclaimed** only after its unlinks and their
   directory syncs are durable. Physical-reclaim credit comes from this
   step.
7. Retires journal records only after both their bitmap checkpoint (step 4)
   and their physical reclaim (step 6) are durable, and only a whole
   segment at a time (format below). The record is what lets a restart
   resume both, so neither alone may retire it.

**Journal format and retirement.**
- **Segments:** the journal is a sequence of segment files, each with a
  checksummed header (node id, root, journal epoch, segment sequence) and
  checksummed records (batch id, inode, generation). A segment is sealed
  when it reaches a size or record limit.
- **No truncation:** records complete out of order, so nothing is ever
  truncated past an unfinished record. A segment file is deleted only when
  every record in it has completed steps 4 and 6. That deletion is followed
  by a directory sync, and replaces no other state.
- **Completion is not persisted:** per-record completion is tracked in
  memory only. After a restart, the node re-runs checkpointing and
  deletion for every record in every surviving segment; both are
  idempotent (bitmap bits already set; unlinks of absent files are
  skipped). This avoids rewriting segment files to mark progress.
- **Where it lives:** in each storage root's `meta/`, next to the
  `gc-dead` bitmaps, so the journal and the root it describes share a
  filesystem.

**Journal bound.** The journal has a per-node cap on accepted but not yet
retired records. Above it the node answers BUSY to new batches, so the leader
keeps their REAP markers and retries later, rather than letting a slow disk
grow the journal without limit.

**Which state permits `REAP_DONE`: acceptance, not completion.** The leader
proposes `REAP_DONE` for an inode once every cluster member has accepted it,
that is, journalled it durably with fences installed (step 3).
- **All members:** as today's presence check in `host_gc_inode_nodes`,
  every member must accept. A down or unreachable node keeps the REAP
  marker, and the inode is retried.
- **Why acceptance:**
  - acceptance transfers durable responsibility for the remaining physical
    work to the node's journal, which is verified and replayed after a
    crash;
  - it lets the replicated metadata drop the marker sooner, which reduces
    retained metadata and REAP-scan work.

  Completion could also be batched, so it does not inherently cost
  per-inode round trips. Physical capacity also matters to users, which
  is why physical reclaim is tracked and reported separately (part 2).
- **Risk and mitigation:** with the marker gone, a lost or corrupt journal
  would leave fragments behind and fences missing. That is why journal loss
  is detected and fails closed (above). It is not assumed to coincide with
  losing the disk.
- **Invariant this relies on: a reaped inode number is never reallocated.**
  The node deletes the whole inode directory regardless of generation
  (`gc_dead_directory`). If the number came back while deletion was
  pending, the new file's fragments could be deleted. The invariant holds
  today in two ways:
  - the allocator skips any inode that keeps durable transaction history
    (`alloc_inode_unused`, `src/meta/meta_apply.c`);
  - the per-inode death bit is permanent, so a reused number would be
    refused writes anyway.

  The implementation must keep it, and a test pins it.
- **Where reclaim-pending lives:** after `REAP_DONE`, metadata no longer
  records which inodes await physical deletion. Each node's journal does,
  so the cluster's reclaim-pending figure (part 2) is the sum of the nodes'
  journal totals, reported by `gc-status`.

**Journal loss or corruption: detect and fail closed.** A journal is not
lost only together with its disk. Corruption, accidental truncation or
deletion, restoring an older copy of `meta/`, or losing a separate journal
device can all leave fragments intact while records are gone. After
`REAP_DONE` there is no replicated marker left to retry, and the orphan scan
does not restore write fences.
- **Detection:**
  - every segment and record is checksummed;
  - each root persists the journal epoch and the next expected segment
    sequence in a small durable per-root record (written with fsync and a
    directory sync, next to `.efs-durable-epoch`), updated whenever a
    segment is created or retired;
  - at startup, a missing segment in the sequence, a checksum failure, an
    epoch or sequence going backwards, or a missing journal while the
    epoch record expects one is a detected loss.
- **Fail closed:** on a detected loss the root refuses all data PUTs, the
  same posture as today's "recovery uncertain" fence. Reads of live data
  continue.
- **Recovery** happens before writes are re-admitted:
  - scan the root's fragment directories;
  - for every inode present on disk, ask metadata whether it is live;
  - journal and fence every inode that is not;
  - checkpoint, then clear the fence.

  This rebuilds the fences, which the orphan scan alone does not.

**After restart:** the node verifies and replays the journal (rebuilding the
in-memory fence set) before it accepts any writes. That re-establishes the fences for all accepted inodes, then
resumes checkpointing and deletion. Every step is idempotent.

**Scope:** part 1 batches the REAP path, which handles whole-inode deaths
(unlinks). Fragments freed by truncates and overwrites go through the
separate GC-record pass (`host_gc_frag_pass`), which already batches 128
acks per `GC_ACK` entry under a time budget. That pass is not redesigned
here. It is measured in the A/B, and batched further if it turns out to be
the next limit.

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
- **Recovery headroom, per filesystem, with a progress argument:**
  - **Two reservations:** each filesystem holding storage roots keeps two
    atomically reserved amounts below the physical reserve:
    - a **journal and checkpoint minimum**, used only by reclaim journal
      segments and bitmap checkpoints, and never by compaction;
    - a **compaction headroom**, used only by W125 compaction copies.
  - **Why journal space is enough:** plain deletion needs only journal and
    checkpoint space, so reclaim always progresses from the minimum. Every
    completed segment retirement frees journal space again, and every
    physical reclaim frees fragment space.
  - **Why compaction progresses:**
    - compaction copies are admitted against the compaction headroom with
      atomic reservations, like writes;
    - the number of concurrent compaction jobs is capped, so the headroom
      is sized for the worst case: the cap times one segment's fragments on
      this filesystem;
    - compaction only selects segments whose old copy can be released
      once the switch is done: none pinned by an open lease or an active
      reader. Pinned segments are counted as pinned bytes, not as
      compaction candidates, so headroom spent on a copy is always repaid
      by a release.
  - **Pinned bytes are a separate limit:** they are bounded by open files
    and reported. If they keep a filesystem at its limits, writes see
    ENOSPC with that reason; compaction does not spin on them.
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
- files below a packing threshold measured in W125 plan phase P0 (candidates 4–64 KiB) packed into shared segments of ordinary
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
  - Concurrent mutations (writes, renames, new links into the subtree)
    while it is being detached or emptied need defined semantics.
  - Part 2 credits quota only when each inode's removal is final.
- **Deferred** until subtree accounting (what the trash holds, how hard
  links and leases retain it, when credit happens) and retention semantics
  are defined.

## Implementation (2026-10-18)

**Code:**
- **Leader** (`src/server/raft_host.c`, `host_gc_reap_pass`): up to 512
  REAP markers per group per pass. Lane sweeps go out as bounded
  `LANE_SWEEP_MULTI` (MD 71) entries, one per owning group and at most
  48 KiB each, for up to 32 rounds of 8 chunks per lane per pass. A lane
  counts as swept only when its lane key reads absent on the applied
  replica. Unhosted lanes send one `SWEEP_NEXT` per pass to their leader,
  which packs a one-item `LANE_SWEEP_MULTI`. Fully swept inodes then go to
  every member in one `GC_INODE_BATCH` (wire 155/156) per member; the
  remote requests are sent before the local acceptance runs.
  `REAP_MULTI` (MD 72) entries complete only the inodes every member
  accepted, each on its inode's own group. Every item rechecks the inode
  as `REAP_BATCH` does, and a refused item keeps its marker. A capped pass
  that made progress reruns after 20 ms (`EFS_GC_BACKLOG_IDLE_MS`
  overrides).
- **Reaped sweeps queue no GC records** (`efs_meta_apply_lane_sweep_reaped`).
  The inode's fragments are deleted whole by the members' journals, so the
  per-chunk `GC_FRAGMENT` round trips are gone from the delete path.
- **Node** (`src/server/reclaim.c`): implements steps 1–7 above.
  - **Acceptance:** dedupe, then the cap check (BUSY above it), then the
    journal append and fsync, then the fences installed under each inode's
    write guard, then the credit; only then the reply.
  - **Background work:** a deleter pool of 4; bitmap checkpoints with one
    update per touched page (`server_gc_death_set_many`); whole-segment
    retirement, where an idle current segment is sealed after 1 s.
  - **Startup:** verification and replay run before `listen`. Fail-closed
    recovery fences only inodes that metadata proves dead: a ReadIndex,
    then a row that is absent while the key keeps transaction history and
    no intent.
  - **Removed:** the old synchronous `server_gc_inode`. `GC_INODE` (121) is
    now a one-item acceptance.
- **Accounting:** `used` is debited at acceptance by the inode's on-disk
  payload, recomputed from disk, which also makes replay credit exactly
  once against the startup inventory. `reclaim_pending_bytes` carries the
  same bytes until deletion. Deletion only counts what it removes, and
  settles once per inode: if another path (the chunk GC pass) removed part
  of the credited bytes meanwhile, `used` gets those back, so nothing is
  debited twice.
- **Status:** `gc-status` is version 4 and adds a `reclaim_*` line:
  accepted, deleted, checkpointed, retired, BUSY, journal records,
  segments and epoch, pending and credited bytes, physical availability
  and refusals, and the fail-closed state.

**Deviations from the plan, and why:**
- **One journal per node:** it lives on `storage_paths[0]`, not one per
  root. Records carry (export, inode) and deletion covers every root, so a
  batch costs one fsync per node. On a node whose roots are on different
  filesystems, the journal shares a filesystem with the first root only.
- **The state record lives outside `meta/`,** at
  `<root>/.efs-reclaim-state`. Restoring an older but self-consistent
  `meta/` then shows missing segments and fails closed; a unit test covers
  this.
- **Credit point:** the credit lands at member acceptance, seconds after
  the final unlink or close, not inside the Raft entry that finalises
  removal. Quota is per node, so the node that holds the bytes is where
  the credit can be exact.
- **Physical floor:** 5% of each filesystem (at least 1 GiB) plus a
  256 MiB journal and checkpoint minimum, not the 20% startup reserve. A
  20% runtime floor would refuse every write on orcd-mini, whose `/data1`
  is about 84% used. The startup quota check keeps its 20% reserve. The
  guard is per filesystem: it is sampled every 500 ms and charged
  atomically under the server lock at both PUT charge sites.
- **Directory durability** is still one directory sync per inode
  directory, not one per touched directory per round. This is the
  remaining per-inode cost; it is measured on orcd-mini.
- **Compaction headroom** does not apply until W125.

## Acceptance gates

All gates are NOTRUN. Each step gets an evidence page per AGENTS.md before
orcd-mini.

**Order:**
1. Correctness at every handoff boundary: the leader batch entry, the node
   acceptance, the bitmap checkpoint, physical deletion, journal
   retirement and `REAP_DONE`. Use crash injection at each boundary.
2. Only then performance: durable reclaim throughput (fragments durably
   reclaimed per second, not bare unlinks), measured together with
   foreground p50/p99 under the same load. The 50–100 K fragments/s figure
   is an experiment target, not inferred capacity.

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
     acknowledgement, during checkpoint, during deletion, and between
     deletion and journal retirement. The journal
     replays before writes are accepted; nothing is lost, double-deleted
     or left writable;
   - change the leader mid-batch, and inject a partial batch failure: an
     idempotent restart, with only failed inodes retried;
   - a node down during a batch: no `REAP_DONE` for its inodes until it
     accepts;
   - a PUT racing the fence: it completes before the fence or is refused,
     never lands afterwards;
   - a PUT paused after its authority checks but before taking the guard,
     resumed after acceptance: refused, with no fragment written;
   - a PUT after acceptance but before the bitmap checkpoint: refused by
     the in-memory fence set, including after a restart in that window;
   - crashes during journal segment retirement: between the last record's
     completion and the segment file's deletion, and between the deletion
     and its directory sync. Replay is idempotent, and the durable sequence
     record agrees with the surviving segments;
   - the journal cap reached: the node answers BUSY, and markers stay and
     are retried;
   - inode-number reuse: a reaped number is never reallocated, including
     after replay and a leader change.
   - an open-unlinked file keeps its data until last close, and is
     credited exactly once, at final lease release.
4. Journal loss detection (fail closed):
   - a corrupted segment (checksum), a truncated or deleted segment, a gap
     in the sequence, and an older copy of `meta/` restored over a newer
     one are each detected at startup;
   - the root then refuses data PUTs;
   - recovery rebuilds the fences from a fragment scan against metadata
     before writes resume;
   - a dead inode's fragments are eventually deleted, and no live inode's
     fragments are touched.
5. Accounting:
   - logical quota, reclaim-pending and physical availability are reported
     separately and agree with on-disk counts after reclaim;
   - exactly-once credit across replay, overwrite and lease release.
6. Physical guard and headroom:
   - concurrent writers race for the last physical space: reservations
     never over-admit;
   - fill to the reserve while reclaim lags: ENOSPC is reported with the
     physical reason, and the disk is never overfilled;
   - with the disk at its limit, reclaim still progresses from the journal
     and checkpoint minimum, and compaction never consumes that minimum;
   - a segment pinned by an open lease or reader is not selected for
     compaction, and is reported as pinned bytes.
7. `make test`, plus the private GC, POSIX and expiry suites, PASS.

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

### Second review (2026-10-18)

Most findings repeated the first review and were already addressed:
- journal-first handoff;
- three-way accounting with atomic reservations;
- batching per owning group;
- reader quiescence and rename durability;
- subtree deferral;
- the rate figure as a hypothesis.

New points, dispositions:

| Finding | Disposition |
| --- | --- |
| Journal retirement must follow durable deletion. | **Accepted; it was a bug in the revision.** Records were to be truncated after the bitmap checkpoint, which would lose pending deletions on a crash. They now retire only after both the checkpoint and the physical reclaim are durable (part 1 step 7). |
| Specify whether "accepted" or "complete" permits `REAP_DONE`. | **Decided: acceptance by every member.** Rationale in part 1. This takes a position rather than waiting for completion, which would re-introduce per-inode round trips without user-visible benefit. |
| Preserve headroom for reclaim and packing compaction. | **Partly accepted.** Plain deletion needs almost no space beyond the small journal. Compaction does need room, because it copies before freeing, so a recovery headroom is reserved below the physical reserve (part 2). |
| W125: admission behaviour under sustained churn. | **Accepted.** W125 now throttles new packed writes with a bounded delay as the caps approach, then returns ENOSPC with the reason. |
| Subtree delete: concurrent mutations. | **Accepted;** added to its deferral conditions. |
| Gate correctness at every handoff, then measure durable throughput with p99. | **Accepted;** gate order added. |

### Self-review (2026-10-18)

Re-reading the plan after the second review, with these findings:

| Finding | Disposition |
| --- | --- |
| `REAP_DONE` on acceptance lets metadata forget an inode while its fragments still await deletion. The node deletes the whole inode directory regardless of generation, so a reallocated number would be hit. | **Checked:** numbers are not reallocated today (`alloc_inode_unused`; permanent death bits). Recorded as an invariant the implementation must keep, with a test. |
| Fence installation must exclude in-flight PUTs. | **Added:** the per-inode write guard, as in today's `server_gc_inode`. |
| The journal had no size bound. | **Added:** a per-node cap; above it, BUSY and the markers stay. |
| Reclaim-pending had no defined owner after `REAP_DONE`. | **Added:** the sum of the nodes' journal totals. |
| Scope against truncate and overwrite reclaim (GC records) was unstated. | **Added:** that pass is not redesigned here, but is measured. |

### Fourth review (2026-10-18)

Review of `d1e64549c`. It judged acceptance-based `REAP_DONE` a reasonable
durable ownership transfer, and raised implementation-contract gaps. All
were accepted:

| Finding | Disposition |
| --- | --- |
| "A lost journal means a lost disk" is wrong: corruption, truncation, an older restored `meta/` or a separate journal device leave fragments intact, the replicated marker is gone after `REAP_DONE`, and the orphan scan does not restore fences. | **Accepted.** The claim is removed. Checksummed segments, a durable per-root sequence and epoch record, detection at startup, fail-closed refusal of data PUTs, and fence-rebuilding recovery from a fragment scan against metadata. |
| Acceptance must drain writers that already passed their checks. | **Accepted.** The death check stays inside the per-inode guard, as today's two write paths already do. New finding while checking it: `inode_is_dead` reads the on-disk bitmap, which is now checkpointed after acceptance, so it must also consult an in-memory fence set rebuilt from the journal. Paused-PUT and pre-checkpoint race gates added. |
| Recovery headroom needs a progress argument. | **Accepted.** Per-filesystem atomic reservations; a journal and checkpoint minimum compaction cannot use; capped concurrent compaction sized into the headroom; pinned segments excluded from selection and reported. |
| Journal retirement needs an on-disk protocol; never truncate past an unfinished record. | **Accepted.** A segmented journal, deleted one whole segment at a time, with completion recomputed by idempotent replay rather than persisted. Crash gates during retirement added. |
| The `REAP_DONE` rationale was wrong: completion can be batched, and physical capacity matters. | **Accepted; corrected.** Acceptance is justified by the transfer of durable responsibility and reduced retained metadata. |

## Non-goals

- Changing POSIX semantics of unlink, open-unlinked files or leases.
- Reintroducing the container engine (D37). Packing returns separately
  under W125.
- Weakening durability: no fragment is reused or deleted before its death
  is durable on that node.
