# Namespace session admission and revocation gates

Normal identified CREATE/mkdir/unlink/rmdir/link/rename register their served
receipt shard before the first effect. Existing shard participants use a fresh
local authoritative view instead of consulting the global owner on every request.
Both cached replies and single-log effects check the local epoch floor. Receipt
transaction PREPAREs acquire shared epoch guards; revocation cannot advance that
floor until every guard resolves. Shared guards allow parallel operations from
one mount, and RESOLVE never writes an older epoch snapshot.

Historical logs without local session rows remain replayable. Unidentified
management requests retain their legacy behavior. This change does not add
session identity to REPORT, retain every ambiguous namespace operation in FUSE,
or activate ticketed storage PUTs / D25.

AMD direct-I/O/TCP tests use disposable private stores and restore installed
service. Initial live namespace/POSIX/GC run passed. Two added cold assertions
initially failed because the helper cleaned up the parent and source before
restart; the final fixture keeps both alive until assertions finish. Those
failed logs are retained. Separate LSM tests exit without closing/checkpointing
and recover two guards, partial resolution, final revocation and delayed requests.

See live.txt, namespace-proof.txt, guard-proof.txt, cold-unit.txt and
regressions.txt for final results. AMD is one host / four daemons; these are
process/WAL recovery gates, not hardware power-loss proof.

Final boundary follow-up: first-touch-proof.txt / first-touch-live.txt record
old-epoch CREATE rejected on a separate untouched namespace shard after global
revocation, without relying on an existing local floor; the proposed name stays
absent and all seven saved receipts remain STALE after cold restart.
