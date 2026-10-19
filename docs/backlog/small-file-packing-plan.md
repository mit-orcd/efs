# W125 implementation plan — small-file packing

**Status:** PLAN, revised 2026-10-18 for **background packing after close**
(the user's choice after the
[early P1 measurement](../status/w125-p1-early-20261018.md)). No code
written; all phases and gates are **NOTRUN**.
Design and requirements: [W125](small-file-packing-default.md).
Prerequisite: [W124](fast-delete-reclaim.md) parts 1–2 (batched reclaim,
explicit accounting), done. Source of reusable code: tag
`archive/packing-containers-20261016` (branch `packing-containers`; see its
`ARCHIVE.md`). Recorded 2026-10-18.

## Ground rules

- **One write path:** files are written exactly as today (per-file chunk
  fragments, asynchronous PUT, unchanged close and fsync). No packed write
  path in the client.
- **Packing is always on:** a background packer moves every closed file
  under the threshold into packed segments. No runtime, build or
  environment switch disables it, and no tunable has an "off" value. Tests
  may shorten the minimum age or trigger a pass.
- **The old way survives only as a tag:** immediately before landing, tag
  the last devel commit without the packer as
  `archive/unpacked-small-files-<date>`.
- **Comparisons are between builds:** every A/B compares a build of that
  tag (or of the commit the work branch started from) with the branch
  build. There are never two modes in one binary.
- **No file data in metadata rows:** metadata holds only references
  (segment id, generation, offset, length, digest).
- **The container storage engine stays out:** `store_containers.c`, the
  `storage_engine` switch and the container-only paths are not ported.
- **One work branch from devel:** `w125-packing`, in the worktree
  `~/git/efs-w125`. Each phase ends with its tests passing on that branch.
  Nothing deploys to orcd-mini without the user's go-ahead.

## What the archive provides

The packing code removed in `5ded11179` (container engine excluded), and its
disposition under background packing:

| Component | Archived files | Role | Disposition |
| --- | --- | --- | --- |
| Segment format | `include/efs/packed_segment.h`, `src/common/packed_segment.c` (163 lines) | Immutable segment image: 96 B header, 72 B per record, ≤ 512 records; per-file references (96 B) with digest; build, validate and read | **Keep.** Raise `EFS_PACK_MAX_PAYLOAD` from 4 KiB to the 16 KiB threshold as a versioned format change. |
| Segment codec | `include/efs/erasure.h` (`efs_segment_geometry`, `efs_encode_segment`, `efs_decode_segment`) | Variable-size segment of up to 128 KiB (`EFS_SEGMENT_MAX_SIZE`), 8 KiB aligned, split 2+1 into ordinary fragments | **Keep**, so a segment stays one stripe of standard fragments. |
| Lifetime ledger | `include/efs/pack_ledger.h`, `src/common/pack_ledger.c` (144) | Per-segment KV object: per-record state (PENDING, LIVE, RELEASED, CANCELLED), phases ACTIVE, RECLAIMING, RETIRED, member fence and delete ACKs | **Keep and extend:** live-byte and live-count fields, a cold marker for compaction output. Today a segment retires only when every record is released, which is the pinned-hole problem. |
| Mapping planner | `include/efs/pack_mapping.h`, `src/meta/pack_mapping.c` (229) | Applies chunk-map changes to packed references together with ledger updates in one KV batch, under mapping CAS | **Keep and extend:** the basis of the packer's switch. It must accept a per-file chunk as the expected source, not only a packed reference. |
| Upload admission | `include/efs/pack_admission.h`, `src/meta/pack_admission.c` (270) | Upload manifests, session authority, upload floors, cancellation, ACKs | **Keep**, with the packer as the only uploader. Re-check against the current session and fence code. |
| Client coalescing | `include/efs/pack_cohort.h`, `src/common/pack_cohort.c` (56) | Fixed-capacity cohort (128 records), deadline, seal, immutable retry | **Do not port.** Grouping happens in the packer. |
| Client writer | `src/client/pack_write.c` (666) | Mounted packed write path behind `EFS_PACKED_WRITES` | **Do not port.** The write path stays as today. Pieces of its segment building may be reused by the packer. |
| Packed reads | `src/common/packed_reader.c` (56), `src/client/read.c` changes | Fetch a segment fragment, decode, verify the reference digest | **Keep.** Used by clients and by the packer when compacting. |
| Storage node | `src/server/packed_upload.c` (110), `packed_fence.h` (160), `packed_gc.h` (115), `pack_authority.h` (102), `store.c` and `handler.c` parts | PUT, GET and GC of packed objects, authority checks, per-object fences | **Keep and change:** segments live in their own per-group segment directory, and retirement uses W124's batched, journal-first node protocol, not per-object `GC_PACKED` round trips (P2). |
| Metadata apply | `src/meta/meta_apply.c` (234 lines removed), Raft tags 45–51 | Packed publication, PACK profile, packed chunk extension, packed retirement | **Keep what the switch and retirement need.** Restore the reserved numbers' names. Client-driven packed publication is not ported. |
| Wire | `include/efs/protocol.h`: messages 131–144, chunk flag `EFS_CHUNK_REC_F_PACKED_BASE` (8) | PUT, GET, GETCHUNKS, GC, capabilities and authority messages | **Keep the read and storage messages.** Numbers are still reserved on devel ("131-150 are reserved"). Replace client capability negotiation with an enforced client handshake (gap 8), because clients are **not** build-gated today. |
| Tests | 46 packing/container tests, 2 stress tests | Unit, mounted and stress | **Restore** the format, ledger, mapping, admission, storage-node and read tests (list in P1–P3). Drop the container-only ones, the client-writer and cohort tests, and those testing the opt-in. |

Devel keeps fail-closed checks for stores these features wrote: the KVLSM2
reader requirement, root format byte 1, KV kinds 36–39, coding profile 2,
Raft tags 45–51, messages 131–144 and chunk flag 8. The port turns those
refusals back into real handling. This is also the compatibility story: a
store written by the archived code is either readable or refused, never
misread.

## Gaps between the archive and W125

1. **Packing threshold.** Initially 16 KiB (P0). The archived validator's
   4 KiB record limit (`EFS_PACK_MAX_PAYLOAD`, checked in
   `packed_segment.c`) must be raised, a format and compatibility decision
   (gap 8). A 128 KiB segment holds at most one 64 KiB record, so 64 KiB
   would pack one file per segment. Larger files keep per-file chunks.
2. **Fragmentation.** The archive has no live-byte accounting and no
   compaction (W125 rules 2 and 3). Under background packing, compaction
   and initial packing are the same copy-forward engine (P4).
3. **Lifetime grouping.** The archive keeps one cohort per metadata
   (publication) group, and every segment belongs to exactly one
   publication group: its ledger and references assume it. The packer's
   grouping key is therefore (publication group, directory subtree,
   creation epoch), with the lifetime key *inside* the group boundary.
   Compaction preserves the boundary too, unless cross-group ledger updates
   get a separate protocol (out of scope).
4. **Reclaim path.**
   - Archived packed GC drives per-object, per-member RPCs. W125 requires
     W124's batched journal-first protocol (P2).
   - Packing a staged file supersedes a generation of a *live* inode. Today
     superseded fragments go through the per-fragment GC-record pass
     (`host_gc_frag_pass`, 128 acks per `GC_ACK` entry), which W124 left
     unchanged. Packing, and above all the backfill of millions of existing
     small files, makes it the main reclaim load. W124's node journal is
     extended to superseded generations, and to removing an inode's emptied
     chunk and inode directories.
5. **Accounting.** Packed bytes use W124 part 2's logical quota,
   reclaim-pending and physical reservation. Logical quota counts a file
   once, in whichever form, so a switch changes no logical quota. Physical
   space is reserved per segment before the copy and released by
   retirement (P2, P4).
6. **No switch.** The opt-in, the client writer and the client cohort are
   not ported (nothing to remove later). Reads keep both forms: staged and
   legacy per-file small files, and packed references.
7. **Close-time coalescing — resolved by the design.** The archived writer
   made close wait for durable publication, which gave one file per segment
   for a sequential writer and failed the early P1 measurement. The packer
   works after close on whole closed directories, so close is unchanged and
   segments fill.
8. **Format compatibility.**
   - Removing the switch does not remove version checks.
   - **Clients are not gated today.** `EFS_MSG_HELLO` is sent only by
     daemons joining the cluster (`src/server/cluster.c`), and the build-ID
     check in `src/server/handler.c` applies to those. efs-fuse sends no
     HELLO and only logs its build ID. So an old client can meet a new
     server, and the reverse.
   - Clients do not write packed data, but they must read packed
     references. An old client must not misread them.
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
       error and closed, so an old client fails cleanly instead of
       misreading packed references;
     - a new client meeting an old server gets an unknown-message error to
       its declaration and refuses to proceed.
   - Old-client against new-server and new-client against old-server
     combinations are tested, including the pool reusing a connection and
     reconnecting after a server restart.
   - Stores are not gated either: an older or newer build may meet a store
     written with the restored or extended format.
   - Every on-disk format the port touches carries a version and is
     refused, fail closed, by a build that does not support it. That
     covers the segment record-size limit, the ledger extension (live-byte
     counters, cold marker), the candidate index and the reserved KV kinds.
   - Upgrades are coordinated: daemons by the build-ID gate, clients by the
     handshake above. Downgrades across a format change are refused.
9. **Drift since 2026-10-16.** The port must reconcile with devel changes
   made after the archive:
   - `h->mu` steps 1–3 (apply-ring seqlock, routing view, write-path
     profiler);
   - GC and query catalog changes (`a2d869fde`, the 60 s refresh,
     `f91afa40a`);
   - the restarted-member cooldown fixes (`e88ddc2ee`);
   - storage-root fencing (`e04a80f63`);
   - Raft log rotation (`8ad79fcd5`);
   - W124 (reclaim journal, MD 71/72, wire 155/156);
   - KV changes.
10. **New components** with no archived counterpart: the candidate index,
    the packer (selection, copy, switch, retirement), superseded-generation
    retirement in the node journal, and the backfill scan.

## Phases

### P0 — Decisions and measurements (no product code)

**Status 2026-10-18:** measurements done
([P0 evidence](../status/w125-p0-20261018.md),
[early P1 evidence](../status/w125-p1-early-20261018.md)).

**Superseded by the user's choice of background packing (2026-10-18):**
- option (a) with no seal window, singleton segments at close, and fused
  admission and publication for singletons: there is no packing at close.
  The close contract is today's, unchanged.
- The GO condition on singleton publication cost no longer applies. The
  early P1 measurement showed it failing for the archived code (close
  3.53 ms against 0.65 ms, 9.9 Raft entries per file against 4.0).

**Still accepted:**
- the 16 KiB initial threshold, raising the 4 KiB record limit as a
  versioned format change;
- lifetime grouping by publication group, directory subtree and creation
  epoch, now applied by the packer;
- merging underfilled segments and hole compaction through one
  copy-forward engine, with batched retirement through W124.

**P0 measurements that still apply:** a packed full segment fragment costs
70,166 B per segment per node on XFS with shared directories, against
71,241 B per file today (including two directories and three inodes). A
256 B file in a full segment costs about 175 B of segment space (model).

**New decisions to record before P4** (proposed values, for review):
1. **Minimum age:** 60 s since the last data publication, with the file
   closed (hypothesis; measured in P4).
2. **Grouping:** (publication group, directory subtree, creation epoch of
   10 minutes); a segment is filled to its record or byte cap from one
   group, then the next.
3. **Packer placement:** one packer per publication group on its leader,
   soft state, restart from the candidate index.
4. **Budget:** a rate limit and a concurrency cap per node, with back-off
   on foreground latency; priority rises near the caps.
5. **Compaction parameters:** *t* = 50%, the dead-bytes limit, and the
   per-node caps on compaction backlog, retained sources and staged bytes.
6. **Ledger and index formats:** the live-byte and live-count fields, the
   cold marker and the candidate-index key, all versioned.
7. **Interaction with W123** (durable versions) and with the read-only
   lease edges measured on 2026-10-18.

**Exit:** the decisions are recorded on the W125 page and reviewed.

### P1 — Port format, metadata and apply

- **Port onto the branch:** the packing parts of `5ded11179` and
  `06db13c9c` needed by background packing: segment format, codec, ledger,
  mapping planner, admission, meta-apply switch and retirement, protocol
  structs. Selectively; a full revert conflicts in 42 files. Exclude the
  container engine, the client writer and the client cohort.
- **Restore names:** the names for the reserved KV kinds, Raft tags,
  messages and the chunk flag.
- **Raise the record limit** to 16 KiB with a format version.
- **Extend the mapping planner** so a switch can name a per-file chunk
  (inode, chunk index, generation, fragment-set identity, digest) as its
  expected source.
- **Resolve drift** in `meta_apply.c`, `raft_host.c` and `kv_lsm.c`.
- **Tests:** restore `test_packed_segment`, `test_pack_ledger`,
  `test_pack_ledger_recovery`, `test_pack_mapping`, `test_pack_admission`,
  `test_packed_metadata` and `test_pack_snapshot`. Add switch tests from a
  per-file chunk, including a failed CAS that cancels one member.

**Exit:** `make test` PASS. Nothing creates packed data yet.

**Status 2026-10-18: done on branch `w125-packing`** (`e4bec5ce8`,
`155aac909`; not merged, not deployed).
- Segment v2 (16 KiB records), ledger v2 (record lengths, derived
  live/dead counters, cold flag), segment codec.
- Packed chunk profile 3 and segment object kind 45 (the retired profile 2
  and kinds 36–41 stay refused); release planner on every path that drops
  or replaces a packed row.
- MD 73–75 (admit, switch with the exact-source CAS, cancel); the store
  requires reader PACK_V2 before the first packed row.
- GETCHUNKS and truncate-tail planning refuse packed rows until P3/P5.
- node9901: full `make test` PASS (`ASAN_OPTIONS=detect_leaks=0`, because
  node9901's `ptrace_scope=2` stops LeakSanitizer). Log outside Git:
  `/tmp/w125-p1/maketest2.log`, sha256 prefix `ad5aaf2ead91a5bb`.
- Restored archive tests not ported yet: `test_pack_mapping`,
  `test_pack_admission`, `test_packed_metadata`, `test_pack_snapshot`
  (their client-publication cases no longer apply). `test_pack_switch`
  covers switch, cancel, overwrite and sweep release.

### P2 — Storage node and reclaim integration

- **Restore:** `PUT_PACKED`, `GET_PACKED` and `GETCHUNKS_PACKED`, the
  packed authority check and the per-object fence. Segments go to a
  per-group segment directory on each node, never under an inode
  directory.
- **Reclaim through W124:**
  - a retired segment becomes an entry in W124's node reclaim journal
    (accept, fence, physical deletion, retire record), not a per-object
    `GC_PACKED` RPC;
  - a superseded per-file generation is a new journal record kind: the
    node deletes exactly that generation's fragment files, then the chunk
    and inode directories once empty;
  - the leader batches both per owning group, as W124 does for inodes.
- **Accounting:** segment fragments count against physical availability
  and reservations (W124 part 2).
- **Tests:** restore `test_packed_store`, `test_packed_upload`,
  `test_packed_gc`, `test_packed_gc_budget` and `test_pack_authority`. Add
  superseded-generation retirement (including an inode overwritten while
  its old generation is queued), and crash injection at each handoff:
  accepted, fenced, deleted, retired.

**Exit:** a private cluster can store, read and reclaim packed objects
written by a test driver, and retire a superseded per-file generation.

**Status 2026-10-18: code and unit tests done on `w125-packing`**
(`f7e85d29c`; not merged, not deployed).
- Segment store in its own tree (`data/segments/g<group>/…`), fsynced
  before the PUT is acknowledged; PUT authority by a linearizable read of
  the segment object. Wire 157–164 (131–150 stay never-reuse).
- Reclaim journal v2 (128-byte records with a kind; v1 replays): SEGMENT
  and SUPERSEDED kinds, segment rescan in fail-closed recovery.
- Leader: segment retirement pass plus MD 76 PACK_RETIRE; the fragment GC
  pass now sends one GC_FRAG_BATCH per node per window instead of one
  delete RPC per fragment.
- node9901: full `make test` PASS (`detect_leaks=0`; log sha256 prefix
  `0423bf65f5d6ec66`, outside Git).
- **Not yet run:** the private-cluster exit check. Nothing proposes
  ADMIT/SWITCH yet; the packer (P4) is the driver, so the cluster check
  moves to P4's early measurement.

### P3 — Client read path and handshake

- **Packed reads:** port `packed_reader` and the `read.c` changes. A
  client reads both packed references and per-file chunks. No packed
  write path.
- **Handshake:** the server-enforced protocol-version declaration (gap 8).
- **Tests:** restore `test_packed_read_budget`. Add mounted reads of files
  switched by a test driver, reads racing a switch, and old/new
  client-server combinations including pool reuse and reconnects.

**Exit:** a mounted client reads packed and per-file small files; POSIX
core PASS on a private cluster.

### P4 — The packer (new): candidates, copy, switch, compaction

- **Candidate index:** recorded in the publication's Raft entry for closed
  files below the threshold; removed on pack, delete or growth. Backfill
  scan for existing stores, rate-limited.
- **Selection:** eligible candidates (closed, minimum age, not pinned by a
  lease) grouped by (publication group, directory subtree, creation
  epoch); compaction candidates (live share below *t* or dead bytes over
  the limit) ordered by dead space per byte copied.
- **Copy forward:** read the sources through the verified read path,
  build the segment, PUT it, admit its records as pending, and wait until
  it is durable on all k+f nodes.
- **Switch:** one Raft entry per segment, with a CAS on each member's exact
  source (staged file or segment, as the design page defines). In the same atomic batch the destination
  ledger marks switched records live and the source releases them. A
  member whose CAS fails is cancelled and counted dead.
- **Retire:** sources go to P2's batched reclaim once readers and leases
  that could reach them have drained.
- **Counters:** the ledger tracks live bytes and live files, updated in the
  same Raft entry as each delete, overwrite, truncate or switch.
- **Limits:** a rate-limited budget below foreground I/O, with back-off on
  foreground latency. As the caps near, priority rises. Copies use W124's
  compaction headroom. No foreground write is delayed for packing.
- **Early measurement (as soon as packing works end to end):** on the
  private node9901 cluster, foreground `tiny_files.py` throughput and close
  latency with the packer running, against a build of the base commit,
  plus files packed per second and the resulting space. Recorded as an
  evidence page before the remaining P4 work.
- **Tests:**
  - unit tests for selection, grouping and bounds;
  - crash injection before the copy is durable, after it, before the
    switch, after it, and before retirement;
  - concurrent write, truncate, delete and read against the packer;
  - two packers or compactors racing on the same file or record: exactly
    one switch wins, and the loser's copy is cancelled and reclaimed;
  - leader change mid-pass;
  - the fragmentation and space-pressure gates (random 15-of-16 deletes,
    whole-tree deletes, churn on a nearly full filesystem) on a private
    cluster.

**Exit:** the packing-rate, fragmentation and space-pressure gates PASS on
a private cluster.

**Status 2026-10-19: packer with budget, compaction and backfill on
`w125-packing` (`1b7f0d7d2`):** foreground within 10% while packing
(−1.2% / −4.3%, three runs), compaction and backfill checked on a private
cluster ([evidence](../status/w125-p4-budget-20261019.md)). Remaining in
P4: crash injection at each packer step, leader change mid-pass, the
24 h churn and space-pressure gates, tighter small-segment merging.

**Earlier (2026-10-18): first packer (`6df0a4d3c`).** Candidate index (KV kind 46,
MD 77 UNINDEX), packer thread, private-cluster check and
[early measurement](../status/w125-p4-early-20261018.md): 60,000 tiny files
in 156 segments (395× less space), whole-tree deletes retire segments
without copying, but foreground −12% (16 workers) and −20% (one writer)
while packing concurrently, above the 10% target. Next: rate budget and
back-off, repeated runs, compaction, backfill.

### P5 — Modifying packed files

Any modification of a packed file goes through today's write path as a new
per-file generation:
- **write and append:** read-modify-write of the packed bytes where a
  partial chunk needs them;
- **truncate,** to a smaller non-zero size, to zero, and extending;
- **sparse files and hole punch:** defined explicitly.

The old record becomes dead space, and the packer repacks the new
generation if it is still below the threshold. A file that grew past the
threshold stays per-file. Tests cover every case, crashes mid-modification,
and concurrent readers.

**Exit:** the modification matrix PASS.

### P6 — Landing checks

There is no old write path to remove: the write path is unchanged, and no
packed write path or opt-in was ported.
- **Behavioural landing tests,** checked through persisted mappings and a
  physical fragment inventory on a private cluster (a source scan is at
  most a supplementary hint):
  - sub-threshold files are packed after the minimum age, and their
    per-file fragments and inode directories are retired;
  - files above the threshold keep per-file chunks;
  - a modified packed file is rewritten per file and repacked;
  - legacy per-file small files are backfilled and packed;
  - empty files create no segment;
  - no switch, environment variable or tunable value disables the packer.
- **Docs:** update `architecture.md` (the small-file paragraph and
  placement table) and `protocols/data.md` to the packed design.

**Exit:** `make test` PASS, plus the behavioural landing tests.

### P7 — Qualification and landing

1. **Private node9901 A/B:** first rebase the branch onto the then-current
   devel, so concurrent fixes are on both sides. Then compare the branch
   build against a build of exactly that devel commit, which step 3 tags.
   An older base would conflate packing with unrelated changes. Run W125's
   write-path, space, packing-rate, fragmentation, read, reclaim and
   correctness gates, including crash injection at every handoff. Evidence
   page per AGENTS.md.
2. **orcd-mini,** with the user's go-ahead. Its existing per-file small
   files are backfilled and packed in the background, rate-limited (about
   4.6 M files, about 298 GiB per node to free). Then the POSIX, two-client,
   durability, GC and expiry suites, and the user's workloads.
3. **Landing:**
   - tag the actual pre-merge devel commit, the same one the final A/B
     used as its baseline, as `archive/unpacked-small-files-<date>`;
   - merge the branch;
   - update D37's register entry, the W125 status and the status index.

## Risks

- **Foreground impact (largest practical risk):** the packer reads, writes
  and switches in the background on groups where Raft proposals already
  lead the lock waits. The budget and back-off must keep the write path
  within its gate; P4 measures this first.
- **Packing rate against create rate:** if the packer cannot keep up with a
  sustained tiny-file workload, staged files accumulate at today's cost.
  The staged cap raises priority, but the rate is a hypothesis to measure.
- **Superseded-generation reclaim:** retiring per-file fragments of live
  inodes at millions-of-files scale is a new load on W124's journal and on
  directory operations.
- **Temporary space:** small files cost today's 69,632 bytes per node until
  they are packed, for at least the minimum age.
- **Port size:** about 2,000–3,000 lines of archived packing code (the
  client writer and cohort are no longer ported) plus the new packer,
  candidate index and retirement, crossing server, metadata and the wire.
- **Existing data:** the backfill rewrites millions of small files on
  orcd-mini. It must be rate-limited and resumable.
- **Read amplification:** a packed read fetches a segment fragment of up
  to 64 KiB for a file of a few hundred bytes. Measured in P7.

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

### Design change: background packing (2026-10-18)

| Change | Reason |
| --- | --- |
| Packing at close (P0 option (a), singletons, fused admission and publication) replaced by a background packer after close. | The early P1 measurement failed the GO condition (close 5.4× slower, throughput −71 to −78%, 9.9 Raft entries per file). The user chose background packing. |
| The "no unpacked small-file path" rule becomes "one write path (today's), packing always on, no switch". Staged per-file small files are a transient form, not a mode. | Keeps close and fsync unchanged while still packing every small file. The old way is still preserved by a tag, and comparisons still build two releases. |
| The client writer, client cohort and opt-in are not ported; P6 removes nothing. | No packed write path exists, so there is nothing to switch or remove. |
| Superseded per-file generations retire through W124's journal. | Packing supersedes generations of live inodes, which today go through the per-fragment GC-record pass. |
| Existing small files are packed by a backfill instead of a separate migration decision. | They are ordinary packer input. |

## Open questions

- The minimum age, creation epoch and *t* (P0 decisions, measured in P4).
- Whether segments from different directories may share a stripe when a
  directory has too few files to fill one (P4).
- The backfill rate on orcd-mini against its foreground workload (P7).
