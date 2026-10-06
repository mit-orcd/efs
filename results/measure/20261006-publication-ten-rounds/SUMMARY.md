# D25 publication continuation — Oct 6 2026

Requested: ten implementation rounds, logical commits, stop for a design decision.
Starting HEAD 6f25ec4b; untracked Mac test artifacts preserved.

1. REPORT captured FileID flag + generation; typed preflight refuses mismatched
   generation and never silently acknowledges a deleted inode. Epoch remains
   captured, unknown/malformed flags fail closed. Build-id matching gates ABI.
2. Same-group Raft apply validates captured generation again after proposal;
   replaced/deleted FileID returns STALE, before touching mappings. Legacy
   publication retains existing deleted-inode no-op semantics.
3. Writer snapshots bind GETCHUNKS base object generation and delta count/seq;
   holes bind zero identity. Reserved unconditional base cannot be manufactured.
4. Immutable materialization verifies that exact base again, masks published
   coverage and overlays only surviving owned bytes. Changed bases and output
   aliases fail before copying. Caller fetches peer bytes for this exact identity.
5. Typed REPORT construction derives CAS/FileID/epoch from the plan, object
   placement/checksums from PUT, never from mutable cache defaults. Full-image
   publication carries observed span-list identity and rejects span PUT bodies.

Local writer-range/recovery and real cache ASan/UBSan regressions PASS.
6. Reserve exact object/snapshot publication before fragment PUT. REPORT cannot
   acknowledge unfinished PUT; failed PUT releases only the unsent token and
   retains accepted ownership. Late completion cannot overwrite a ready token.
7. Retire fully clipped ownership only after recomputing from a fresh lane view
   and bound GETCHUNKS. Pending publications and any surviving byte refuse it.
8. Capture a cache snapshot sequence under the slot lock; stale snapshots cannot
   reserve a newer publication.
9. Add cache pre-I/O reservation tied to exact FileID, chunk and sequence.
10. Add typed cache PUT completion, immutable REPORT reconstruction and exact
    acknowledgement. Ambiguous REPORT errors keep bytes/token pinned. Successful
    completion frees the accepted overlay instead of promoting it to a clean
    read image; concurrent newer ownership remains dirty.

Local ASan/UBSan cache/state regressions and writer fence merge PASS.
The first five rounds are commit 142b5473. Linux `make -j8 -B all` and full `make test` PASS in the private
`/data1/efs/review/ten-rounds` source directory on the NUC host. Test output is
[nuc-test.log](nuc-test.log). This did not exercise the running four-node cluster.

## Activation boundary and next decision

These are staged APIs: FUSE admission and both flush paths still need integration.
REPORT currently returns one batch verdict, can commit a prefix or other records
before STALE, and can lose apply verdicts from its ring. Therefore STALE is not
proof a particular publication never committed. Clearing its token and rebasing
could resurrect bytes across a shrink; retaining it is safe but can stall.

Recommendation: implement durable per-publication identity/outcome recovery before
activating typed flush. Replies should identify each record's result, with a
retry/status path that survives lost replies, restart and apply-ring eviction.
A latest GETCHUNKS object alone cannot prove whether an earlier publication was
committed and subsequently overwritten. This is additional protocol/storage work,
not a reason to activate the staged APIs with ambiguous error recovery.

No live deployment or POSIX acceptance is claimed for this batch.
