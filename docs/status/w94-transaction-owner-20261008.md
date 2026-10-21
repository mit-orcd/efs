# W94 prepared transaction ownership and revocation

Pending CREATE remains staged. A transaction that can publish a session-owned
lease must be resolved before that participant acknowledges the session fence.
Otherwise an intent prepared before revocation can materialize ownership after
LEASE_DROP and leave an orphaned claim.

The staged `efs_session_prepare_owner` primitive takes an exact-value EXCL
intent on the participant's established-epoch/rejection-floor row, using the
same transaction identity and participant set as the pending lease effect.
Ordinary local session writes honor that intent. COMMIT or ABORT resolves the
owner and lease intents in one KV batch; a failed resolution keeps both intact.
Only after resolution can the local floor advance. Replayed resolution cannot
roll the floor back or recreate already-retired ownership.

This deliberately serializes prepared ownership for one session/participant.
It is a correctness baseline, not a demonstrated creation-throughput gain.
The host integration must stop new owner admission on global FENCING, resolve
outstanding transactions, retry local fencing, and acknowledge only successful
local fences. It must register every affected participant and preserve the
original CREATE identity across ambiguity. This primitive does not perform that
coordinator work and does not enable FUSE pending-CREATE adoption.

The server previously normalized local-fence BUSY to success. It now retains
that verdict in its apply-result ring, while consuming the rejected command so
later RESOLVE can proceed. IO/NOMEM still hold apply for retry. Required lease
cleanup retains its existing retry behavior.

Gates include COMMIT and ABORT, matching and conflicting PREPARE retries,
combined resolution-batch IO failure, rejection of old epochs after fencing,
full revocation/drop, and eight process-exit/reopen phases with no orderly LSM
close. These establish process-crash behavior, not hardware power-loss proof.
The live wire gate prepares a private session-row intent, requires FENCE_LOC
BUSY, resolves ABORT, and then requires FENCE_LOC success. It exercises the
server reply and log-progress boundary; it is not a production pending-CREATE
namespace transaction or a complete runtime-session coordinator.


The second AMD repeat `gc-direct-t3xivx_5` additionally rejects truncated,
oversized, zero-epoch, out-of-range-shard and wrong-group fences before proposing
any mutation. All five wire probes return INVAL, the guarded valid request
returns BUSY, and resolution permits the successful retry. Full direct-I/O/TCP
POSIX/peer, ownership, GC and two-crash WAL gates pass; installed service restored.
Session normal/sanitizer checks and the full Linux target pass. No NUC test ran.
Wire evidence (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261008-w93-w96-review/w94-fence-validation/prepared-owner-wire.txt`).


Pending-CREATE apply now checks the established local session and rejection
floor before replay lookup or recording a negative verdict. A retained old
successful receipt behind a persisted floor cannot grant adoption, and delayed
CREATE after revocation cannot install another pending claim. Tests cover absent
admission, exact successful ownership, full revocation, retained old receipts,
and delayed CREATE after real process-exit/reopen. This closes a staged primitive
prerequisite; global owner admission and cross-group preparation/draining remain
host integration requirements. No production RPC or FUSE flag is enabled.
