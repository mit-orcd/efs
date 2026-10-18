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
| Client coalescing | `include/efs/pack_cohort.h`, `src/common/pack_cohort.c` (56) | Fixed-capacity cohort (128 records), deadline, seal, immutable retry | **Keep and change:** keep its one-publication-group ownership, and add the lifetime key inside it (P3). |
| Client writer | `src/client/pack_write.c` (666) | Mounted packed write path behind `EFS_PACKED_WRITES` | **Port without the switch.** It becomes the only path for files under the threshold. |
| Packed reads | `src/common/packed_reader.c` (56), `src/client/read.c` changes | Fetch a segment fragment, decode, verify the reference digest | **Keep.** |
| Storage node | `src/server/packed_upload.c` (110), `packed_fence.h` (160), `packed_gc.h` (115), `pack_authority.h` (102), `store.c` and `handler.c` parts | PUT, GET and GC of packed objects, authority checks, per-object fences | **Keep and change:** packed GC uses W124's batched, journal-first node protocol, not its own per-object `GC_PACKED` round trips (P2). |
| Metadata apply | `src/meta/meta_apply.c` (234 lines removed), Raft tags 45–51 | Packed publication, PACK profile, packed chunk extension, packed retirement | **Keep.** Restore the reserved numbers' names. |
| Wire | `include/efs/protocol.h`: messages 131–144, chunk flag `EFS_CHUNK_REC_F_PACKED_BASE` (8) | PUT, GET, GETCHUNKS, GC, capabilities and authority messages | **Keep.** Numbers are still reserved on devel ("131-150 are reserved"). Keep the capability negotiation, or replace it with an enforced client handshake (gap 8), because clients are **not** build-gated today. |
| Tests | 46 packing/container tests, 2 stress tests | Unit, mounted and stress | **Restore** the packing ones (list in P1–P3). Drop the container-only ones and those testing the opt-in switch. |

Devel keeps fail-closed checks for stores these features wrote: the KVLSM2
reader requirement, root format byte 1, KV kinds 36–39, coding profile 2,
Raft tags 45–51, messages 131–144 and chunk flag 8. The port turns those
refusals back into real handling. This is also the compatibility story: a
store written by the archived code is either readable or refused, never
misread.

## Gaps between the archive and W125

1. **Packing threshold.** It is a hypothesis to measure, not a format
   necessity.
   - A 128 KiB segment cannot hold two 64 KiB records once headers, the
     record table and digests are included, so 64 KiB packs at most one
     file per segment.
   - The real format constraint is the archived validator's 4 KiB record
     limit (`EFS_PACK_MAX_PAYLOAD`, checked in `packed_segment.c`). Raising
     it is a format and compatibility decision (gap 8).
   - **P0.1 measures 4, 8, 16, 32 and 64 KiB candidates** and decides.
     Larger files keep per-file chunks.
2. **Fragmentation.** The archive has no live-byte accounting and no
   compaction (W125 rules 2 and 3). This is the largest new component (P4).
3. **Lifetime grouping.**
   - The archive keeps one cohort per metadata (publication) group, and
     every segment belongs to exactly one publication group: its ledger
     and references assume it.
   - The grouping key must therefore be (publication group, directory,
     writer session, creation window), with the lifetime key added
     *inside* the group boundary.
   - Cold compaction must preserve the boundary too, unless cross-group
     ledger updates get a separate protocol (out of scope).
4. **Reclaim path.** Archived packed GC drives per-object, per-member RPCs.
   W125 requires W124's batched journal-first protocol (P2).
5. **Accounting.** Packed bytes must use W124 part 2's logical quota,
   reclaim-pending and physical reservation, at record granularity for the
   logical quota and segment granularity for physical space (P2, P4).
6. **No switch.** The opt-in and every unpacked small-file *write* path go
   away (P3, P6). Legacy reads stay: existing stores hold per-file small
   fragments.
7. **Close-time coalescing.** The archived writer makes close and fsync
   wait for durable publication (`pack_write.c`: "Close/fsync require
   consumed durable publication"), with a 10 ms cohort deadline.
   - A single writer doing create, write and close in sequence therefore
     publishes one file per segment, because the next file cannot join a
     cohort its predecessor's close is waiting on.
   - The target workload looks exactly like that (`tiny_files.py` workers
     write and close one file at a time).
   - A longer window cannot help that writer. Its next file only exists
     after its close returns, so waiting longer only adds latency.
   - Coalescing must come from *concurrent* writers or directories in the
     same publication group, or from accepting singleton segments and
     merging them later by compaction.
   - P0 must choose explicitly (see P0.0). Any change that lets close
     return before publication needs defined cross-client visibility,
     error reporting and fsync semantics.
8. **Format compatibility.**
   - Removing the switch does not remove version checks.
   - **Clients are not gated today.** `EFS_MSG_HELLO` is sent only by
     daemons joining the cluster (`src/server/cluster.c`), and the build-ID
     check in `src/server/handler.c` applies to those. efs-fuse sends no
     HELLO and only logs its build ID. So an old client can meet a new
     server, and the reverse.
   - **Enforcement point: the server, on every connection.** A client-side
     capability query only protects a new client meeting an old server; an
     old client never asks. So:
     - every client connection, including pooled connections and
       reconnects, must send a protocol-version declaration as its first
       message;
     - the server serves no data read or mutation (GET, GETCHUNKS, PUT,
       publication, packed or not) on a connection until a compatible
       version is declared;
     - an undeclared or incompatible connection is refused with a distinct
       error and closed, so an old client fails cleanly instead of misreading
       packed references or writing per-file small fragments;
     - a new client meeting an old server gets an unknown-message error to
       its declaration and refuses to proceed.
   - Old-client against new-server and new-client against old-server
     combinations are tested, including the pool reusing a connection and
     reconnecting after a server restart. The archived capability
     negotiation is only replaced once these tests pass.
   - Stores are not gated either: an older or newer build may meet a store
     written with the restored or extended format.
   - Every on-disk format the port touches carries a version and is
     refused, fail closed, by a build that does not support it. That
     covers the segment record-size limit, the ledger extension
     (live-byte counters, compaction phase, cold marker) and the reserved
     KV kinds.
   - Raising the record-size limit and extending the ledger get an explicit
     audit. Upgrades are coordinated: daemons by the build-ID gate, clients
     by the handshake or negotiation above. Downgrades across a format
     change are refused.
9. **Drift since 2026-10-16.** The port must reconcile with devel changes
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

**Status 2026-10-18:** measurements done; decisions proposed and awaiting
review ([evidence](../status/w125-p0-20261018.md)). The result is a
conditional GO for option (a) with singleton segments, no seal window and
merge compaction. The condition is that a singleton's publication costs no
more than today's close, to be measured first on the prototype.

0. **Allocation and cost go/no-go (before any port work).** Prove that
   ordinary create, write and close gets a real space win at acceptable
   cost before investing in P1–P7.
   - **Model causality, not replayed timestamps.**
     - Each worker is a closed loop: create, write, then close, and its next
       file starts only after its close returns.
     - Under "close waits for publication", a close's latency includes
       waiting for its segment to seal and publish.
     - Replaying timestamps from today's faster baseline would invent
       batching that cannot happen, so the model (or a prototype) feeds
       per-worker service times from a capture of the actual
       `tiny_files.py` recipe: 16 workers, a single writer, frequent
       fsync, and the user's mixed workload.
     - Use a trace option in a throwaway build or an equivalent capture,
       not a change to devel.
   - **Choose the close contract explicitly:**
     - **(a)** close waits for publication, sealing on a short window
       shared by concurrent writers. Singleton segments are accepted for
       sequential writers, and merged later by compaction. **The first
       experiment uses (a),** keeping today's close contract, so that
       packing's storage benefit is measured separately from any change to
       visibility and error semantics.
     - **(b)** close returns earlier, which needs defined cross-client
       visibility, error reporting and fsync behaviour before it can be
       chosen.
   - **Decision metrics, not occupancy alone.** Segments are variable-size
     (8 KiB alignment), so a singleton need not allocate a full stripe.
     - **Estimate for a 256 B file alone in a segment:** an 8 KiB segment
       gives a 4 KiB fragment per node, plus the 4 KiB digest and alignment
       tail that direct-I/O fragment files carry (today's 69,632-byte files
       are 65,536 + 4,096). That is 8 KiB per node and 24 KiB across three
       nodes, before filesystem metadata.
     - That is about 8.5× smaller than 69,632 bytes per node, but still
       about 96× the 256-byte payload.
     - This is an estimate. Measure the real cost.

     Measure:
     - allocated bytes per file from `st_blocks` and filesystem allocation
       (`df` or XFS free-space deltas), not fragment payload length,
       initially and after compaction settles. Include the ledger, the
       reclaim journal and segment metadata;
     - metadata (Raft) operations per file, including publication, ledger
       and compaction switches;
     - close latency p50/p99;
     - compaction traffic (bytes copied and Raft entries per file).

     Go only if allocated bytes improve substantially and close latency and
     metadata cost stay within agreed limits. Low occupancy can still save
     space, and high occupancy can still be too expensive.

1. **Threshold:** for record sizes of 256 B, 1, 4, 8, 16, 32 and 64 KiB,
   measure:
   - physical allocation per logical byte;
   - segment fill;
   - read amplification (bytes fetched per byte read);
   - metadata cost per file.

   Choose the packing threshold and the per-segment record cap. That
   includes whether to raise the archived 4 KiB validator limit (gap 8).
   Correct W125.
2. **Grouping key, window and close contract:**
   - the exact key (publication group, directory, session, window);
   - what close must publish for visibility and the fsync contract;
   - whether closes coalesce across concurrent writers;
   - what seals a segment (deadline, capacity, fsync, memory pressure).

   Use P0.0's results.
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
- **Lifetime grouping in the cohort:** one open cohort per (publication
  group, directory, session), with close semantics as decided in P0.2.
  Sealing is by window deadline, capacity, fsync or memory pressure, not
  by every close unless P0 shows that is acceptable.
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
  - switch each survivor's reference with the mapping planner's CAS on the
    **exact expected source reference** (source segment id and generation,
    offset, length, digest). A generation alone is not enough, because
    relocation preserves the content generation.
  - In the same atomic batch, the destination segment's ledger takes
    ownership of the record and the source's ledger releases it.
  - A concurrent overwrite, delete or second compactor that already moved
    the record makes the CAS fail. The copy is then cancelled through
    admission.
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
  - two compactors racing on the same record: exactly one switch wins,
    and the loser's copy is cancelled and reclaimed;
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

- **Remove:** every code path that *writes* a sub-threshold file as
  per-file fragments, and any remaining packing opt-in. Legacy reads of
  existing per-file small fragments stay, as do large-file writes.
- **Behavioural landing tests**, checked through persisted mappings and a
  physical fragment inventory on a private cluster (a source scan is at
  most a supplementary hint):
  - fresh sub-threshold files are packed, and no per-file fragment is
    created;
  - files above the threshold get per-file chunks;
  - threshold transitions in both directions;
  - legacy per-file small files are still readable, and a modification
    rewrites them packed;
  - empty files create no segment.
- **Docs:** update `architecture.md` (the small-file paragraph and
  placement table) and `protocols/data.md` to the packed design.

**Exit:** `make test` PASS, plus the behavioural landing tests.

### P7 — Qualification and landing

1. **Private node9901 A/B:** first rebase the branch onto the then-current
   devel, so concurrent fixes are on both sides. Then compare the branch
   build against a build of exactly that devel commit, which step 3 tags.
   An older base would conflate packing with unrelated changes. Run W125's space, fragmentation, performance (the user's
   tiny-file recipe), reclaim and correctness gates, including crash
   injection at every handoff. Evidence page per AGENTS.md.
2. **orcd-mini,** with the user's go-ahead. Existing stores hold today's
   per-file fragments; those stay readable, and new small files are
   packed. A migration of existing small files is a separate optional step
   (a compaction-style background rewrite) and needs its own decision.
   Then the POSIX, two-client, durability, GC and expiry suites, and the
   user's workloads.
3. **Landing:**
   - tag the actual pre-merge devel commit, the same one the final A/B
     used as its baseline, as `archive/unpacked-small-files-<date>`;
   - merge the branch;
   - update D37's register entry, the W125 status and the status index.

## Risks

- **Cost per file (largest practical risk):** if ordinary create/write/close
  yields mostly singleton segments, the space win may survive (variable
  segment size), but the extra Raft publication and compaction traffic, or
  the close latency, may not be acceptable. P0.0 decides from allocated
  bytes, metadata operations, close latency and compaction traffic.

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

## Review (2026-10-18)

Review of `2e9a6d186` against the archived segment format and writer.
Claims were checked in the archive (`pack_write.c`, `packed_segment.c`) and
in devel (`handler.c`).

| Finding | Disposition |
| --- | --- |
| Keep publication-group ownership in the grouping key; cold compaction keeps it. | **Accepted.** Confirmed: one cohort per metadata group, and every segment belongs to one publication group. The key is now (publication group, directory, session, window). |
| The threshold correction is a hypothesis; the 4 KiB validator limit is the real format constraint. | **Accepted.** P0.1 measures 4–64 KiB for allocation, fill, read amplification and metadata cost. Raising the limit is a format decision (gap 8). |
| Close-triggered sealing may defeat the target workload. | **Accepted, and made the go/no-go.** Confirmed that the archived close waits for durable publication (10 ms deadline), which gives one file per segment for a sequential writer. P0.0 measures occupancy under the real `tiny_files.py` recipe (16 workers, one writer, frequent fsync) before the port. |
| Removing the switch does not remove compatibility checks. | **Accepted.** My first answer kept negotiation dropped on the grounds that HELLO gates clients. That was wrong (see the follow-up below). On-disk formats get explicit versions and fail-closed refusal, with coordinated upgrades and refused downgrades (gap 8). |
| The compaction CAS must identify the exact old mapping; test racing compactors. | **Accepted.** The CAS is on the exact source reference, with destination ownership and source release in one batch. A two-compactor race test is added. |
| Replace the source-only landing guard with behavioural coverage. | **Accepted.** P6 checks persisted mappings and the physical inventory for fresh files, large files, transitions, legacy reads and modification, and empty files. Legacy reads stay. |
| Tag the actual pre-merge devel and compare against it after integration. | **Accepted.** Rebase before the final A/B; that same commit is the baseline and the tag. |

### Third review (2026-10-18)

| Finding | Disposition |
| --- | --- |
| The singleton estimate omits the direct-I/O tail: 8 KiB per node, not 4 KiB. | **Accepted; my error.** Verified that fragment files carry a 4 KiB digest and alignment tail (69,632 = 65,536 + 4,096). P0.0 now measures `st_blocks` and filesystem allocation, including ledger and journal. |
| Negotiation must exclude incompatible clients on the server, including pooled connections and reconnects. | **Accepted.** Gap 8 now requires a version declaration as the first message on every client connection, and no data read or mutation before it, with old/new tests that include pool reuse and reconnects. |
| The first experiment keeps the existing close contract. | **Accepted.** P0.0's first experiment uses option (a). |

### Follow-up review (2026-10-18)

| Finding | Disposition |
| --- | --- |
| HELLO does not gate clients. | **Accepted; my error.** Verified: only daemons send `EFS_MSG_HELLO` (`cluster.c`); the client only logs its build ID. Gap 8 now keeps capability negotiation, or an enforced client handshake, plus old/new client-server tests. |
| The P0 simulation must keep workload causality; a longer window cannot help a sequential writer. | **Accepted.** P0.0 models closed-loop workers and requires an explicit close contract: (a) singletons plus compaction, or (b) early close with defined semantics. |
| Occupancy alone is not the metric. | **Accepted.** P0.0 decides on allocated bytes per file (variable segment size), metadata operations, close latency and compaction traffic. |

## Open questions

- The final threshold and *t* (P0).
- Whether segments from different directories may ever share a stripe,
  for example cold compaction output across directories (P0.2 and P4).
- Whether existing per-file small fragments are migrated, and when (P7).
