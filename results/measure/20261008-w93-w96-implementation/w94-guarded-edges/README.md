# W94 inactive bounded guarded lease edges

One FileID/session lease record now supports sequenced OPEN/CLOSE and exact-
token pending cleanup. Higher per-file sequences supersede older arrivals;
exact retries return recorded verdicts, and changed payloads are refused.
Read-only outcome probes never acquire ownership or advance acknowledgements.

A separate session/shard replay domain retains at most 16 verdicts (973 encoded
bytes). Closed records remain in the same lease key, are not active ownership,
and cannot exceed that unacknowledged verdict bound. Contiguous client ACK
retirement, the new effect and its replay verdict share one KV batch. An ACK
never deletes a newer OPEN on the same key. Overflow refuses the new effect.
Replay floors are forgotten only after completed global revocation plus a
durable local rejection floor; old ownership is removed first.

Tests cover all 24 four-edge arrival orders, late OPEN and CLOSE, payload
collision, pending/normal ownership, batch failures, full-window pressure,
fixed encoded byte bounds, local/global fencing, stale old epochs, directory
refusal, malformed ownership/window and nine process-exit/cold-reopen phases.
After cold acknowledged tombstone retirement, delayed OPEN stays stale.
Exact final complete Linux tests and focused ASan/UBSan pass (metadata/session
code sanitized; normal linked KV library). Newly added final negative cases
were separately rebuilt and run after the full suite.

The shared lease-scan/format integration also passes the full AMD direct/TCP
fixture `gc-direct-ofxe2t00`: POSIX, peer64/64, legacy atomic CREATE/rejected
replies, replay/sync/shutdown, cold WAL recovery and physical GC. Installed four
daemons/two mounts were restored. That live binary preceded the final read-only
probe and malformed-verdict checks; those are covered by the exact Linux suite.

No production wire caller uses guarded edges yet. Legacy edge calls refuse the
new format. Unknown CREATE must be resolved before abandoning a known token;
this protocol does not cancel a late unknown CREATE by guessing its inode.
Client ownership/retry handoff, production runtime sessions, cross-group pending
composition and wire-level lost-reply/failover gates remain before activation.
