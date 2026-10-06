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

On the next authorized rollout, W36 `peer_rename_vs_unlink_src` 20/20 goes
first. W54 post-GC cold reads and W38 traced/cold verification remain owed.
Isolated NUC acceptance uses private clients; original mounts and retained
write evidence are unchanged. Public logical truncate remains disabled.
