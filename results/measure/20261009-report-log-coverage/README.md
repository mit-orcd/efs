# REPORT shared-log coverage and overflow lanes

REPORT now waits for the shared log boundary before advertising publication
entries as durable, propagates hold/sync failures, and never silently returns
whole-batch OK for an invalid record. Background proposals advertise only
indices covered by completed sync. Lane activation for records beyond the
32-inode cache happens before taking the REPORT log hold; an activation race
returns BUSY rather than waiting for the request's own unreleased hold.

AMD direct-I/O/TCP fixture gc-direct-fvkizk15 passed: actual fragment uploads
for40 distinct fresh cross-group lanes in one REPORT, exact sparse/cold bytes,
physical reclamation, core212/212, peer64/64, exact800 cold append records,
retained session/lease/namespace gates and GC/restart. Full code regressions
exit0 and Linux build is warning-free. An actual nested shared-log hold test
proves the release waiter remains blocked with uncovered bytes until the other
owner releases; cold reopen then retains the synchronized bytes. Extracted
production helpers cover sync refusal and covered-index/background call sites.

The strengthened invalid-record check exposed a weakness in the previous live
REPORT retry proof: it submitted a sparse zero-node base while the bytes lived
in a span, and the old skip behavior returned OK. That mapping-retry claim is
superseded here: the helper now replays the actual nonzero live span descriptor,
and explicitly requires INVAL for a zero-node record. Revocation/direct-apply
proofs remain valid. The direct-Raft probe now follows leader changes with bounded
retries and requires both STALE and a nonzero submitted log index.

Sequential AMD direct/TCP sync fixtures also pass: explicit O_SYNC/O_DSYNC
small/queued/append writes and mount-wide sync ordinary writes are visible on
the peer before close, retain exact cold bytes and physically reclaim after
deletion. Explicit append/fsync boundaries and the800-record append audit pass.
Installed service restored afterward: four identical978caaf7 daemons healthy,
both original mounts serving, original stores retained and reservation released.
The candidate is committed here, not installed over the restored service.
This does not close aggregate partial-publication replay outcomes or activate D25.
