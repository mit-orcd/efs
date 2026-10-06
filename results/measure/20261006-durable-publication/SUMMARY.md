# Durable per-publication results and recovery — Oct 6 2026

Accepted next step: recover exact outcomes before activating D25 typed flush.

Implemented canonical request fingerprints plus mount UUID/session/unique
snapshot identity. Identical application writes remain different operations;
changing one scoped operation's payload fails closed. Cache request size and
identity are bound before sending, so later inode growth/shrink does not alter
a retry. New Raft tag 28 retains the legacy publication encoding and appends
an explicit big-endian operation identity; old PUBLISH log lengths are unchanged.

Applied KV kind 26 stores FileID/chunk/operation -> version, terminal verdict,
request digest. Success shares the atomic mapping/lane/inode/GC batch; STALE
and INVAL without mutations are also replicated. Storage errors/BUSY leave no
terminal proof. Exact result lookup precedes current fences and mapping checks.

PUBLICATION 113/114 and PUBLICATION_STATUS 115/116 route through the publication
lane with ReadIndex. They recover durable state even when apply-ring verdicts
are missing/ambiguous. Missing records mean unknown. The staged cache resolver
acknowledges only exact successful ownership; exact rejection releases its
pending token while retaining accepted bytes/pins. Other failures retain both.

Validation:

- NUC Linux full `make -j8 test`: PASS; [output](nuc-test.log).
- Real LSM child `_exit` without close/flush: committed and rejected receipts
  survive reopen after a superseding write and fence. Original retry cannot
  restore old mappings. Atomic batch failure leaves neither mapping nor receipt;
  failed rejection storage likewise remains unknown.
- Distinct operations writing the same full image and same span are separate;
  an old span retry after a peer fold is a no-op, while a new identical span
  is published after it. Reusing an operation with changed size is refused.
- Production host adapter, publication encoder and apply decoder are executed
  with faulted wait/result adapters. Actual identity suffix round-trips; lane-only
  routing, aggregate/apply-ring STALE versus exact commit, delayed outcomes and
  query-only recovery pass. No running-cluster leader-change claim.
- Production client RPC regressions verify identity, native-padding clearing,
  redirection, bounded BUSY, malformed replies, missing results, lost send/reply,
  deadlines and unknown output on invalid requests.
- Actual cache/ownership harness and both new RPC adapters pass ASan/UBSan.
  Cache retry retains its original intended size; unknown/wrong result retains
  ownership; exact rejection keeps bytes, and exact commit releases its snapshot.
- Architecture documentation 5/5 and diff whitespace gates pass.

This remains staged: active FUSE admission and both legacy flush paths are
unchanged. Before activation finish bounded receipt acknowledgement/retirement
with replay protection, coherent lane-local mtime invalidation, caller integration
and live shrink/leader/routing gates. Receipts intentionally have no expiry yet.
The one-publication endpoint establishes correctness; lane batch coalescing is
still needed for production throughput. Server outcome persistence does not
journal client dirty bytes or promise recovery after client process loss (D28).
No live deployment or POSIX acceptance is claimed for this turn.
