# W125 — Small-file packing as the default

**Class:** capacity / performance. **Recorded:** 2026-10-18.
**Status:** PROPOSED; to start after W124 parts 1–2. The user wants packing to
become the only small-file storage. The previous way is preserved by a tag,
not by a switch in the code.
Design (size classes, reclaim), port, qualification and landing:
**NOTRUN**.
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

- **What existed:** devel had experimental tiny-file packing:
  - client `pack_write` (`EFS_PACKED_WRITES`);
  - packed segments, readers, cohorts, ledger and mapping;
  - packed publication, upload, fence and GC.

  It ran alongside an experimental container storage engine.
- **Removal:** on 2026-10-16 both left devel at the user's request, to be
  resumed later (D37, commit `5ded11179`). The last devel commit with both
  is `6c274f47f`, at tag `archive/packing-containers-20261016` on branch
  `packing-containers`.
- **Resuming:** the removal commit says to branch from devel and revert it.
- **Default:** the user now wants packing on by default.

## Rule: one storage path, no A/B in the code

The user's requirement (2026-10-18): packing replaces the current small-file
storage. It is not an option beside it.
- **No switch:** no runtime switch, environment variable, build flag or
  opt-out, and no code that keeps both placements.
  - The archived `EFS_PACKED_WRITES` switch and its unpacked fallback for
    small files are not ported.
- **Preserve the old way in Git:** immediately before packing lands, tag
  the last devel commit that stores small files as per-file fragments
  (`archive/unpacked-small-files-<date>`).
- **Replace in the next change:** packing lands and the old small-file
  path is removed in the same change (or the immediately following
  commit).
- **Comparisons:** every before/after comparison builds the tag and the new
  commit as two releases and compares the builds. Files above the packed
  threshold keep today's per-file fragments; that is the size-class rule,
  not an alternative mode.

## Proposal

**No file data in metadata rows.** Per the user (2026-10-18), every small
file's bytes are packed into ordinary fragments. Metadata holds only the
file's location in a segment (segment id, offset, length, digest), never its
payload.

**Placement by size, decided when the file's data is published:**

| Size | Storage |
| --- | --- |
| up to the packing threshold (about 64 KiB, to be measured) | packed: many files' payloads in shared packed segments, each segment one erasure-coded stripe of standard fragments |
| larger | today's per-file chunk fragments |

A packed segment is one stripe: k data fragments plus f parity fragments,
each the standard fixed-size fragment file (69,632 bytes on orcd-mini today).
Packed and unpacked data therefore use the same fragment files, placement,
integrity format and reclaim path.

## Fragmentation: never pin mostly-dead fragments

The failure to design out: 16 small files share a fragment, 15 are deleted,
and the 16th lives forever, pinning a fragment that is 15/16 dead. With
sub-KiB files, one 64 KiB fragment can hold hundreds of files, so an
unmanaged scheme degrades towards today's waste. Three rules, required from
the start:

1. **Pack files that die together (lifetime grouping).**
   - An open packing segment accepts files only from the same directory,
     the same writer session and a short creation window (milliseconds to
     seconds, bounded by fsync).
   - Real tiny-file workloads fill a directory and later delete the whole
     tree (`tiny_files.py`, then `edelete`), so files packed together are
     usually deleted together. The segment then empties completely and is
     reclaimed whole with no copying.
   - Mixed-lifetime survivors are the exception, not the norm.
2. **Track live bytes per segment.**
   - Each segment has a live-byte and live-file counter in metadata,
     updated in the same Raft entry as the delete or overwrite that
     changes it.
   - An overwritten packed file is written as a new copy, and its old
     bytes become dead space, counted the same way.
3. **Compact by copying survivors forward, with a hard bound.**
   - **Trigger:** a segment becomes a compaction candidate when its live
     share falls below a threshold *t* (for example 50%) or its dead bytes
     exceed a size limit.
   - **Order:** candidates are processed by most dead space per byte
     copied, the log-structured-filesystem cost/benefit order.
   - **Copy:** the survivors' payloads are copied into a separate cold
     packing segment, so survivors are not mixed back with fresh data. The
     new copy is durable before any switch.
   - **Switch:** each survivor's location is switched with a Raft
     compare-and-swap on its generation, so a concurrent overwrite or
     delete wins and the copy is discarded.
   - **Retire:** when the old segment's live count reaches zero, the whole
     stripe goes to W124's batched reclaim, to be deleted or reused as a
     whole-fragment slot.
   - **Readers:** the old segment is kept until no reader or open lease
     can still reach the old location, and the per-file digest rejects any
     stale read.
   - **Budget:** compaction is rate-limited, below foreground I/O, on the
     W124 batched path.
   - **The 15-of-16 case:** one survivor (1/16 of the segment) is copied,
     and the 15/16 is freed.

**Bounds this gives:**
- every segment that is not a candidate is at least *t* live, so packed
  space overhead is at most 1/*t* (2× at *t* = 50%) on top of erasure
  coding, plus open and in-flight segments;
- compaction rewrites at most 1/(1−*t*) times the deleted bytes;
- today's overhead for sub-KiB files is about 800×.

**Partially filled segments:** an fsync or the creation window can seal a
segment early. An underfilled sealed segment is just a low-live segment.
Compaction merges several of them into one full cold segment, under the same
bound.

**No hole reuse inside a segment.** Segments are erasure-coded across nodes.
Filling a hole in place would be a partial-stripe update, with read-modify-
write parity, consistency and crash hazards. Space is reused only at whole-
segment or whole-fragment granularity, after compaction has emptied it.

## Required design decisions (each recorded before code)

1. **Thresholds.** Choose the packing threshold (about 64 KiB), the
   compaction threshold *t* and the dead-bytes limit from measured space,
   compaction write cost and read latency.
2. **Grouping key and window.** Decide the exact grouping key (directory,
   session, window) and how fsync and close seal an open segment.
3. **Growth across the threshold.**
   - A packed file that grows past the threshold moves to per-file
     fragments as a new immutable generation, as re-striping does; the
     same applies when shrinking below it.
   - Truncate and sparse files follow the same rule.
   - Shared segments are never mutated in place.
4. **Segment accounting and reclaim.**
   - The live-byte counter's key and update path.
   - Compaction scheduling and its foreground budget.
   - Whole-segment retirement through W124's batched, node-local reclaim.
   - W124 part 2 (quota credit at delete commit) applies to packed bytes.
5. **Durability and fsync.**
   - fsync of a packed file gives the same guarantee as today's per-file
     durable ACKs, including grouped barriers.
   - A compaction copy is durable before its location switch.
   - Interaction with W123 (durable versions) must be stated.
6. **Recovery and integrity.**
   - Segment checksums and each file's identity within its segment, so a
     packed read verifies exactly its bytes (I25).
   - Orphan-segment safety (I15), covering a compaction copy that never
     switched.
   - Generation coherence (I13).
7. **Which archived parts to keep.**
   - Port the packing code only, and re-check it against the rules above.
     The container storage engine stays out unless separately decided.
   - Reconcile with everything that changed after 2026-10-16: the `h->mu`
     steps, the GC and query changes, the cooldown fixes and the KV
     changes.

## Order of work

1. W124 part 1 (batched reclaim) and part 2 (quota credit at delete
   commit), as prerequisites.
2. Design review of the decisions above, recorded here.
3. Port packing from `packing-containers` onto a work branch from devel,
   without the switch. Add lifetime grouping, live-byte accounting and
   compaction, and restore its tests.
4. Qualify the branch on a private node9901 cluster, comparing its build
   against a build of the devel commit it branched from (the commit step 5
   tags). Then qualify it on orcd-mini with the user's go-ahead.
5. Once the gates pass:
   - tag the current devel as `archive/unpacked-small-files-<date>`;
   - land packing on devel with the old small-file path removed, so there
     is no switch.
6. Re-assess W124 part 4 (fragment slot reuse). With small files packed,
   it applies at whole-fragment granularity to retired segments and large
   files.

## Acceptance gates

All gates are NOTRUN.
1. **Space:** for 1 M files of 256 B, 4 KiB and 32 KiB:
   - raw bytes per logical byte, new build versus the tagged build;
   - target within a small constant of the 2+1 coding overhead, not
     800×.
2. **Fragmentation:**
   - **Random deletion:** create 1 M files, then delete a random 15 of
     every 16. Packed overhead must settle within the 1/*t* bound, and
     compaction must move only the survivors.
   - **Whole trees:** delete whole trees. Segments must retire with no
     copying.
   - **Churn:** 24 h of mixed create and delete churn. Overhead stays
     within the bound throughout.
3. **Performance:** create, write, read, stat and delete p50/p99 and
   throughput for the same sets, comparing the new build against the
   `archive/unpacked-small-files-<date>` build, under the user's tiny-file
   workload recipe.
   - No regression beyond agreed limits for files above the packing
     threshold.
   - Foreground impact during compaction within agreed limits.
4. **Reclaim:** delete 1 M packed files. Quota freed at delete commit
   (W124 part 2), and retirement and compaction complete in minutes.
5. **Correctness:**
   - POSIX core, two-client, durability (`smoke-persist`), private GC,
     POSIX and expiry suites;
   - crash tests during packing, during compaction (before and after the
     location switch) and during retirement;
   - concurrent overwrite or delete against compaction;
   - the threshold transitions (grow, shrink, truncate, overwrite).

   All must PASS.
6. **Landing:**
   - the tag exists;
   - devel contains no unpacked small-file path and no packing switch
     (checked by a source test);
   - a recorded evidence page;
   - D37's register entry updated.

## Non-goals

- A runtime or build-time choice between packed and unpacked storage.
- File data stored in metadata (KV) rows.
- In-place reuse of holes inside segments.
- Reintroducing the container storage engine.
- Changing POSIX semantics or the fsync contract.
