# W123 — Automatic crash recovery through per-file durable versions

**Class:** new feature / durability recovery. **Recorded:** 2026-10-16.
**Status:** PROPOSED; design/ordering review required before implementation.
Implementation, migration, performance and hardware acceptance: **NOTRUN**.
Related: [W71 durability](work-items.md#w71) and the
[existing persistent crash-fence blocker](../status/amd-async-durability-20261015.md#review-follow-up-clean-restart-versus-uncertain-recovery).
This feature does not close W71 or authorize a deployment.

## Goal and constraints

After an ordinary crash, automatically recover the committed state without
formatting or rebuilding the node. Preserve successful fsync guarantees,
asynchronous writes, immediate peer visibility and grouped data barriers.
Retain durable local storage. No payload redo journal is proposed initially.
Do not clear the current uncertainty fence or merely narrow EIO by daemon ID:
a fresh client has no old verifier with which to detect lost published bytes.

No cross-file application dependency graph: reading A and writing B does not
make fsync(B) promise durability of A. Storage-internal dependencies, namespace
operations and ordering within a file still require explicit treatment.

## Required ordering

1. Maintain separate visible and durable versions for each file. A recoverable
   version includes size, fragment generations, base/span mappings and the
   namespace information required by the specified fsync contract. Persist
   enough version/dependency information to reconstruct it. A sequence number
   or a pair of flags alone is insufficient; retaining all historical versions
   is not required once dependency-safe reclamation is proven.
2. Write new immutable fragments and publish the visible version immediately.
   Capture a particular version for fsync. Complete data barriers covering its
   required dependencies and destinations, then persist its durable-version
   metadata before returning fsync success. Retain conservative all-node COMMIT
   participation initially; changing availability/quorum policy is separate.
3. Concurrent later writes cannot advance a captured fsync result incorrectly.
   Partial overwrites and folds must include their base/span dependencies;
   an independent complete replacement need not preserve irrelevant history.
   Metadata persistence failure after the data barrier must not return success.
4. Keep the previous durable version intact. Audit retries, generation-zero
   objects, truncation, namespace deletion and span folds for destructive
   replacement. Generation-bearing filenames alone do not prove immutability.
   GC may delete data only when no recoverable version or active operation
   references it, including durable recovery dependencies on other nodes.
5. Restart from durable-version metadata and validate required references.
   Discard/quarantine unfinished versions, recover healthy files and reopen
   service without rebuilding the node. Missing/corrupt data belonging to a
   committed version remains an explicit error: never silently roll back a
   successful fsync or declare genuinely lost committed data recovered.
6. Specify create/rename/unlink, file versus directory fsync, retained readers,
   leader changes, replay, duplicate requests and completion-loss behavior.
   Define how fresh clients discover uncertain file versions. No elapsed-time
   assumption or client verifier alone can authorize durability or reclamation.

## First deliverable: design and ordering review

Before code, map actual PUT acknowledgement, application write completion,
visible publication, data barrier, durable metadata commit and GC eligibility.
Produce the per-file state machine and crash table, identifying existing
records that can be reused and any added durable metadata operations. Review
retry writes, generation-zero compatibility and folds explicitly.

Specify migration for old clean stores and already-fenced uncertain stores.
New recovery metadata cannot recreate bytes already lost under the old format;
no operator-clear shortcut is implied. Persist recovery completion before
removing a fence and specify repeatable restart during recovery itself.

## Acceptance gates

- Inject crashes before/after every ordering transition and lost replies;
  exercise fresh/peer clients, concurrent writes/fsync, retries, truncation,
  folds, GC and repeated recovery. Every successful fsync must survive with
  exact bytes and promised metadata. Un-fsynced data may be lost only within
  the defined contract, without damaging committed versions.
- Check isolated error handling, missing committed fragments, storage errors,
  namespace recovery and cancellation. No node-wide permanent refusal for an
  ordinary recoverable crash; no false durability success for uncertain data.
- Native/sanitizer tests and small private Intel TCP/direct integration first;
  `.efs` and expiry disabled. Packing/containers are removed from devel, not
  prerequisites. RDMA testing uses AMD software RDMA and ORCD hardware RDMA.
  SIGKILL tests are not hardware power-loss qualification; test that separately.
- Compare matched baseline/candidate throughput, visibility latency and fsync
  latency separately, including tail latency, metadata write amplification,
  disk use from retained versions and recovery time. Agree workload/noise bounds
  before measurements. No-slowdown is an acceptance requirement, not an assumed
  zero-cost property. Resolve regressions without weakening durability.

This is a design record only. Reference devel at recording: `c1c1bc570`.
No binaries built, tests of implementation run, or cluster changes made.

## Review feedback (2026-10-16)

Agree with the direction, ordering list and gates. One design input for the
ordering review, aimed at the no-slowdown gate:

- **Derive fragment durability from the existing barrier epochs instead of a
  new per-fsync metadata write.** Each root already persists its barrier
  progress: `put_sync_barrier_locked` runs `syncfs` and then writes
  `(boot, durable seq, verifier)` to `.efs-durable-epoch` (`src/server/store.c`).
  If every PUT acknowledgement also carried its root's `(boot, seq)` and the
  published version recorded that tag per fragment, recovery could classify
  each fragment as durable (`seq <= durable seq` recorded for that boot) or
  uncertain. A version is then durable when all its fragments are. Ordering
  item 2's "persist durable-version metadata before fsync success" may reduce
  to today's data barrier, provided the published metadata itself is already
  Raft-durable at publication. Verify that first.
- Cost would be about 8 bytes more per PUT reply and per fragment reference
  (a wire change; `EFS_PUT_REPLY_LEN` is 10 today), with no additional disk
  I/O. Recovery reads only metadata plus the per-root records.
- This also closes the fresh-client hole without client verifiers: uncertain
  fragments are identified from the stored tags, not from whichever client
  happens to remember an old verifier.
- To check: a PUT must be assigned to an epoch only after its write is
  complete, so that a barrier never certifies a write still in flight. Also
  check multi-root nodes and a barrier failure (`r->error`) inside a boot.
- This still covers only fragments with recorded tags. Old-format and
  already-fenced stores still need the migration rule in this plan.
