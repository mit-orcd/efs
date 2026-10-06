# In flight — D25 writer integration

[Work queue + status](README.md) · [Decisions](decisions.md) ·
[Architecture](../how-it-works/architecture.md)

## Oct 6 2026 — correctness before performance

The Oct 2 handoff is preserved in [project history](../archive/project-history.md).
Its cluster-state claims are historical; establish current state before any
live operation. This implementation phase performs isolated tests only.

Committed foundations: `bb18e40a` metadata/read views, `033a842a` FUSE memory
and D27 recovery/stop, and `3d9bb6b2` POSIX acceptance gates. The current
partial-writer change (`8727f682`) validates published merge bases under fresh lane
authority in both flush paths; it does not activate logical truncation.

Next, in order:

1. The bounded dirty-range primitive is implemented and byte-model tested in
   `include/efs/dirty_ranges.h` (`6221d22f`); it is not connected to FUSE. Add authoritative
   writer snapshots (FileID/epoch/complete history/retirement floor), including
   absent chunks and new lanes, before assigning epochs at write admission.
   Replace dcache's unlabelled range union with the primitive, reserve its
   metadata cost, and preserve matching byte/range snapshots across retries.
   Test unpublished overlapping writes across repeated shrink/extend and
   history retirement; do not use cached epochs as write authority.
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
No live deployment, repair or workload replay was performed in this phase.
