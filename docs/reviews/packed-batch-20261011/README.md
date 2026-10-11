# Packed batch correctness review — 2026-10-11

Pinned source: `e5a385530223b0a5ce7bdfd6618c5a976fd87113`.

## Disposition

One actionable mixed-version availability defect reproduced in the extracted production scheduler. Acceptance remains pending. No evidence from this review establishes the cause of the reported seven-client ON failures, and no mounted or remote qualification was performed.

An active publication cohort snapshots batch capability only at admission (`pack_write.c:516`). Publication and retirement disable batching only after PROTO (`:354`, `:198`). An old server ignores unknown message 141 in its handler default case rather than replying with PROTO. The resulting NET timeout retains exact ownership correctly, but an existing cohort continues submitting the unsupported opcode even after capability refresh records a scalar-only peer. Retirement inherits the same stale batch choice. The baseline probe demonstrated three identical NET retries after a definitive global capability downgrade; no ACK consumption or byte release occurred.

`old-peer-fallback.patch` is an integration patch against the pinned source, **not a production change already installed on this review branch**. This branch predates the packing implementation; importing that implementation would confound the isolated review. Apply the patch to the release branch and rerun its qualification before accepting it.

The patch freshly probes compatibility after a batch NET error. Only a successful probe confirming scalar-only support enables the existing immutable scalar replay path. Failed probes and all-modern peers retain batch ownership/retry behavior. Publication and retirement both use this decision. It adds no transport IDs, durable tags, unbounded allocation, or alternate receipt semantics. Probing adds network work on failed batch attempts and remains subject to the existing caller RPC deadline.

## Safety review

The client validates frame length, count, version, mode, group, every digest, terminal verdict and locator consistency before exposing the reply or consuming any cache state. Zero locators remain allowed for legacy V1 receipts and the durable retired floor; rejecting them would break existing compatibility.

The server validates the entire request before session/proposal effects, obtains authoritative read barriers, and derives each final result from its own durable receipt. Partial cohort proposal/application and missing replies do not synthesize success from a shared applied index. A terminal INVAL application can cause a whole-envelope error rather than returning other exact receipts on that attempt; ownership remains retained and exact retry can recover. No demonstrated false durable verdict resulted from this behavior.

Scheduler tests preserve immutable requests after lost replies, consume committed/rejected outcomes once, retain rejected dirty bytes, retire only the oldest consumed ACK, and release credits only for exact completed retirement. Batch requests are bounded at 16 records, ACK credits at two cohorts, and helper stacks are precharged with nonmoving contexts. These checks do not replace runtime accounting or mounted stress validation.

## Reproduction and validation

Baseline production RPC test passed normally and with ASan/UBSan: 16-record bound, corrupt whole frames, partial verdicts, redirects, deadline, lost reply, retirement and invalid requests.

Baseline scheduler probe passed normally and with ASan/UBSan, including the reproduced stale active-cohort batch mode after capability downgrade. The patched production publication/retirement helpers then passed both modes, including failed-probe retention, definitive downgrade fallback and existing partial/lost-reply ownership checks.

The scheduler harness extracts actual production functions and links exact pinned publication, ACK, key, packed codec, checksum, common, erasure and portable BLAKE3 units. Network/cache boundary functions are controlled doubles; this is not a live old-server connection or a full Raft crash test. The helper's real capability probing is replaced by successful/failed probe responses, so transport timeout scheduling and deadline enforcement require integration tests.

Re-run from a clean archive (repository containing the pinned commit):

```sh
snapshot=$(mktemp -d)
git archive e5a385530223b0a5ce7bdfd6618c5a976fd87113 src include deps tests Makefile | tar -x -C "$snapshot"
patch -d "$snapshot" -p1 < docs/reviews/packed-batch-20261011/old-peer-fallback.patch
EFS_REVIEW_SNAPSHOT="$snapshot" python3 docs/reviews/packed-batch-20261011/test_old_peer_fallback.py
EFS_REVIEW_SNAPSHOT="$snapshot" WRITER_RPC_TEST_CFLAGS='-fsanitize=address,undefined' python3 docs/reviews/packed-batch-20261011/test_old_peer_fallback.py
```

No deployment, push, cluster changes, optional refactoring, or reserved ID reuse occurred.
