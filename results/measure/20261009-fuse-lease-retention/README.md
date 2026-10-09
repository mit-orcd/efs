# FUSE retained legacy lease ownership — AMD October 9

Baseline 136fc54f merges devel da653c33 into devel-w66 (prior tip87fb02d4).
Candidate keeps failed last-close HOLD ownership until confirmed release;
failed OPEN replies retain compensating cleanup. Reopen uses the same inode
edge stripe to finish old cleanup before acquiring a new HOLD. Clean stop
retries one retained release per iteration and refuses unresolved ownership.
Authoritative NOT_FOUND on CLOSE is terminal: the legacy handler observes the
inode already reclaimed, including a lost last-close acknowledgement. Ambiguous
transport/protocol failures remain pending and cache-pinned.

Actual production-function failure injection covers ambiguous OPEN/CLOSE,
reopen barrier, unlocked RPC, drain refusal/recovery and reclaimed-inode retry.
Existing threaded open/close and rejected kernel reply regressions pass.

AMD direct-I/O/TCP private fixture gc-direct-ezwypqum: core212/212, peer64/64,
CREATE adoption, rejected descriptor replies, guarded lease lost-reply/read-only
probe/cold replay, physical deletion effects, offline member/rejoin, cold empty
fragment inventory, zero quota and late-PUT fencing pass. See live.log.
Both installed mounts were restored; installed binaries/stores unchanged.

The first live fixture gc-direct-kc8y1zxb passed POSIX but timed out at clean
stop because terminal NOT_FOUND was retained. The corrected run above supersedes
that failure. Initial full make test stopped at docs links absent from the
private rsync copy. Local architecture gate5/5 passes; linked evidence upload
was rejected by automatic review and was not performed. Code recipes before
that gate passed (initial-tests.log); later recipes were executed separately.
The stop and threaded open extraction fixtures needed the new dependency and
updated retention expectation; repaired checks pass. middle-tests.log records
the second extraction failure; remaining-tests.log records the final suffix
through all compiled store/meta/raft/session/simulation tests. Final AMD build
has no warnings. No hardware power-loss or distributed RDMA proof claimed.

This is legacy HOLD ambiguity retention, not deferred sequenced FUSE activation.
Unresolved generation/UUID/epoch/sequence identities still require production
open-description integration; unified full mount identity and production PUT
tickets remain open. No D25 or pressure activation, push or tag.
