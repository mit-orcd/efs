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
- Initial cost hypothesis: reuse existing barrier writes and carry compact tags
  in PUT replies and fragment references. **The 8-byte estimate and zero-extra-I/O
  claim are not established; see the source review below.** Recovery may also
  need data validation and retained version/dependency history.
- This also closes the fresh-client hole without client verifiers: uncertain
  fragments are identified from the stored tags, not from whichever client
  happens to remember an old verifier.
- To check: a PUT must be assigned to an epoch only after its write is
  complete, so that a barrier never certifies a write still in flight. Also
  check multi-root nodes and a barrier failure (`r->error`) inside a boot.
- This still covers only fragments with recorded tags. Old-format and
  already-fenced stores still need the migration rule in this plan.


## Source review of barrier-epoch proposal — 2026-10-16

Reviewed `086c8e4a05ba19e4a2e688221c92e9409a87d1e7` on devel.
**Verdict: retain as the preferred optimization hypothesis, with the following
required corrections.** Source review only; implementation/performance tests
NOTRUN. Unrelated working changes to Raft read admission were left untouched.

1. **Persist recovery evidence across boots before reusing the record.**
   `put_sync_attach` reads the previous record, increments `boot`, resets
   `seq=1`/`durable_seq=0`, then writes `(new boot, seq=0, clean=0)` to the same
   record. It retains only the prior wall-clock time for the scan, not a durable
   history of old watermarks. A second crash during recovery would lose the
   previous boot's numerical proof. Define an atomic, repeatable handoff of old
   watermarks or a persistent sequence/incarnation scheme. Keep proof until all
   surviving fragment/version references have been safely covered. Do not infer
   that all writes from an older boot were durable. The current in-place record
   checksum detects a tear but does not provide a previous valid recovery slot;
   define recoverable record update behavior without falsely clearing fences.

2. **Bind tags to exact completed writes and stable root identity.**
   `finish_fragment_put` calls `put_sync_note` after `finish_shard_write` succeeds.
   Under `r->mu`, the barrier captures `target=r->seq`, moves the batch and
   increments the sequence before dropping the mutex for syncfs. A future tag
   must capture the assigned epoch while enrolling that completed write under
   the same lock, before any backpressure wait can release it. Never read the
   current sequence later while forming the reply. Include storage-root identity
   and incarnation, plus a non-ambiguous epoch/sequence; a path index alone must
   not survive root replacement/reordering as false proof. Specify overflow,
   retries/deduplication, normal/ticketed PUTs and barrier failures. Failed data
   or record barriers must not advance the proven watermark; a later successful
   barrier may cover prior work only with a demonstrated ordering argument.

3. **The existing reply does not already contain that tag.**
   `handler.c` emits 10 bytes: status, path index and an eight-byte
   `put_boot_epoch` verifier. The verifier can survive a clean restart while
   per-root boot/sequence changes. Appending only a sequence would therefore
   alias histories unless the identity scheme changes. A `(boot,seq)` tag alone
   also needs its root/incarnation context. Determine the encoding after the
   model is specified; neither an eight-byte increase nor a specific final wire
   size is approved. Carry tags through publication, base/span references,
   snapshots, compaction and replay, not only the wire reply.

4. **Fragment proof does not replace recoverable file-version history.**
   The predicate is valid only for the matching root/incarnation/epoch and the
   immutable object actually enrolled. Recovery must select a complete file
   version, including size, required base/spans and namespace contract. If the
   latest published version is uncertain, its durable predecessor and its data
   must still exist. A latest-only mapping plus watermarks cannot restore a
   discarded predecessor. GC/fold retention remains necessary even when no new
   fsync-time marker is introduced. Persisted proof can close the fresh-client
   hole only with these complete-version and retention rules.

5. **No additional fsync-time metadata operation is plausible, not proven.**
   Raft already distinguishes locally durable log progress (`durable_idx` in
   `raft.c`), and the publication endpoint uses proposal/apply waits and durable
   receipts (`server_raft_host_publication`). This supports exploring reuse of
   existing metadata publication. It is not an end-to-end proof for every
   REPORT/publication, inode-size, namespace, snapshot and recovery path.
   Demonstrate that the exact tagged version and necessary predecessor history
   are recoverably committed before success. The root barrier can then supply
   the data-durability proof without a second per-file metadata commit, provided
   that proof survives restart. Larger publication records and retained versions
   still add memory/storage/replication/compaction costs; avoiding an extra
   barrier does not mean zero extra I/O or unchanged latency.

Required ordering refinement (alternative to an explicit per-file durable marker):

```text
completed immutable PUT -> capture root/incarnation/epoch tag under root lock
-> recoverably publish exact tagged file version and preserve dependencies
-> ensure each required root watermark covers its captured tags
-> fsync success
```

Background barriers may finish before metadata publication; that is safe only
when both proofs exist before fsync success. An explicit durable-version marker
and a durability predicate derived from persisted tags are alternative designs;
choose after proving crash recovery, not from the wire-size estimate.

Additional review gates: crash after reading the old record but before/after
marking a new boot; crash again during recovery; clean restart with verifier
reuse and sequence reset; root replacement/reordering; concurrent PUT enrollment
and barrier capture; barrier/record failure and torn records; deduplicated PUT
retries; metadata leader change/snapshot; latest uncertain version with an older
successful fsync; folds/GC racing both. Record byte-for-byte preservation and
actual wire/storage overhead. No fence removal is authorized by this review.

### Response to the source review (2026-10-16)

Agree with all five corrections; checked against `086c8e4a0`. The original
comment missed point 1: `put_sync_attach` resets `seq=1`/`durable_seq=0` and
overwrites the only record. Point 2 is also confirmed: `put_sync_note` can wait
on `r->cv` after enrolling a write, so the tag must be captured before that wait.

One simplification to evaluate for point 1, instead of keeping a history of old
watermarks:

- **Never reset the sequence.** Make the per-root epoch sequence monotonic for
  the life of a root incarnation (random root ID written at format), across
  clean and unclean restarts. A tag is then `(root ID, seq)`; boot drops out.
- **Bound what a crashed boot could have handed out.** Epochs advance only in
  the barrier, which already writes the record. At a crash, any epoch the old
  boot could have assigned lies within a small fixed distance above the last
  recorded durable value. Today that is the open epoch plus the epoch of the
  barrier in flight. A new boot starts above that bound. Epochs at or below the
  recorded durable value stay proven; epochs between it and the bound stay
  uncertain permanently.
- **Use two record slots.** Alternate checksummed A/B slots and select the
  newest valid one. A torn write then falls back to the previous valid proof
  rather than refusing startup. The durable value only grows, so a second crash
  during recovery loses nothing.

**Hypothesis, not an established property:** one record without history or
additional I/O was proposed. The follow-up review below identifies correctness
requirements that this representation does not yet meet. Proving that bound against the barrier code (`seq++`
before `syncfs`, record write after) is part of the ordering review.
Points 3–5 (wire encoding, complete-version history, GC/fold retention and
end-to-end publication durability) are unaffected and still required.


### Review of monotonic root sequences (`5bc66cf1f`) — 2026-10-16

**Verdict: useful ingredients, but the proposed simplification is unsafe as
specified.** Keep stable root incarnation IDs and investigate alternating
records; do not approve a single monotonically increasing durable watermark
without addressing the following. Source/API review only; no implementation,
runtime changes or crash tests. W123 remains proposed; W71 remains open.

1. **A high watermark cannot represent permanently uncertain holes.** Suppose
   the durable record is 100; the crashed run issued 101 and 102. Restart skips
   to 103, writes new data and persists 103. The predicate `seq <= durable_seq`
   now falsely certifies 101/102, even if their data never reached storage.
   Sequence uniqueness and data durability are different facts. Preserve
   durable interval/incarnation evidence, or complete a crash-safe reconciliation
   that eliminates/replaces every uncertain reference before allowing a
   contiguous-watermark predicate again. That includes snapshots, receipts,
   retained versions, delayed retries and readers, not just current mappings.

2. **The claimed small distance is not bounded by today's failure loop.**
   `put_sync_barrier_locked` increments `r->seq` before I/O. On failure it sets
   `r->error` and requeues the inflight counts without advancing `durable_seq`.
   `put_sync_thread` does not gate its `due` branch on `r->error`; it can retry
   barriers, incrementing sequence numbers repeatedly while the persisted
   watermark stays unchanged. Consequently, an open-plus-inflight bound only
   describes a failure-free interval, not all executions. The proof must cover
   data-barrier failures, record-write failures, retries, admission after errors
   and sequence exhaustion; do not derive a crash bound from the success path.

3. **Skipping an inferred range does not survive repeated crashes by itself.**
   Starting from stored 100, one run skips to 103, acknowledges a PUT tagged 103,
   then dies before persisting anything. The next run still sees 100 and may
   issue 103 again. A newly persisted 103 can then certify the earlier lost
   object. Before issuing tags, either durably reserve a disjoint range / run
   identity or finish a durable recovery handoff that prevents aliasing. The
   current startup record write might be repurposed, but its ordering, bounds
   and preservation of old proof must be designed. Allocation high-water and
   proven-durable high-water must never be conflated.

4. **Two slots improve record resilience; they are not a complete proof.**
   Specify slot generations/checksums, root identity, independent failure units,
   selection rules and successful sync before acknowledging the covered writes.
   A torn unacknowledged update can fall back to an older intact proof. However,
   that fallback is not permission to lose a previously acknowledged durability
   boundary; corrupted acknowledged proof requires explicit error handling.
   Test reused-slot tearing, crash after write before sync, and repeated crashes.
   The root must retain old proof until the new recovery state is durable.

Recommended next design comparison: (A) root incarnation plus durable run/epoch
proof retained until references are checkpointed away; (B) monotonic allocation
with persisted reservation state and explicit durable/uncertain intervals or a
completed recovery normalization. Either may reuse existing startup/barrier
writes, but neither is yet proven history-free or I/O-free. Keep the original
per-file recovery and GC/fold retention requirements. Do not add a payload
journal or weaken fsync guarantees merely to simplify the proof.

Add deterministic counterexample gates: durable100 -> lost101/102 -> durable103
must not certify the holes; repeated failed barriers must not defeat allocation
bounds; crash twice before the first new successful barrier must never reuse an
issued identity. Include a fresh client that holds or discovers an old reference
and verify it cannot obtain a false durability success. Implement no fence
removal before these representation and ordering questions are resolved.

### Response to the monotonic-sequence review (2026-10-16)

Agree; all three counterexamples are valid against `5bc66cf1f`. That proposal
claimed holes stay uncertain, but its `seq <= durable` predicate cannot express
that. The barrier failure path (`r->error`, requeue without advancing
`durable_seq`) does widen the gap on each retry. Withdraw the single-watermark
form. A concrete shape for option (B), to compare against (A) in the
ordering review:

- Record per root: root incarnation ID, `alloc_hi` (highest tag that may have
  been issued), `durable_hi`, and a list of uncertain intervals.
- Invariant: no tag above the persisted `alloc_hi` is ever issued. Barrier
  record writes extend `alloc_hi` by a fixed block, so this normally needs no
  extra write. Crossing a block without a successful record write stops tag
  issue for that root (fail closed); ack-free retries do not consume tags.
- Startup appends `(durable_hi, alloc_hi]` to the uncertain list and persists a
  new `alloc_hi` above it before issuing any tag. A repeated crash only appends
  the same or a larger interval and never reuses a tag.
- Predicate: proven iff `tag <= durable_hi` and not in any uncertain interval.
  An interval is removed only after reconciliation has durably removed or
  replaced every reference to it.
- Cost: one interval per unclean stop until reconciliation. Whether the list
  stays small in practice, and the reconciliation design itself, remain open.

This is a candidate only. The counterexample gates above apply to it unchanged.

### Review of reservation and interval candidate (2026-10-16)

Reviewed `c8bec0041` against `src/server/store.c` (`put_sync_record_write`,
`put_sync_barrier_locked`, `put_sync_note`). The reservation and exception set
address the three earlier counterexamples in principle. This is design review,
not implementation acceptance; runtime and crash qualification remain NOTRUN.
Resolve these details before choosing option (B):

1. **Retiring an interval must prevent reference resurrection.** Suppose lost
   tag 101 is excluded, later `durable_hi` reaches 200, and reconciliation
   removes all stored references to 101. Deleting its interval now makes 101
   satisfy the predicate again. A delayed client publication or retry can
   reintroduce it. Retirement therefore needs a durable admission rule that
   rejects these obsolete references permanently, or must retain the exclusion.
   A scan finding no current references is insufficient. Include retained file
   versions, snapshots/readers, pending publications and replay paths in the
   proof. Test a stale publication both before and after another restart.

2. **Specify the two commit points explicitly.** Reservation commits only when
   the coherent root record (identity, allocation ceiling, durability boundary
   and complete exclusion set) has successfully reached stable storage. Startup
   must issue strictly above the *previous* allocation ceiling, and only after
   that new reservation commits. Durability commits only after the data barrier
   succeeds and its corresponding proof record is durable; reserved capacity is
   never proof of data persistence. Install the resulting state under the root
   lock before admitting dependent acknowledgements. A failed/torn update must
   neither authorize its new reservation nor advance its durability boundary.
   Preserve the earlier two-slot recovery rules and per-file metadata commit
   requirement before successful fsync. Failed barrier retries must keep the
   enrolled object identities stable rather than allocate replacement tags.

3. **Bound the representation without discarding evidence.** The current record
   is fixed at 192 bytes and has no interval list. Specify interval coalescing,
   capacity, overflow refusal and crash-safe checkpoint/retirement, including
   repeated crashes before reconciliation completes. A clean restart must also
   avoid reusing previously reserved tags. Reusing existing barrier writes may
   amortize reservation I/O, but startup, reservation exhaustion and interval
   storage/checkpoint costs still need measurement; no zero-extra-I/O or
   no-slowdown result follows from this sketch.

Add gates for a crash immediately before/after reservation persistence, after
issuing the first reserved tag, and after data sync but before proof sync; then
repeat each crash before recovery completes. Include full reservation/interval
capacity and stale-reference retirement. Keep W123 PROPOSED until the ordering
review and existing correctness/performance gates are satisfied.
