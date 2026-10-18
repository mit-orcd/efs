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
    disk across the cluster.
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

**Placement by size class, decided when the file's data is published:**

| Size | Storage | Reason |
| --- | --- | --- |
| up to an inline threshold (about 1–2 KiB, to be measured) | inline in the inode's metadata (KV) row | no fragment at all; read with the GETATTR row; delete is metadata-only |
| inline threshold to about 64 KiB | packed segments: many files' payloads in shared, erasure-coded segments | amortises the per-fragment header and file cost; segment-level placement and integrity |
| larger | today's per-file chunk fragments | already efficient |

**Required design decisions** (each recorded before code):
1. **Thresholds.** Choose the inline and packed thresholds from measured
   KV row-size cost, read latency and replication cost. They must stay
   within the KV value and Raft entry limits.
2. **Growth across a class.**
   - Append and overwrite: a file that grows past a threshold moves to the
     next class as a new immutable generation, as re-striping does.
   - Truncate and sparse files follow the same rule.
   - No in-place mutation of shared packed segments.
3. **Reclaim of packed segments.**
   - A delete leaves a hole in a shared segment.
   - Reclaim needs per-segment live-byte accounting and compaction, which
     rewrites live payloads into new segments and retires the old one.
   - It must use W124's batched, node-local reclaim path, not a new
     per-file protocol.
   - W124 part 2 (quota credit at delete commit) applies unchanged.
4. **Durability and fsync.**
   - fsync of a packed or inline file must give the same guarantee as
     today's per-file durable ACKs, including grouped barriers.
   - Interaction with W123 (durable versions) must be stated.
5. **Recovery and integrity.**
   - Segment checksums and the per-file identity within a segment, so a
     packed read verifies exactly its bytes (invariant I25).
   - Orphan-segment safety (I15).
   - Generation coherence (I13).
6. **Inline data and the namespace.**
   - Inline payloads raise KV and Raft log volume.
   - Measure the impact on the metadata path, which already carries the
     lease commits found on 2026-10-18.
7. **Which archived parts to keep.**
   - Port the packing code only. The container storage engine stays out
     unless separately decided.
   - Reconcile with everything that changed after 2026-10-16: the `h->mu`
     steps, the GC and query changes, the cooldown fixes and the KV
     changes.

## Order of work

1. W124 part 1 (batched reclaim) and part 2 (quota credit at delete
   commit), as prerequisites.
2. Design review of the decisions above, recorded here.
3. Port packing from `packing-containers` onto a work branch from devel,
   without the switch. Restore its tests and add the inline class.
4. Qualify the branch on a private node9901 cluster, comparing its build
   against a build of the devel commit it branched from (the commit step 5
   tags). Then qualify it on orcd-mini
   with the user's go-ahead.
5. Once the gates pass:
   - tag the current devel as `archive/unpacked-small-files-<date>`;
   - land packing on devel with the old small-file path removed, so there
     is no switch.
6. Re-assess W124 part 4 (fragment slot reuse). With small files packed or
   inline, it applies mainly to large files.

## Acceptance gates

All gates are NOTRUN.
1. **Space:** for 1 M files of 256 B, 4 KiB and 32 KiB:
   - raw bytes per logical byte, new build versus the tagged build;
   - target within a small constant of the 2+1 coding overhead, not
     800×.
2. **Performance:** create, write, read, stat and delete p50/p99 and
   throughput for the same sets, comparing the new build against the
   `archive/unpacked-small-files-<date>` build, under the user's tiny-file
   workload recipe. No regression beyond agreed limits for files
   above the packed threshold.
3. **Reclaim:** delete 1 M packed files. Quota freed at delete commit
   (W124 part 2), segment compaction completes in minutes, and there is no
   foreground regression during compaction.
4. **Correctness:** POSIX core, two-client, durability (`smoke-persist`),
   private GC, POSIX and expiry suites, crash tests during packing and
   compaction, and the class transitions (grow, truncate, overwrite) all
   PASS.
5. **Landing:**
   - the tag exists;
   - devel contains no unpacked small-file path and no packing switch
     (checked by a source test);
   - a recorded evidence page;
   - D37's register entry updated.

## Non-goals

- A runtime or build-time choice between packed and unpacked storage.

- Reintroducing the container storage engine.
- Changing POSIX semantics or the fsync contract.
