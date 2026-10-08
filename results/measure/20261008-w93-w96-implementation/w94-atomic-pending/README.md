# W94 staged atomic pending CREATE

Pending ownership now composes into the same CREATE KV batch as namespace,
inode, allocation and replay verdict. Token and lease identity come from the
same request. Invalid/mismatched/pre-acknowledged identities are refused.
Replay-window overflow returns BUSY without a namespace effect, rather than
falling back to unprotected mutation. Retired verdicts without an inode return
STALE and cannot grant adoption. Old retries do not reacquire closed ownership.

Focused injected batch-failure, retry, unlink/normal-close protection, exact
token cleanup, replay-window pressure and retirement tests pass. Real child
processes exit without closing the KV engine after atomic CREATE and each
ownership edge; cold reopen verifies the namespace and exact token/bits.
Session/metadata code and these cold tests also pass ASan/UBSan with a normal
linked KV library. Complete Linux make test on the exact final source passes.

The shared CREATE-helper integration was also run through a full AMD direct/TCP
live fixture (`gc-direct-6tmcbm26`): POSIX, peer64/64, atomic legacy CREATE lease,
rejected replies, append/sync/shutdown/replay, physical GC and WAL second-cold
recovery all pass; installed service restored. The subsequent pending-only
pressure/retirement guards and updated cold test are covered by the exact final
full suite, not by that earlier live binary.

No production wire caller uses the pending helper yet. First-use cross-group
pending transaction composition, guarded HOLD sequencing, client adoption and
durable runtime-session cleanup remain before activation. Existing legacy
atomic CREATE paths remain active and tested; this does not eliminate HOLD yet.
