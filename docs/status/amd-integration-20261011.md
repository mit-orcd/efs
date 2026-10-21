# AMD integration and guarded-lease follow-up — Oct 11

## Scope and integration

`devel-amd` integrates every local branch, remote-tracking ref and detached
worktree tip captured at the start of this request. All 20 recorded tips are
ancestors of integration commit `3b3b0172`. No source conflict required choosing
one implementation over another; generated architecture HTML was regenerated
from merged sources. Shared branches and installed AMD data remain unchanged.
Starting inventory (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-integration/starting-refs.json`).

## W94 legacy guarded-lease fixture

The prior failed private fixture retains an actual CLOSE_STEP row for inode
45057, generation 1, original client UUID `39a7c4ddbdf87bb8962334ab0ad6da7c`
and sequence 12 on both inspected stopped replicas. The test used
`write_bytes()`, closing its real descriptor before attempting an absent-value
PREPARE for an artificial marker. Real CLOSE queues its own cleanup marker,
so that setup races bounded asynchronous GC rather than isolating the intended
prepared-marker guard. No marker was overwritten or deleted for this diagnosis.

The initial correction kept the real descriptor open, but the rerun still
failed: diagnostics show 60 redirects and 60 STALE replies, with zero transport
failures. Actual combined CLEAR_PENDING also creates a CLOSE_STEP marker, and
that marker remains while the normal lease is held. The original descriptor
setup therefore cannot supply the absent-marker precondition.

The final fixture uses ordinary unleased CREATE for an empty inode before
PREPARE, expected legacy OPEN/BUSY, RESOLVE/ABORT and successful legacy
OPEN/CLOSE. It never overwrites an existing marker or weakens a timeout. It
checks both physical absence and authoritative inode removal after unlink;
the earlier mounted CREATE/adoption cases retain exact-byte cold oracles. The helper now reports exhaustion counts for connection,
transport, redirect, cleanup STALE/BUSY and session BUSY without extending or
weakening its existing retry budget. Both earlier-candidate live matrices subsequently passed; their acceptance is recorded below.

## Gates and remaining scope

The first merged normal run stopped at the documentation link gate because the
isolated deployment excluded committed `results/` evidence; this was a copy
omission, not a production compiler failure. The evidence is now copied and the
full normal/fault suites pass on the complete repaired deployment. All five documentation checks pass in
the complete local worktree. Live validation uses private direct/buffered TCP
stores and the two permitted AMD mount paths, with restoration afterward.
RDMA, D25 activation and pushes remain outside this work.

W94 remains partial until the complete two-mode fault matrix, sustained-service
acceptance and full session/data-plane integration pass. W66's broader scaling
and D36 choices are unchanged.

## W72 REGISTER verdict mismatch — reproduced and repaired

Both merged normal and fault-enabled full native suites pass. The first live
direct/TCP run passes retained CREATE retry, combined reply loss and negative
cleanup, then fails ordinary CREATE negative-replay at its first CREATE.
The installed mounts restore successfully. That failed run is preserved.

The imported REGISTER-first namespace optimization only bootstraps after an
authoritative `NOT_FOUND`. Actual `apply_session_cmd` masked REGISTER's missing
owner and refusal verdicts as success. Admission then skipped owner creation
and could not establish the participant. Its mocked command test returned
`NOT_FOUND` directly, so the full native suite missed this mismatch.

REGISTER now preserves its actual business result in the apply ring. The Raft
apply layer advances committed missing/BUSY/STALE/INVAL outcomes so a later
CREATE/REGISTER or recovery operation can progress. IO/NOMEM still retain the
failed apply and retry. The actual-source regression fails on `df63bbfa`'s
masked result and passes after repair; existing namespace owner tests pass.
This preserves missing-only bootstrap and refuses stale/uncertain admission;
it does not activate production ticket integration.
Before (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-integration/register-verdict-before.log`) /
after (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-integration/register-verdict-after.log`).

Post-repair full normal/fault native suites and both earlier-candidate live
matrices pass. W72 and W94 remain partially open.

## Applied checkpoint evidence correction

Audit found that the original lease-count and new guard audit scripts treated
`[u64 index][u64 complement]` as one integer. That malformed bound could admit
unapplied log entries. The shared reader now accepts only a legacy atomic
8-byte index or a valid 16-byte index/complement record; torn, oversized and
corrupt checkpoints fail closed. A regression demonstrates that the former
reader admits an entry beyond index 1 while the corrected reader excludes it.

The same stopped 64-file control stores revalidate with correct group 0/2
indices: separate 24/783, combined 17/709. All 64 files retain their exact
committed identity/flag counts: 192 versus 128 creation/close lease commands.
The guard audit likewise validates indices 6386/5792 for the stopped failed
fixture, with combined adoption seq 11 and later close seq 12. Throughput,
latency and cold-byte observations are unchanged.
Corrected guard evidence (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-integration/guard-adoption-edges.json`),
prior invalid lease bound (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-integration/lease-proof-prior-invalid-bounds.json`),
prior invalid guard bound (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-integration/guard-edges-prior-invalid-bounds.json`).

The full repaired direct/TCP matrix now passes, including the prepared legacy
guard, authoritative inode reclaim, normal CREATE EEXIST cold replay, pending
LOCAL/HASHED cold edges and POSIX/peer. Buffered follow-up subsequently passed.
The production source is unchanged since the successful `8e0e4f3c` normal and
fault-native gates; the final fixture revision is `5360c735`.

## Follow-up captured integration

Other active branches advanced while the first AMD matrix ran. Captured Mac
`e679adb7`, NUC `d7b7e7e1` and ORCD `de348f87` are now integrated at
`611ea8ad`. The overlapping session fix preserves both REGISTER and ESTABLISH
verdicts, with both branches' regressions retained. The quota-test conflict
retains real filesystem inventory and controlled headroom, asymmetric used
bytes, mixed-device grouping and overflow checks. Both indexed Mac and ORCD
status notes are preserved; generated architecture output is regenerated.
Captured updates (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-integration/follow-up-refs.json`).

The earlier `8e0e4f3c` candidate with the corrected fixture passes full normal
and fault suites and direct/buffered TCP matrices: core 216/216, peer 67/67 in
each mode, exact cold bytes, inode/physical reclaim and zero final quota.
All four installed daemons and two TCP/commands-off clients restore, with no
private listeners left. Those passes are retained as **earlier-candidate**
evidence, not claimed for the follow-up source.
Direct (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-integration/evidence/live/direct.log`) /
buffered (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-integration/evidence/live/buffered.log`) /
restoration (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-integration/evidence/restoration-proof.json`).

## Final captured-candidate acceptance

Candidate `90a47313` (captured production source `611ea8ad`) passes fresh full
normal and fault-enabled native suites and both complete AMD TCP matrices.
Each mode passes core **216/216**, peer **67/67**, all six cold first-mutation
CREATE/MKDIR/UNLINK/RMDIR/RENAME/LINK cases without prior OPEN, prepared legacy
lease guards, retained CREATE/reply-loss/negative verdicts and cold name reuse.
Both modes also pass 32 identical APPEND RPCs, lost replies and ACK retirement,
O_SYNC/O_DSYNC visibility before close, cross-group append/peer extension,
failed-sync retained-byte recovery and exact cold crash bytes. Final restart
verifies empty fragment inventory, drained ledgers, zero quota and late PUT
fencing on every member. No native or live failures remain in this capture.

Normal, fault, native-round, direct, buffered, live-round and restoration exit
markers are all zero. The independent restoration audit verifies the original
four installed daemons and two TCP/commands-off clients; no private screens or
24190–24193 listeners remain. Installed stores and binaries were not replaced.
Acceptance and file hashes (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-integration/final-acceptance.json`),
normal native (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-integration/evidence/final/normal-native.log`),
fault native (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-integration/evidence/final/fault-native.log`),
direct (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-integration/evidence/final/live/direct.log`),
buffered (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-integration/evidence/final/live/buffered.log`),
restoration (archived raw evidence: `archive/raw-evidence-20261021:results/measure/20261011-amd-integration/evidence/final/restoration-proof.json`).

This accepts the recorded starting inventory plus the three follow-up captured
tips, not commits added to other active branches after that capture. Production
queries remain disabled; Mac/ORCD query runtime acceptance is not claimed.
W72/W94 remain partial: full production flush/PUT-ticket integration and
sustained-service acceptance remain. W64's wider failover obligations and W66's
D36/scale decisions remain unchanged. No RDMA gates, pushes or D25 activation.
