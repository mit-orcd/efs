# W125 — Small-file packing as the default

**Class:** capacity / performance. **Recorded:** 2026-10-18.
**Status:** PROPOSED; design revised 2026-10-18 to **background packing
after close** (the user's choice after the
[early P1 measurement](../status/w125-p1-early-20261018.md)).
[Implementation plan](small-file-packing-plan.md) (phases P0–P7).
Design decisions, port, qualification and landing: **NOTRUN**.
Related:
- [W124 fast delete](fast-delete-reclaim.md), a prerequisite;
- [D37](../status/decisions.md#d37--packing-and-container-engine-removed-from-devel-taken-oct-16);
- the archived implementation at tag `archive/packing-containers-20261016`
  (branch `packing-containers`).

## Problem

Every file, however small, is stored as at least one full fragment per node.
- **Fragment size:** on orcd-mini every fragment file is exactly 69,632
  bytes (64 KiB payload plus header and digest), so a 200-byte file uses
  three of them, one per node, with 2+1 coding.
- **Measured 2026-10-18,** in the
  [deletes, GC and crawl profile](../status/orcd-mini-gc-delete-profile-5d673efa8-20261018.md)
  and W124's space accounting:
  - 4.6 M live tiny files of 0.2–0.3 KiB (all under 1 KiB) hold about
    1–1.5 GB of data;
  - they occupy about 298 GiB per node, roughly 800× their size in raw
    disk across the cluster;
  - packed at a near-full density, they would need a few GiB per node.
- **Side effect:** every tiny-file delete leaves one fragment per node for
  reclaim. That is a large part of the 6.1 M-fragment backlog W124
  addresses.

## History

- **What existed:** devel had experimental tiny-file packing at close time:
  - client `pack_write` (`EFS_PACKED_WRITES`);
  - packed segments, readers, cohorts, ledger and mapping;
  - packed publication, upload, fence and GC.

  It ran alongside an experimental container storage engine.
- **Removal:** on 2026-10-16 both left devel at the user's request, to be
  resumed later (D37, commit `5ded11179`). The last devel commit with both
  is `6c274f47f`, at tag `archive/packing-containers-20261016` on branch
  `packing-containers`.
- **Packing at close was measured and rejected (2026-10-18).**
  - [P0](../status/w125-p0-20261018.md) accepted singletons at close plus
    merge compaction, on condition that a singleton's publication costs no
    more than today's close.
  - The [early P1 measurement](../status/w125-p1-early-20261018.md) of the
    archived code failed that condition:
    - close 3.53 ms against 0.65 ms;
    - throughput −71% (one writer) and −78% (16 workers);
    - 9.9 Raft entries per file against 4.0.
  - The cause is structural. Today the data is sent before close
    (asynchronous PUT), so close waits for one publication. A packed
    segment can only be written once it is sealed, so close waits for the
    PUTs, admission and publication in sequence.
- **Decision (user, 2026-10-18):** background packing after close. Keep
  today's write path and pack closed small files afterwards.

## Rule: one write path, packing always on, no A/B in the code

The user's requirements (2026-10-18), as revised for background packing:
- **One write path.** Every file is written exactly as today: per-file
  chunk fragments, asynchronous PUT, unchanged close and fsync. There is no
  second, packed write path in the client. The archived `pack_write`
  writer, its client cohorts and `EFS_PACKED_WRITES` are not ported.
- **Packing is part of storage, not a mode.** For a small file, today's
  per-file fragments become a transient staging form. The background
  packer moves every closed file under the threshold into packed segments
  within a bounded delay, and nothing else stores small files long-term.
- **No switch.** There is no runtime switch, environment variable, build
  flag or opt-out that disables the packer, and no code that keeps
  small files unpacked by choice.
  - Operational tunables (minimum age, rate budget, priority) have no
    value that means "off".
  - Tests may shorten the minimum age, or ask the packer to run a pass at
    once, so they can check packed state deterministically.
- **Both representations are read.** The read path handles packed
  references and per-file chunks. That is inherent in staging, and it also
  covers every existing store, whose small files become packer input.
- **The old way is preserved in Git.** Immediately before the packer
  lands, tag the last devel commit without it
  (`archive/unpacked-small-files-<date>`).
- **Comparisons build two releases.** Every before/after comparison builds
  the tag and the new commit and compares the builds. There are never two
  modes in one binary.
- **Size class, not a mode.** Files above the packed threshold keep
  today's per-file fragments permanently.

## Proposal

**No file data in metadata rows.** Per the user (2026-10-18), every small
file's bytes are packed into ordinary fragments. Metadata holds only the
file's location in a segment (segment id, generation, offset, length,
digest), never its payload.

**Life of a small file:**

| Stage | Storage | Who |
| --- | --- | --- |
| written, open, or closed less than the minimum age ago | today's per-file chunk fragments | client, unchanged write path |
| closed, below the threshold, quiet for the minimum age | candidate, queued for packing | metadata publication records it |
| packed | a record in a shared packed segment; the per-file fragments are retired | background packer |
| modified (write, append, truncate) | a new generation through today's write path; the packed record becomes dead space | client, unchanged write path |
| deleted | metadata only; the record becomes dead space, and an empty segment is retired whole | POSIX delete, then W124 reclaim |

A file deleted before it is packed is never packed, so create-then-delete
workloads pay no packing cost.

**Placement by size:**

| Size | Long-term storage |
| --- | --- |
| up to the packing threshold (initially 16 KiB, accepted in P0; it raises the archived validator's 4 KiB record limit as a versioned format change) | packed: many files' payloads in shared packed segments, each segment one erasure-coded stripe of standard fragments |
| larger | today's per-file chunk fragments |

A packed segment is one stripe: k data fragments plus f parity fragments,
each a standard fragment file. Segments are variable size (up to 128 KiB,
8 KiB aligned), so packed and unpacked data use the same fragment format,
placement, integrity and reclaim path.

**Segment ownership.** A packed segment does not live under any inode's
directory. It has its own owner: a segment directory per publication group
on each node, keyed by segment id. Inode death (W124 REAP) therefore never
reaches a segment's fragments. A segment's space is freed only by
retirement once its live count is zero.

## The background packer

One copy-forward engine does both the initial packing and the compaction
of holes. Its inputs are either per-file staged small files or low-live
packed segments, and its output is always a new packed segment.

1. **Candidates.**
   - When a publication leaves a file closed and below the threshold, the
     same Raft entry records it in a per-group candidate index keyed by
     (directory subtree, creation epoch). No extra Raft entry per file is
     needed.
   - The entry is removed when the file is packed, deleted or grows past
     the threshold.
   - Existing stores are backfilled into the index by a one-time,
     rate-limited scan.
   - A candidate is eligible once it is closed (no write lease), its last
     data publication is at least the minimum age old, and it is not open
     for reading by a lease that pins the chunk.
2. **Lifetime grouping.** The packer builds each segment from candidates
   that will probably die together:
   - one publication group per segment, always (the ledger and references
     assume it);
   - then the same directory subtree and creation epoch, filling up to the
     segment's record and byte caps.

   Unlike packing at close, the packer sees whole closed directories, so a
   sequential writer's folder fills segments instead of producing
   singletons. Real tiny-file workloads fill a directory and later delete
   the whole tree (`tiny_files.py`, then `edelete`). Such segments empty
   completely and are retired whole with no copying.
3. **Copy.** The packer reads each candidate's bytes through the normal
   verified read path, builds the segment image (per-record digests), and
   writes it through the packed PUT. The segment's records are admitted as
   pending. The copy is durable on all k+f nodes before any switch.
4. **Switch.** One Raft entry per segment switches the members' mappings,
   with a compare-and-swap on each member's exact expected source:
   - for a staged file: inode, chunk index, generation and fragment-set
     identity, plus the content digest;
   - for a compaction source: source segment id and generation, offset,
     length and digest. A generation alone is not enough, because
     relocation keeps the content generation.

   In the same atomic batch, the destination ledger marks each switched
   record live and the source releases it. A member whose CAS fails, because
   it was written, truncated, deleted or moved meanwhile, is cancelled in
   the destination ledger and counted as dead space there. The rest of the
   segment switches normally.
5. **Retire the sources.**
   - **Staged per-file fragments** are superseded generations of a live
     inode. They are retired through W124's batched, journal-first node
     reclaim, extended to superseded generations: the node deletes exactly
     that generation's fragment files, and then the inode's chunk and inode
     directories once they are empty.
   - **Emptied segments** are retired whole through the same journal.
   - **Readers:** a source stays until no reader or open lease can still
     reach it. The per-record and per-fragment digests reject any stale
     read.
6. **Budget and placement.**
   - The packer runs on each publication group's leader, at most one per
     group. Its state is soft: after a leader change it restarts from the
     candidate index, and a half-written segment's pending records are
     cancelled through admission (I15).
   - It runs on a rate-limited budget below foreground I/O and backs off
     when foreground latency rises. Its priority rises as the caps below
     near.

**Metadata cost:** the write path keeps today's ~4.0 Raft entries per file.
Packing adds a candidate-index write inside the existing publication batch,
one switch entry per segment (up to 512 files), and batched retirement:
amortised, a small fraction of an entry per file.

## Fragmentation: never pin mostly-dead fragments

The failure to design out: 16 small files share a fragment, 15 are deleted,
and the 16th lives forever, pinning a fragment that is 15/16 dead. With
sub-KiB files, one 64 KiB fragment can hold hundreds of files, so an
unmanaged scheme degrades towards today's waste. Three rules, required from
the start:

1. **Pack files that die together:** the packer's lifetime grouping above.
   Compaction output keeps the publication-group boundary and is written to
   separate cold segments, never mixed with freshly packed files.
2. **Track live bytes per segment.** Each segment's ledger has a live-byte
   and live-file counter, updated in the same Raft entry as the delete,
   overwrite, truncate or switch that changes it. An overwritten packed
   file is written as a new per-file generation, and its old record becomes
   dead space, counted the same way.
3. **Compact by copying survivors forward, with a hard bound.**
   - **Trigger:** a segment becomes a compaction candidate when its live
     share falls below a threshold *t* (for example 50%) or its dead bytes
     exceed a size limit.
   - **Order:** candidates are processed by most dead space per byte
     copied, the log-structured-filesystem cost/benefit order.
   - **Mechanism:** the packer's copy, switch and retire steps, with a
     segment as the source.
   - **The 15-of-16 case:** one survivor (1/16 of the segment) is copied,
     and the 15/16 is freed.

**Bounds this gives, and what they exclude:**
- **Packed segments that are not candidates** are each at least *t* live,
  so their overhead is at most 1/*t* (2× at *t* = 50%) on top of erasure
  coding.
- **Excluded from that bound:**
  - segments queued for compaction;
  - sources retained after a switch until readers and leases retire;
  - staged small files not yet packed, which cost today's 69,632 bytes per
    node each.
- **Caps:** per node, the compaction backlog (dead bytes awaiting
  compaction), the retained-source bytes and the staged-unpacked bytes are
  capped. As a cap nears, the packer's budget and priority rise:
  compaction first when dead space dominates, packing first when staged
  bytes dominate.
- **No foreground throttling by the packer.** Writes are never delayed for
  packing. If physical space runs short, writes get W124 part 2's ordinary
  ENOSPC at the physical reserve, with its reason. The packer's copies use
  W124's compaction headroom, and every copy is repaid by a retirement many
  times larger: a 128 KiB segment replaces up to 512 per-file fragments.
  With concurrent jobs capped and sources pinned by open leases excluded,
  a full disk can still pack and compact. The journal and checkpoint
  minimum is not available to the packer.

  Total small-file space is then at most 1/*t* × live packed bytes, plus
  the caps.
- **Copy cost:** compaction rewrites at most 1/(1−*t*) times the deleted
  bytes. Initial packing copies each small file once (about 256 B for the
  measured workload), against 69,632 bytes per node freed.
- **Today:** overhead for sub-KiB files is about 800×.

**No hole reuse inside a segment.** Segments are erasure-coded across nodes.
Filling a hole in place would be a partial-stripe update, with read-modify-
write parity, consistency and crash hazards. Space is reused only at whole-
segment or whole-fragment granularity, after the segment has been emptied.

## Required design decisions (each recorded before code)

1. **Thresholds and limits.**
   - Packing threshold: 16 KiB initially (P0). 32 and 64 KiB stay
     candidates after measuring read amplification and metadata cost.
   - Compaction threshold *t*, the dead-bytes limit, and the per-node caps
     on compaction backlog, retained sources and staged bytes, from
     measured space, copy cost, read latency and the physical reserve.
2. **Packer policy.**
   - Minimum age before packing (a hypothesis: 60 s), and the creation
     epoch length for grouping.
   - Exact grouping key (publication group, directory subtree, epoch) and
     how a large directory is split across segments.
   - Placement on the group leader, the budget, the back-off signal and the
     concurrency cap.
3. **Candidate index.** Its KV kind and key, its update inside the
   publication batch, and the backfill scan for existing stores.
4. **Switch.** The exact source identity for staged files and segments,
   the batch size per entry, and cancellation of failed members.
5. **Format compatibility.**
   - Versioned on-disk formats for the segment (including the raised record
     limit), the extended ledger and the candidate index.
   - Builds that do not support a format refuse it, fail closed.
   - Coordinated upgrades; downgrades across a format change are refused.
   - Clients are not build-gated today: only daemons send HELLO. Clients
     must read packed references, so compatibility is enforced by the
     server on every client connection, pooled or reconnected: a version
     declaration is the first message, and no data read or mutation is
     served before it. Old against new is tested in both directions.
6. **Modifying packed files.** Any write, append, truncate or hole punch on
   a packed file produces a new per-file generation through today's write
   path (a read-modify-write of the packed bytes where needed). Its record
   becomes dead space, and the packer repacks the new generation if it is
   still below the threshold. Shared segments are never mutated in place.
7. **Segment accounting and reclaim.**
   - The live-byte counter's key and update path.
   - Retirement of superseded per-file generations and whole segments
     through W124's batched node journal, including removal of emptied
     inode directories.
   - W124 part 2: logical quota counts a file's bytes once, whichever form
     it is in; a switch changes no logical quota. Physical space is
     reserved for the segment before the copy and released by retirement.
8. **Durability and fsync.** Unchanged for writers, since the write path is
   unchanged. A packer copy is durable on all k+f nodes before its switch,
   and the switch keeps the file's durability guarantee. Interaction with
   W123 (durable versions) must be stated.
9. **Recovery and integrity.**
   - Segment checksums and each file's identity within its segment, so a
     packed read verifies exactly its bytes (I25).
   - Orphan-segment safety (I15): a copy that never switched is cancelled
     and reclaimed.
   - Generation coherence (I13).
10. **Which archived parts to keep.** Port the segment format, codec,
    ledger, mapping planner, admission, packed reader, packed storage-node
    paths and meta-apply handling. Do not port the client packed writer,
    client cohorts, the opt-in or the container storage engine. Reconcile
    with devel changes since 2026-10-16 (`h->mu` steps, GC and query
    changes, cooldown fixes, KV changes, W124).

## Order of work

1. W124 part 1 (batched reclaim) and part 2 (quota credit at delete
   commit), as prerequisites (done).
2. Revise and review the decisions above, recorded here.
3. Port the packing pieces from `packing-containers` onto the work branch
   (`w125-packing`), without the client writer or any switch. Add the
   candidate index, the packer, live-byte accounting, compaction and
   superseded-generation retirement, and restore the tests.
4. Measure the packer early, on a private node9901 cluster, as soon as it
   packs end to end: foreground `tiny_files.py` throughput with the packer
   running, against a build of the base commit, and the packing rate.
5. Rebase onto the then-current devel. Qualify on a private node9901
   cluster against a build of exactly that devel commit, so concurrent
   fixes are on both sides. Then qualify on orcd-mini with the user's
   go-ahead, including the backfill of its existing small files.
6. Once the gates pass:
   - tag that same pre-merge devel commit, the final A/B baseline, as
     `archive/unpacked-small-files-<date>`;
   - land the packer on devel, always on.
7. Re-assess W124 part 4 (fragment slot reuse). With small files packed,
   it applies at whole-fragment granularity to retired segments and large
   files.

## Acceptance gates

All gates are NOTRUN.
1. **Write path unchanged:** create, write, close and fsync p50/p99 and
   throughput for the user's tiny-file recipe, new build against the
   tagged build:
   - with the packer idle: within run-to-run noise;
   - with the packer running at its budget: within an agreed limit (target
     a loss of at most 10%).
2. **Space:** for 1 M files of 256 B, 4 KiB and 16 KiB, after packing
   settles:
   - raw bytes per logical byte, new build against the tagged build;
   - target within a small constant of the 2+1 coding overhead, not 800×;
   - per-file inode directories removed for packed files.
3. **Packing rate and delay:** the packer keeps up with the recipe's
   sustained create rate, and staged bytes stay within their cap. Report
   files packed per second and the age at which files are packed.
4. **Fragmentation:**
   - **Random deletion:** create 1 M files, let them pack, then delete a
     random 15 of every 16. Packed overhead settles within the 1/*t* bound,
     and compaction moves only the survivors. The backlog and retained
     sources stay within their caps, including at the maximum delete rate.
   - **Whole trees:** delete whole packed trees. Segments retire with no
     copying.
   - **Churn:** 24 h of mixed create, overwrite and delete churn. Overhead
     stays within the bound throughout.
   - **Space pressure:** churn on a nearly full filesystem. The packer
     still progresses from the compaction headroom, the physical reserve is
     never crossed, and writes see only the ordinary reserve ENOSPC.
5. **Reads:** read p50/p99 and read amplification for packed files against
   per-file reads on the tagged build.
6. **Reclaim:** delete 1 M packed files. Quota freed at delete commit
   (W124 part 2), and segment retirement completes in minutes.
7. **Correctness:**
   - POSIX core, two-client, durability (`smoke-persist`), private GC,
     POSIX and expiry suites;
   - crash tests during the copy, before and after the switch, and during
     retirement;
   - concurrent write, truncate, delete and read against the packer;
   - two packers or compactors racing on the same file: exactly one switch
     wins;
   - modification of packed files (write, append, truncate, hole punch).

   All must PASS.
8. **Landing:**
   - the tag exists;
   - behavioural tests through persisted mappings and a physical fragment
     inventory:
     - sub-threshold files are packed after the minimum age, and their
       per-file fragments and inode directories are retired;
     - large files keep chunks;
     - a modified packed file is rewritten per file and repacked;
     - legacy per-file small files are backfilled and packed;
     - empty files create no segment;
     - no switch disables the packer;
   - a recorded evidence page;
   - D37's register entry updated.

## Non-goals

- A runtime or build-time choice between packed and unpacked storage.
- A packed write path in the client, or a change to the close or fsync
  contract.
- File data stored in metadata (KV) rows.
- In-place reuse of holes inside segments.
- Reintroducing the container storage engine.
