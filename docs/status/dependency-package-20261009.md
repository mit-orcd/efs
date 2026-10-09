# Dependency-linked correctness work — October 9 UTC

Branch devel-w66, baseline ab050595 (all then-current devel branches merged).
No other active agents per user. AMD direct/TCP is the default functional cycle;
commands remain disabled, D25 and pressure policy remain staged.

## Recommended sequence

1. D29/W88 and D30 cleanup/admission foundations: bound remote lane selection
   and append reservation/receipt cleanup; prove replay and charge state-machine
   expansion before pressure activation.
2. W72/W86: production mount session lifecycle, durable revocation and PUT
   tickets. Those provide identity/lifetime authority for publication and GC.
3. D27 plus W88 production publication: pre-write FileID/geometry admission,
   immutable accepted-byte ownership across every path, ordered receipt
   consumption/retirement, deadlines and recovery.
4. D25/W77: integrate typed publication/read views and request-wide observation,
   then lost-ACK, failover, cold-byte and partial-commit gates before activation.
5. W66: repeat pump/read measurements with the new foundations, then versioned
   routing/topology, compatibility and measured leadership scaling.

W55/W58 remain separate historical-trigger/NFS investigations; current passing
FUSE suites do not close those triggers. W71 hardware power-loss proof needs
physical fault infrastructure. These are not solved by the above gates.

## Implemented cleanup portion

978caaf7 introduces an RPC-only SWEEP_NEXT request. The lane leader validates
its route and converts it to at most eight exact chunk keys before logging;
remote reapers no longer propose the historical whole-lane sweep.
New REAP_BATCH entries carry at most eight exact reservation/receipt keys.
Their atomic delete retries cannot consume the next batch; the reap marker
remains until remaining rows are absent. Historical REAP_DONE/LANE_SWEEP
semantics remain unchanged for replay. Mixed-version/downgrade is not certified.

The first combined live gate exposed starvation: partial progress on the first
lane delayed all later lanes. Fixed the reaper to advance independent lanes on
BUSY, retain the marker, and stop on storage/authority failures. The original
180-second deletion-drain deadline is unchanged.

Validation: Linux full regression, strict decoder/verdict fixture, failed atomic
write/malformed keys, exact batch retry, actual LSM WAL crash/reopen, AMD private
GC/restart, core POSIX 212/212, peer 64/64, exact 800 cold append records, zero
physical fragments/quota after restart and late-PUT fencing pass. This does not
establish a universal memory/work bound or finish production publication.

[Raw evidence](../../results/measure/20261009-bounded-gc-cleanup/README.md).

## Installed deployment

978caaf7 is now installed on AMD, all four daemons direct/TCP. Full installed
core POSIX212/212, peer64/64 and persistence26/26 pass. Backup ab050595 retained;
reservation released, both mounts serving, both command masks disabled.
Next unresolved dependency is admission expansion accounting; do not activate
pressure/D25 from this cleanup acceptance alone.

## Admission arithmetic checkpoint

The next D30 portion saturates cross-group backlog sums, body/entry charge
multiplication and combined charges. Previously extreme observations could wrap
to small values and reopen admission. Regression cases cover local admission,
follower receive and leader eligibility, including multiplication, addition and
cross-group overflow. Ordinary capacity/recovery reserve behavior is unchanged.

AMD private source: full Linux `make all` and `make test` pass; final strict
host-pressure fixture passes on AMD and macOS. Installed service remains
978caaf7, direct/TCP, pressure off. No live activation claim is made for this
arithmetic fix. Command-specific expansion accounting remains open: the staged
16x wire-byte plus 4 KiB/entry estimate is not a universal bound, particularly
for legacy whole-lane/reap replay. Retained charges must follow log ownership
through apply, truncation and snapshot before pressure activation.

## Pending command work ownership

Admission now sums command-specific work from the actual unapplied Raft suffix
in both memory and disk stores. The pure callback reads retained bodies without
allocation or disk I/O and stops at the supplied capacity. Apply excludes the
completed prefix; replacement/truncation releases the removed suffix; cold log
replay reconstructs the same charges. Caller completion does not release work.

Bounded LANE_SWEEP_BATCH reserves at least 64 KiB for up to eight chunks and
72 base/delta GC rows plus tombstones/batch overhead. Historical unbounded
LANE_SWEEP and REAP_DONE saturate the charge: pressure-enabled nodes refuse new
admission while those commands remain pending, but committed apply still runs.
This deliberately does not certify mixed-version pressure-enabled catch-up.
Other commands retain the staged estimate and require further coverage audit,
including truncate, lane fences, transaction resolution and session cleanup.
Pressure/D25 activation remains blocked on that audit and corresponding gates.

Observations now include `pending_work`; the compaction cap harness checks that
field instead of reconstructing the superseded uniform wire-size estimate.
Scanning cost is proportional to pending entries until capacity; performance
under a full pending queue still needs measurement before activation.

Validation: AMD full Linux regression passes, final build has no warnings,
focused Raft/host/store tests pass (exact capacity, unbounded barriers, retained
ownership, replacement, truncation, saturation, snapshot prefix and cold replay).
Private AMD direct/TCP pressure-enabled GC/core/peer POSIX gate passes, including
physical deletion counters, offline member/rejoin, cold inventory/quota zero and
late PUT fencing. Installed 978caaf7 service restored with pressure off. The
last observation-only adjustment was rebuilt and focused-tested after the live
run. No pressure-cap stress or universal bound is claimed by this checkpoint.

## Fixed-capacity apply expansion audit

TRUNCATE (4,164 items), LANE_FENCE (65), transaction RESOLVE (48), and DROP
(32) now have capacity-derived minimum admission charges. Apply arrays and
bounds share constants with admission, avoiding independent magic numbers.
The staged allowance includes item descriptors, one/two key arrays and maximum
values, with four row images and fixed slack for batch/LSM copies. Large wire
payloads retain the greater wire-derived charge. This fixes demonstrable small
request undercharging; four row images is not an allocator/RSS proof.

Exact-capacity acceptance/refusal cases cover all four tags, including recovery
reserve treatment for RESOLVE/DROP. AMD full Linux build/regression passes with
no build warnings. Architecture checks pass 5/5.

The audit also confirms another activation blocker: session LEASE_DROP calls
lease, lock and append cleanup loops; session RECLAIM calls lease cleanup. A
single entry can write arbitrarily many batches. Those need leader-selected
exact bounded keys and replay-safe completion, preserving durable revocation
and append frontier semantics. Do not convert them to partial success or
silently shorten their historical replay. Other tags still need auditing;
D30 and production session integration remain open.

AMD private direct/TCP pressure-enabled core/peer POSIX and GC gate passes,
including physical delete counters, offline member rejoin, cold inventory/quota
zero and late PUT fences. Installed 978caaf7 restored, both mounts serving,
pressure off. Evidence: results/measure/20261009-apply-expansion-audit.

## Bounded session lease/lock phase (staged)

New session subcommands LEASE_LOCK_NEXT (RPC only) and LEASE_LOCK_BATCH select
and log at most eight exact lease/lock keys. Proposal first checks the session
owner through ReadIndex for completed global revocation; replicated apply checks
the local durable rejection floor, validates the entire key set and refuses
prepared EXCL ownership. Missing authority is BUSY, not cleanup completion.
Deletion plus live-inode CLOSE_CLEANUP obligations share one atomic KV batch.
Exact replay cannot consume the next batch. Other sessions and newer epochs
survive. salt=1 means only this lease/lock phase is empty, never full cleanup.

Legacy LEASE_DROP replay remains unchanged and receives an unbounded pending
charge with staged pressure enabled. Append cleanup, bounded close effects,
receipt retirement and production coordinator integration remain open. The
selector uses fixed memory but can scan unrelated shard keys; no CPU bound is
claimed. Previous audit wording about RECLAIM was imprecise: guarded retire
calls lease cleanup, whereas the session RECLAIM wire command calls inode
reclaim. Both paths still require their own audit.

AMD full regression and final focused tests pass, including batch I/O failure,
prepared row BUSY/resolve, malformed/duplicate/foreign keys, local/remote global
barriers and exact capacity. Actual WAL crash/reopen passes. Live cleanup RPC
and physical reclaim pass; core POSIX212/212, peer63/64 due to append/truncate
EIO. This full candidate is not accepted yet. Failed fixture gc-direct-igaguvk4
retained; installed978caaf7 restored, pressure off. A deterministic regression
now reproduces read retry losing authoritative absence after peer truncate.

## Peer truncate authoritative read absence

The preceding peer EIO is fixed: published-image retry uses the successful
GETCHUNKS absence bitmap instead of fetching the obsolete mapping deliberately
retained in the local staging table. A confirmed hole clears the publication
observation; authority failure still fails, and staging ownership is preserved.
Deterministic regression reproduces the original failure. AMD full regression,
eight append/truncate races plus cold restart/concurrent append pass. Combined
session cleanup/GC/core POSIX212/212/peer64/64 passes. This supersedes the first
candidate's failed peer gate above; no production activation follows.
Evidence: results/measure/20261009-truncate-read-absence.

## Retained append-resolution replay

APPEND_RES no longer converts failed KV apply into success. IO/NOMEM hold the
replicated log index for retry; deterministic business verdicts advance while
retaining their result, and malformed short input returns PROTO. Missing-row
idempotence is preserved. Current production append completion uses coordinated
transactions; this is a retained replay-path fix. Metadata batch fault injection
checks reservation identity and visibility barrier survive failure. AMD full
regression and direct/TCP combined session/GC/core212/212/peer64/64 gate pass.
Evidence: results/measure/20261009-append-resolve-replay.

## Legacy append admission expansion

APPEND_RES is now explicitly unbounded in staged pending-work charging: its
38-byte wire frame can load and delete all reservations. Enabled admission
refuses new legacy work; committed replay is unchanged and disabled policy
preserves admission. The new regression failed before the fix, passes afterward.
AMD full regression and private direct/TCP pressure-enabled session/GC/core
POSIX212/212/peer64/64 pass. Evidence:
results/measure/20261009-append-admission-audit.

Remaining close-path blocker is concrete: LEASE_CLOSE, close-bearing LEASE_EDGE
and CLOSE_CLEANUP invoke host_lease_close_effects, which calls whole-file lock
cleanup and append drain. Bounded successor commands must retain the durable
marker until all phases and frontier obligations finish. Refusing every normal
close is not a safe activation solution. Session append cleanup, receipt
retirement, production coordinator and remaining command/queue accounting remain
open; no pressure/D25/session activation or broad memory/CPU bound is claimed.

## Final writeback gates / checkpoint

Final candidate through c2712632 passes AMD direct/TCP W64/W73 gates: twelve
ordinary/append 31-byte/2-MiB fsync/O_SYNC/O_DSYNC cases verified before close
and cold crash; two mixed peer-extension lane-boundary cases; 32 identical
concurrent append requests with lost reply, changed-payload refusal, retirement,
next sequence, cold replay and leader death. Actual offline O_SYNC/O_DSYNC
callback EIO retains exact bytes for later fsync and cold recovery. Both fixtures
pass full physical GC/restart/quota/late-PUT gates. Source/binary SHA identities
and JSON evidence: results/measure/20261009-writeback-final-gates.

Installed978caaf7 restored and independently verified: four direct/TCP daemons,
pressure off, both mounts serving. All logical code changes committed on
devel-w66; no push/merge/tag or production activation. Next dependency remains
bounded last-close/append cleanup, then receipt retirement and session lifecycle
integration; D27/D25/W66 readiness remains open.

## Atomic capped final-close locks

The grant invariant is 32 rows per domain, 64 per file. Final-close locks now
validate the envelope before effects and delete one atomic fixed-capacity batch;
prepared ownership returns BUSY without partial deletion. Host BUSY persists a
close obligation so later RESOLVE can apply; storage failures retain log retry.
Old source fails six new regression assertions; fixed tests, full Linux suite,
actual host-function deferral, AMD direct/TCP session/guarded lease/append/GC,
core212/212 and peer64/64 pass. Installed service restored.
[Evidence](../../results/measure/20261009-atomic-close-locks/README.md).
The remaining unbounded last-close expansion is append drain; this portion does
not activate pressure admission, D25 or production sessions.

## Replay-identified bounded close successor

Staged deferred lease edge/probe atomically persists the exact generation/UUID/
epoch/sequence obligation with lease state and replay floor/result. The worker
performs capped lock cleanup, one cross-authority append completion or eight
DONE deletes, then exact retirement after ownership/cursor/reservations clear.
Prepared obligation blocks deferred OPEN; a fresh lease after capture aborts
cleanup before append effects. The collector has a hard 69-image allocation cap.
A single-group leader delegates through RPC-only NEXT to a dual-host follower
with fresh two-group ReadIndex; handlers never recursively forward.
AMD direct/TCP actual crash retains64 OPEN reservations; single-group node1
leadership recovers exact1MiB content plus8MiB holes, then cold inspection proves
zero obligations/reservations/locks/cursor. Full Linux, core212/212, peer64/64,
append and physical GC/restart pass; installed service restored.
[Evidence](../../results/measure/20261009-deferred-close/README.md).
Legacy HOLD/apply drain is unchanged and production FUSE does not yet use this
successor. The package remains open for production lease/session identity,
ambiguous release retention, bounded session append cleanup and receipt/session
retirement. Existing ordered frontier scans retain a CPU/time-bound follow-up;
this is a finite allocation/entry-shape claim, not whole-call timing proof.

## Bounded revoked lease/lock resume

Management now exposes `raft-session NODE:PORT cleanup-leases UUID OLD_EPOCH
SHARD`. It drains exact eight-row batches with a finite admission/work budget,
stops ambiguous transport outcomes, and requires explicit empty proof. The
server checks global barrier completion plus the local durable floor. Resuming
uses the original epoch instead of BEGINing another revocation. AMD cold
13-lease/8-lock gate, physical reclamation, full Linux regressions, core212/212,
peer64/64 and cold GC pass; installed direct/TCP service restored.
[Evidence](../../results/measure/20261009-bounded-lease-resume/README.md).
Full fencing retains legacy append cleanup: substituting lease-only NEXT would
omit abandoned reservation holes. Bounded session append and PUT-ticket phases
remain dependencies before replacing the whole coordinator or activating it.
