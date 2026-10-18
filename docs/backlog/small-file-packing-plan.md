# W125 implementation plan — small-file packing

**Status:** PLAN; no code written. All phases and gates are **NOTRUN**.
Design and requirements: [W125](small-file-packing-default.md).
Prerequisite: [W124](fast-delete-reclaim.md) parts 1–2 (batched reclaim,
explicit accounting). Source of reusable code: tag
`archive/packing-containers-20261016` (branch `packing-containers`; see its
`ARCHIVE.md`). Recorded 2026-10-18.

## Ground rules

- **One storage path:** no runtime, build or environment switch, and no
  unpacked fallback for small files.
- **The old way survives only as a tag:** immediately before landing, tag
  the last per-file-fragment devel commit as
  `archive/unpacked-small-files-<date>`.
- **Comparisons are between builds:** every A/B compares a build of that
  tag (or of the commit the work branch started from) with the branch
  build. There are never two modes in one binary.
- **No file data in metadata rows:** metadata holds only references
  (segment id, generation, offset, length, digest).
- **The container storage engine stays out:** `store_containers.c`, the
  `storage_engine` switch and the container-only paths are not ported.
- **One work branch from devel,** for example `w125-packing`. Each phase
  ends with its tests passing on that branch. Nothing deploys to orcd-mini
  without the user's go-ahead.

## What the archive provides

The packing code removed in `5ded11179` (container engine excluded), and its
disposition:

| Component | Archived files | Role | Disposition |
| --- | --- | --- | --- |
| Segment format | `include/efs/packed_segment.h`, `src/common/packed_segment.c` (163 lines) | Immutable segment image: 96 B header, 72 B per record, ≤ 512 records; per-file references (96 B) with digest; build, validate and read | **Keep.** Revisit the record cap: `EFS_PACK_MAX_PAYLOAD` is 4 KiB today (decision P0.1). |
| Segment codec | `include/efs/erasure.h` (`efs_segment_geometry`, `efs_encode_segment`, `efs_decode_segment`) | Variable-size segment of up to 128 KiB (`EFS_SEGMENT_MAX_SIZE`), 8 KiB aligned, split 2+1 into ordinary fragments | **Keep**, so a segment stays one stripe of standard fragments. |
| Lifetime ledger | `include/efs/pack_ledger.h`, `src/common/pack_ledger.c` (144) | Per-segment KV object: per-record state (PENDING, LIVE, RELEASED, CANCELLED), phases ACTIVE, RECLAIMING, RETIRED, member fence and delete ACKs | **Keep and extend:** live-byte and live-count fields and a compaction phase (P4). Today a segment retires only when every record is released, which is the pinned-hole problem. |
| Mapping planner | `include/efs/pack_mapping.h`, `src/meta/pack_mapping.c` (229) | Applies chunk-map changes to packed references together with ledger updates in one KV batch, under mapping CAS | **Keep.** It is the basis for compaction's location switch. |
| Upload admission | `include/efs/pack_admission.h`, `src/meta/pack_admission.c` (270) | Upload manifests, session authority, upload floors, cancellation, ACKs | **Keep.** Re-check against the current session and fence code. |
| Client coalescing | `include/efs/pack_cohort.h`, `src/common/pack_cohort.c` (56) | Fixed-capacity cohort (128 records), deadline, seal, immutable retry | **Keep and change:** group by lifetime key, not only publication group (P3). |
| Client writer | `src/client/pack_write.c` (666) | Mounted packed write path behind `EFS_PACKED_WRITES` | **Port without the switch.** It becomes the only path for files under the threshold. |
| Packed reads | `src/common/packed_reader.c` (56), `src/client/read.c` changes | Fetch a segment fragment, decode, verify the reference digest | **Keep.** |
| Storage node | `src/server/packed_upload.c` (110), `packed_fence.h` (160), `packed_gc.h` (115), `pack_authority.h` (102), `store.c` and `handler.c` parts | PUT, GET and GC of packed objects, authority checks, per-object fences | **Keep and change:** packed GC uses W124's batched, journal-first node protocol, not its own per-object `GC_PACKED` round trips (P2). |
| Metadata apply | `src/meta/meta_apply.c` (234 lines removed), Raft tags 45–51 | Packed publication, PACK profile, packed chunk extension, packed retirement | **Keep.** Restore the reserved numbers' names. |
| Wire | `include/efs/protocol.h`: messages 131–144, chunk flag `EFS_CHUNK_REC_F_PACKED_BASE` (8) | PUT, GET, GETCHUNKS, GC, capabilities and authority messages | **Keep.** Numbers are still reserved on devel ("131-150 are reserved"). Drop the capabilities negotiation, which only served the switch. |
| Tests | 46 packing/container tests, 2 stress tests | Unit, mounted and stress | **Restore** the packing ones (list in P1–P3). Drop the container-only ones and those testing the opt-in switch. |

Devel keeps fail-closed checks for stores these features wrote: the KVLSM2
reader requirement, root format byte 1, KV kinds 36–39, coding profile 2,
Raft tags 45–51, messages 131–144 and chunk flag 8. The port turns those
refusals back into real handling. This is also the compatibility story: a
store written by the archived code is either readable or refused, never
misread.

## Gaps between the archive and W125

1. **Packing threshold.** W125 says "about 64 KiB", but a segment is at
   most 128 KiB of body, so a 64 KiB threshold would pack only one or two
   files per segment. The threshold must sit well below the segment size;
   the archive used 4 KiB. **P0.1 decides it (likely 4–16 KiB) and
   corrects W125.** Larger files keep per-file chunks.
2. **Fragmentation.** The archive has no live-byte accounting and no
   compaction (W125 rules 2 and 3). This is the largest new component (P4).
3. **Lifetime grouping.** The archive's cohort groups by publication
   group. W125 rule 1 needs a key of directory, writer session and creation
   window (P3).
4. **Reclaim path.** Archived packed GC drives per-object, per-member RPCs.
   W125 requires W124's batched journal-first protocol (P2).
5. **Accounting.** Packed bytes must use W124 part 2's logical quota,
   reclaim-pending and physical reservation, at record granularity for the
   logical quota and segment granularity for physical space (P2, P4).
6. **No switch.** The opt-in, the capability negotiation and every
   unpacked small-file path go away (P3, P6).
7. **Drift since 2026-10-16.** The port must reconcile with devel changes
   made after the archive:
   - `h->mu` steps 1–3 (apply-ring seqlock, routing view, write-path
     profiler);
   - GC and query catalog changes (`a2d869fde`, the 60 s refresh,
     `f91afa40a`);
   - the restarted-member cooldown fixes (`e88ddc2ee`);
   - storage-root fencing (`e04a80f63`);
   - Raft log rotation (`8ad79fcd5`);
   - KV changes.

## Phases

### P0 — Decisions and measurements (no product code)

1. **Threshold:** measure segment fill, read latency and space for 256 B,
   1 KiB, 4 KiB, 8 KiB and 16 KiB records. Choose the packing threshold
   and the per-segment record cap, and correct W125.
2. **Grouping key and window:** the exact key (directory inode, session,
   window), and what seals a segment early (fsync, close, deadline,
   capacity, memory pressure).
3. **Compaction parameters:** the threshold *t*, the dead-bytes limit, the
   candidate-backlog and retained-segment caps per node, and the
   backpressure delay.
4. **Ledger extension format:** the live-byte and live-count fields, the
   compaction phase, and the cold-segment marker. These are on-disk format
   changes, versioned.
5. **Interaction with W123** (durable versions) and with the read-only
   lease edges measured on 2026-10-18.

**Exit:** the decisions are recorded on the W125 page and reviewed.

### P1 — Port format, metadata and apply

- **Revert onto the branch:** take the packing parts of `5ded11179` and
  `06db13c9c` (segment, codec, ledger, mapping, admission, meta-apply
  packed publication, protocol structs). Exclude the container engine.
- **Restore names:** the names for the reserved KV kinds, Raft tags,
  messages and the chunk flag.
- **Resolve drift** in `meta_apply.c`, `raft_host.c` and `kv_lsm.c`.
- **Tests:** restore `test_packed_segment`, `test_pack_ledger`,
  `test_pack_ledger_recovery`, `test_pack_mapping`, `test_pack_admission`,
  `test_packed_metadata`, `test_pack_snapshot` and
  `test_packed_publication`.

**Exit:** `make test` PASS. No client path creates packed data yet.

### P2 — Storage node and reclaim integration

- **Restore:** `PUT_PACKED`, `GET_PACKED` and `GETCHUNKS_PACKED`, the
  packed authority check and the per-object fence.
- **Reclaim through W124:** a retired segment becomes an entry in W124's
  node reclaim journal (accept, fence, physical deletion, retire record),
  not a per-object `GC_PACKED` RPC. The leader batches segment retirements
  per owning group, as W124 does for inodes.
- **Accounting:** each segment's fragments count against physical
  availability and reservations (W124 part 2).
- **Tests:** restore `test_packed_store`, `test_packed_upload`,
  `test_packed_gc`, `test_packed_gc_budget` and `test_pack_authority`, and
  add crash injection at each handoff: accepted, fenced, deleted, retired.

**Exit:** a private cluster can store, read and reclaim packed objects
written by a test driver.

### P3 — Client write and read path

- **Port the writer:** `pack_write.c` and `packed_reader`, without
  `EFS_PACKED_WRITES` and capability negotiation. Every file under the
  threshold at publication time is packed.
- **Lifetime grouping in the cohort:** one open cohort per (directory,
  session), sealed by window deadline, capacity, fsync or close, or
  memory pressure.
- **fsync:** fsync of a packed file seals and makes durable its cohort,
  with the same contract as today's per-file durable ACKs, including
  grouped barriers.
- **Publication:** the batch publication RPC for a sealed cohort.
- **Tests:** restore the mounted tests (`test_mounted_pack_batch`,
  `_parallel`, `_flush_retry`, `_retry`, `_admission`),
  `test_packed_read_budget`, `test_packed_writer_view_reuse` and
  `test_packed_publication_progress`. Add grouping and seal tests, and a
  memory-budget test.

**Exit:** a mounted client writes, reads and deletes small files through
packing only. POSIX core PASS on a private cluster.

### P4 — Live-byte accounting and compaction (new)

- **Counters:** the ledger tracks live bytes and live files, updated in
  the same Raft entry as each delete, overwrite or release.
- **Candidates:** a segment whose live share is below *t*, or whose dead
  bytes exceed the limit. Selection uses dead space per byte copied
  (cost/benefit order).
- **Copy forward:**
  - read the survivors and pack them into cold segments (never mixed with
    fresh data);
  - make the copies durable;
  - switch each survivor's reference with the mapping planner's CAS on
    its generation (a concurrent overwrite or delete wins, and the copy is
    cancelled through admission).
- **Retire:** the old segment's live count reaches zero, readers and
  leases that could reach it drain, and it goes to P2's batched reclaim.
- **Merging:** several underfilled sealed segments are compacted together
  into one.
- **Limits:** compaction runs on a rate-limited budget below foreground
  I/O. As the backlog or retained-segment caps near, its priority rises.
  At the caps, packed writes get bounded backpressure, then ENOSPC with
  the reason. Compaction copies use W124's recovery headroom.
- **Tests:**
  - unit tests for selection and bounds;
  - crash injection before the copy is durable, after it, before the
    switch, after it, and before retirement;
  - concurrent overwrite or delete against compaction;
  - the fragmentation gate (random 15-of-16 deletes, whole-tree deletes,
    overload) on a private cluster.

**Exit:** the fragmentation and overload gates PASS on a private cluster.

### P5 — Size transitions

A file that crosses the threshold changes storage class as a new immutable
generation:
- **growing** (write or append past the threshold): packed to per-file
  chunks;
- **shrinking** (truncate or overwrite below it): chunks to packed;
- **sparse files and holes:** defined explicitly.

The old copy becomes dead space and is counted or reclaimed normally.
Tests cover every transition, crashes mid-transition, and concurrent
readers.

**Exit:** the transition matrix PASS.

### P6 — Remove the old small-file path

- **Remove:** every code path that stores a sub-threshold file as
  per-file fragments, and any remaining packing opt-in or capability
  code.
- **Guard test:** a source test asserts there is no such path and no
  switch (W125 landing gate).
- **Docs:** update `architecture.md` (the small-file paragraph and
  placement table) and `protocols/data.md` to the packed design.

**Exit:** `make test` PASS, with the source test included.

### P7 — Qualification and landing

1. **Private node9901 A/B:** the branch build against a build of its base
   commit. Run W125's space, fragmentation, performance (the user's
   tiny-file recipe), reclaim and correctness gates, including crash
   injection at every handoff. Evidence page per AGENTS.md.
2. **orcd-mini,** with the user's go-ahead. Existing stores hold today's
   per-file fragments; those stay readable, and new small files are
   packed. A migration of existing small files is a separate optional step
   (a compaction-style background rewrite) and needs its own decision.
   Then the POSIX, two-client, durability, GC and expiry suites, and the
   user's workloads.
3. **Landing:**
   - tag the base devel commit `archive/unpacked-small-files-<date>`;
   - merge the branch;
   - update D37's register entry, the W125 status and the status index.

## Risks

- **Port size:** about 4,000 lines of archived packing code plus the new
  compaction, crossing client, server, metadata and the wire. Phases P1–P3
  are a port; P4–P5 are new design work.
- **Metadata pressure:** packing replaces fragment I/O with more metadata
  work (references, ledger updates, compaction switches) on Raft groups
  where proposals already lead the lock waits. P0.5 and P7 must measure
  it.
- **Existing data:** orcd-mini holds millions of per-file small fragments.
  Without migration, those keep their cost until they are deleted.
- **Memory:** client cohorts hold up to 128 KiB of payload each. The
  budget and the seal-on-pressure rules matter for many open directories.

## Open questions

- The final threshold and *t* (P0).
- Whether segments from different directories may ever share a stripe,
  for example cold compaction output across directories (P0.2 and P4).
- Whether existing per-file small fragments are migrated, and when (P7).
