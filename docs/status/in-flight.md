# In flight — D25 writer integration

[Work queue + status](README.md) · [Decisions](decisions.md) ·
[Architecture](../how-it-works/architecture.md)

## Oct 6 2026 — correctness before performance

The Oct 2 handoff is preserved in [project history](../archive/project-history.md).
Its cluster-state claims are historical; establish current state before any
live operation. Current acceptance targets the four-node NUC; live results are recorded per rollout.

Committed foundations: `bb18e40a` metadata/read views, `033a842a` FUSE memory
and D27 recovery/stop, and `3d9bb6b2` POSIX acceptance gates. The current
partial-writer change (`8727f682`) validates published merge bases under fresh lane
authority in both flush paths; it does not activate logical truncation.

The user accepted [lane-local authority first](d25-admission-routing.md).
Durable lane authority/bootstrap primitives and the lane-only read RPC are now
staged; the older inode writer RPC remains for cold discovery and tests.
Cold bootstrap RPC and missing-lane-only client admission fallback are implemented. Geometry-checked cache admission and immutable snapshots are staged; connect FUSE callers and typed publication next. The latest token checks are recorded in the
[checkpoint](../../results/measure/20261006-writer-token-checkpoint/SUMMARY.md).

Next, in order:

1. Writer authority, FileID-tagged GETCHUNKS, typed base/publication planning,
   budgeted sidecar lifetime, admission-before-copy and immutable body snapshots
   are implemented and tested. Optional state now participates in cache drop,
   replacement/reclaim and REPORT acknowledgement. The active legacy cache also
   preserves pending bytes across late-loader merges and holds the slot lock
   through installation. [Current checkpoint](../../results/measure/20261006-cache-five-rounds/SUMMARY.md).
   Full-overwrite ownership/reset is now atomic, its cache-hit fast path avoids
   extra allocation, and pending publication blocks another snapshot. Clean
   cache binding to expected FileID/authority is staged and tested; legacy dirty
   bytes cannot acquire a new epoch. Write extent guards reject index/end wrap.
   [Latest checkpoint](../../results/measure/20261006-admission-five-rounds/SUMMARY.md).
   The [NUC ten-round checkpoint](../../results/measure/20261006-nuc-ten-rounds/SUMMARY.md)
   adds durable bootstrap crash recovery, snapshot overlap protection, exact
   same-mutation PUT ownership checks and lane-validated cache APIs. Legacy flush
   refuses typed ownership before I/O. These APIs do not activate D25.
   The [publication ten-round checkpoint](../../results/measure/20261006-publication-ten-rounds/SUMMARY.md)
   binds captured FileID and exact CAS bases, reserves identity before PUT, and
   supplies typed cache completion APIs. They remain staged. Before activation,
   [durable publication submission/status and cache result handling](d25-admission-routing.md#durable-publication-results--implemented-staged)
   now distinguish exact commit from terminal rejection and unknown. Legacy
   aggregate STALE still cannot authorize dropping or rebasing ownership.
   Acknowledged retirement now bounds each stream to 64 live receipts and
   advances a durable replay floor atomically. RETIRED never releases ownership.
   Ordered ACK retry ownership is now staged and NUC-tested (immutable,
   bounded, oldest-consumed-first, metadata-budgeted). Before activation,
   integrate it with both flush paths and add I23 session
   fencing/admission before cleanup of abandoned streams/floors; finish coherent
   lane-local mtime invalidation.
   Next connect every FUSE write entry point and both flush paths to these APIs;
   replace the unlabelled union, serialize pending publications and drain range
   exhaustion before copying bytes. No epoch-aware FUSE admission is active.
   Test unpublished overlapping writes across shrink/extend and history
   retirement; never re-age retained legacy writes or derive authority from a
   cache hit.
2. Carry captured epochs for spans and unconditional full overwrites, with
   fence races returning STALE and rebuilding from a new authoritative view.
3. Implement bounded live-file materialization scheduling, durable progress
   and safe lane/inode history retirement; keep unlink sweep separate.
4. Enable the logical resize coordinator only after those prerequisites pass
   cold-read, concurrency, restart, history-full and bounded-apply gates.
5. Complete D27 strict whole-call timing and remaining legacy PUT coverage;
   run the recorded fault/recovery and small-host RSS acceptance gates.

NUC rollout 79983128 passed W36 `peer_rename_vs_unlink_src` 20/20 first.
Xorinox b4a75492 passed the real two-client W36 gate 20/20 after guarded orphan cleanup. A later rollout must repeat that gate. W54 post-GC cold reads and W38 traced/cold verification remain owed.
NUC unit/build acceptance used a private source directory. Live acceptance
used both normal mounts after clean drains and a four-node rollout. Public logical truncate remains disabled.
