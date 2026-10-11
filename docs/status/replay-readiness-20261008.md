# Replay, writeback and production readiness — Oct 8, 2026

Authorized on devel: replay/writeback correctness, followed by durability and
production activation readiness. AMD is one physical host, four daemons and
software RXE. Functional cycles use direct-I/O/TCP. No benchmark source or
benchmark harness changes belong to this package.

## Checkpoint

| Item | Current work / exit gate |
| --- | --- |
| W64 | `9ddcf7a2`: durable inode-shard reservation receipts, bounded ACK retirement and exact replay. Full AMD units, lost committed replies, 32 concurrent identical requests, explicit inode-leader death, cold replay and exact bytes pass. Coordinated EOF/routing and bounded completion now pass AMD direct/TCP append/fsync, peer extension, leader-death replay, POSIX and cold-byte gates (details below). Legacy requests retain their old reserve path; D25 remains staged. |
| W88 | Production REPORT identity and receipt authority must replace evictable trailer history before activation. Actual host/FUSE eviction, overlapping writes, GC/re-PUT and cold bytes remain required. |
| D27/0a | `011ae854`: hostname resolution now respects inherited RPC deadlines; at most eight detached NSS jobs retain their own completion state. Hung NSS, saturation, late completion, FD cleanup and thread-creation faults pass, including AMD ASan/UBSan. Explicit stop now drains new partial bodies through the per-inode recovery path and waits for detached REPORT ownership; offline fsync/close, recovery, graceful leader stop and cold bytes pass below. Whole-write fault/recovery and small-host RSS acceptance remain open. |
| W23/W89 | W89 measurement defect closed by the Oct 10/11 AMD raw/TSV and fresh real-cap/recovery/cold-value gate ([checkpoint](amd-non-rdma-20261010.md#w89)); D30/W23 memory/lag and sustained-stall bounds remain open. |
| Queue | Dated evidence reconciled below without removing hardware/multi-host obligations. |
| W71 | `e77e1f13`: production fragment contents, checksum/length and ancestor directory entries are fenced before PUT succeeds. Actual buffered/direct/bounce fault tests and direct/TCP live POSIX/replay/append/GC/restart gates pass. Physical power-loss acceptance remains owed. |
| W73 | Explicit O_SYNC/O_DSYNC callback publication is implemented and accepted in AMD direct/TCP gates after the W64 fix: small/queued, append/non-append, peer-before-close, offline publication failure/fsync recovery and cold crash bytes pass. Historical rejected candidate evidence remains preserved; hardware power-loss acceptance is W71. |
| W72/W86 | Real production sessions, touched-authority registration, durable revocation and ticket/storage fences remain pending. |
| D25 | Remains staged: write-path integration and bounded history/materialization are still required before logical resize activation. |

## W64 implementation details

ACK-bearing APPEND is 36 suffix bytes; old 20/28-byte requests and old 45-byte
Raft entries retain legacy behavior. New 53-byte log entries include sequence
and ACK. The durable window has a separate kind (31), keyed by inode and
session identity, on the inode shard. The successful reply stores requested
length and exact reserved offset. Sixteen unacknowledged results fit per stream;
full windows return BUSY without changing reservation or cursor state. Client
ACKs reuse the existing bounded in-flight identity tracker and refresh safely
between RPC attempts. A lower outstanding sequence prevents retirement past it.
An ACKed result without an exact cached offset fails STALE. Receipts survive
resize and are purged in bounded batches when the dead inode has no open lease.
IO/allocation failures during durable reservation apply retry the log entry.
Mount owner tokens now derive from the random mount identity rather than the
small inode namespace tag. Production session registration/fencing is W72,
not implied by these fixed-epoch legacy owner identities. Stream floors remain
until inode GC; session-driven floor reclamation is still separate work.

The original W87 incident is retained; these passes do not identify its sole
cause. Cross-lane EOF observations during concurrent failover remain an explicit
acceptance obligation. No rolling downgrade of the new log format is claimed.

## Cross-group append blocker — original reproduction

The W73 candidate's large O_SYNC+O_APPEND write returned 1,048,528 bytes
instead of 2 MiB. Trace: the first callback published successfully, then the
next reservation returned NOT_PRIMARY (-15). The inode was 16385 (group 0).
A single-group inode leader cannot pass `host_read_inode_lanes` after group-2
lanes activate, and redirects to a dual host. The dual follower requires the
inode leader for append ownership and redirects back. This is a source-backed
routing conflict; repeating a whole RPC or trusting a local incomplete EOF is
not a correctness fix.

Required continuation: capture authoritative EOF and protect the inode,
reservation/result and relevant lane state through coordinated validation and
commit. Replicas must apply the same chosen reservation, not independently
compute offsets from differently hosted lane rows. Cover append after each
publication, overlapping non-append extension, concurrent clients, changed
leaders and lost replies. Keep durable replay and legacy compatibility tests.

W73's proposed explicit write-callback publication is preserved in
[the unaccepted patch](../../results/measure/20261008-replay-readiness/w73-not-accepted.patch).
It was removed after that failed run. After the coordinated W64 fix, the callback change was reapplied and gated successfully (see W73 acceptance below). The opt-in live
`--append-sync-boundary` exercises repeated 128 KiB appends with explicit fsync,
peer visibility before close and cold crash bytes; it does not require W73.
A single large write followed by fsync passed. Repeated 128 KiB writes
with fsync reproduced EIO without the W73 callback change in fixture
`/data1/efs/gc-direct-6geaxxur`: ino 8193 successfully published through ci 1,
then its next reserve returned NOT_PRIMARY (-15). This confirms that the
routing blocker precedes W73; intermediate publication boundaries need their
own gate.

## 06:44 UTC continuation — staged append reservation transaction

`efs_meta_capture_append` now captures one complete EOF and exact source/output
images for the inode guard, cursor, durable receipt, reservation and every active
lane. `efs_meta_execute_append` validates that complete plan and freezes the
inode first using the existing exact-image PREPARE/DECIDE/RESOLVE protocol.
Followers install the chosen offset; they do not recompute it from partial
hosted views. Capture never mutates state; an exact replay has zero participants.
The bounded plan has at most 68 images and uses caller-owned heap storage.

AMD targeted tests and ASan/UBSan pass: extension racing capture aborts without
reserving, competing owners conflict, inode freeze blocks cold activation and
legacy reservation mutation, all 64 lanes (including missing stamps) participate,
full receipt windows refuse allocation, and a committed overlay cannot combine
a new cursor with an old raw foreign-reservation scan. Lost COMMIT and abrupt
LSM exit, with foreign-lane resolution before inode resolution, recover the same
offset or the original absent reservation according to the durable decision.
The full AMD `make -j12 test` passes, console `/tmp/efs-append-txn-unit.log`.
[Exact source hashes and commands](../../results/measure/20261008-replay-readiness/append-transaction-tests.json)
record this staged implementation.

**Production APPEND routing is not changed by this prerequisite.** Next:
coordinate reservation completion and lane-barrier cleanup with the same inode
and cursor guards. The existing resolution may delete many DONE rows at burst
completion; preserve those semantics and recovery while keeping individual
PREPARE/apply operations bounded. Avoid mixing committed overlay cursors with
raw reservation scans. Then wire ACK-bearing APPEND to dual-host capture/commit,
retaining legacy decoding, and run `--append-sync-boundary`, lost-reply/leader
churn, mixed append/non-append extension and broad POSIX/peer gates before W73.

## W64 bounded completion prerequisite — 07:06 UTC

Coordinated completion now captures the existing frontier/hole state machine
without mutating the source KV, then commits exact inode, cursor, target
reservation and active-lane images. Closing a burst clears all lane barriers
and the cursor atomically but retains DONE rows. Separate transactions prune
eight DONE rows per page, guarded by exact inode/cursor images; cleanup refuses
to run while any OPEN reservation remains. New reservation capture refuses to
start a fresh burst until old DONE rows are gone, including after truncate.
Durable reservation receipts survive this cleanup.

This avoids overflowing the resolver's fixed 48-item participant batch when a
large burst closes. A 200-reservation reverse-order test completes, including
lost COMMIT acknowledgement, then prunes in 25 bounded pages. Abrupt LSM exit,
partial foreign-lane resolution, reopened completion and reopened prune commits
recover exact size and original reservation reply. AMD targeted tests,
ASan/UBSan and the full unit/document suite pass. The bounded image and
per-participant checks also refuse future oversized plans before PREPARE.
[Source hashes and gates](../../results/measure/20261008-replay-readiness/append-completion-tests.json).
Production routing integration and the live append/fsync reproducer are next;
this prerequisite alone does not close W64 or activate D25.

## W64 production APPEND authority — 07:13 UTC

ACK-bearing APPEND now routes to a host with authoritative views of both shard
groups, then captures/commits one exact reservation using the tested planner.
It no longer requires that coordinating host to be the inode-group leader.
Replicas install identical chosen images; no replica independently recomputes
EOF. Legacy request/log decoding remains available. REPORT completion uses
coordinated inode/cursor/lane replacements too, and completion failure is now
returned for retry rather than silently forgetting the owed completion.

AMD direct/TCP fixture `/data1/efs/gc-direct-6np4w1hz` passes repeated 128 KiB
append+fsync through cross-group publication, two peer non-append extensions
through holes followed by append at the authoritative EOF, peer visibility and
exact bytes after a daemon crash. Lost replies, 32 identical requests, explicit
inode-leader death, cold retry, ACK retirement and changed-payload refusal pass.
Single-client POSIX: **216 PASS, 0 FAIL, 1 SKIP**. Two-client POSIX: **64/64**.
Concurrent append during GC retains all **800 exact cold records**; physical
fragment deletion, quotas, late-PUT fencing and restart inventory pass.
[Fixture manifest](../../results/measure/20261008-replay-readiness/append-authority-pass/manifest.json),
[gate console](../../results/measure/20261008-replay-readiness/append-authority-pass/console.log),
[append/extension cases](../../results/measure/20261008-replay-readiness/append-authority-pass/append-sync-boundary.json).

A combined shutdown run exposed a separate client drain problem after a failed
PUT: clean stop retained a new partial dirty body until its 60-second deadline.
That failure is being addressed separately, not counted as a passed shutdown
gate. Physical power loss, multi-host transport acceptance, production session
fencing, bounded transaction-history retirement and D25 activation remain owed.

## D27 explicit-stop recovery — 07:20 UTC

The combined gate initially failed clean stop after a leader restart. The
pressure-reclaim flush intentionally skips unpublished partial chunks; using
it for stop left a failed new partial write dirty until the 60-second deadline.
Explicit stop now snapshots at most 64 distinct dirty inodes per pass and
uses the same per-inode flush/recovery path as close/fsync. It holds no cache
mutex during network I/O, retains failures, and inherits the stop deadline.
REPORT then releases that page before the next pass. Stop also waits for
`inflight_records`, not only records still in the pending queue.

AMD units cover bounded pages, duplicate inodes, unlocked I/O, failure
retention, expiry, detached REPORT ownership and deadline restoration. The
full unit suite passes (`/tmp/efs-stop-dirty-unit.log`). The complete direct/TCP
append/replay/POSIX/GC run now passes graceful leader shutdown and exact cold
acknowledged bytes; a second run validates the final in-flight ownership check.
[Shutdown evidence](../../results/measure/20261008-replay-readiness/stop-flight-pass/shutdown-mutations.json).

The new deterministic `--stop-dirty-recovery` gate stops all four private
daemons after a new partial write. Both fsync and close return **EIO**. After
restart, clean stop drains the retained **45 bytes in 0.270 seconds**, and a
cold remount reads the exact SHA-256-verified body. No force-discard is used.
[Recovery evidence](../../results/measure/20261008-replay-readiness/stop-partial-pass/stop-dirty-recovery.json).
The failed combined run is retained under `shutdown-before-fix`; its failure
remains recorded rather than being counted as a pass. D27 whole-call recovery
and small-host memory acceptance are still separate obligations.

## W73 synchronous callback publication — 07:28 UTC

Both production FUSE write entry points now finish flush/REPORT before
acknowledging positive O_SYNC/O_DSYNC writes. Publication runs after write
admission credits and append locks are released, within the inherited call
budget. Publication failure returns an error and retains recovery ownership;
ordinary writes retain their existing behavior. O_DSYNC currently uses the
same conservative full-publication fence as O_SYNC.

AMD direct/TCP fixture `/data1/efs/gc-direct-po64tbre` passes 31-byte and 2 MiB
writes for O_SYNC and O_DSYNC, with and without O_APPEND. A 2 MiB write must
return all requested bytes; no short result is hidden or retried by the test.
The peer verifies every body before the writer is allowed to close. Actual
FUSE callback trace is required, then daemon crash/restart verifies exact
hashes. Explicit append/fsync and peer sparse extensions run alongside these.
[All cases](../../results/measure/20261008-replay-readiness/sync-publication-pass/synchronous-writes.json).

The same run passes lost-reply/leader-death append replay, deterministic dirty
stop recovery, graceful leader shutdown, all **216 POSIX + 64 peer tests**
(0 failures, 1 POSIX skip), **800 exact cold append records** during GC, physical
fragment inventory and late-PUT fencing after restart. Full AMD units pass in
`/tmp/efs-sync-retry-unit.log`; extracted production callback tests also verify
ordinary-write behavior, both synchronous flags, failure propagation, empty
writes and inherited deadline restoration.
[Gate console](../../results/measure/20261008-replay-readiness/sync-publication-pass/console.log).
The earlier failed candidate remains historical evidence, not an outstanding
routing failure. Hardware power loss and the final multi-mode matrix are owed;
D25 remains staged.

## W64 bounded capture memory and streaming frontier — 07:34 UTC

Coordinated completion now emits a bounded image set directly. It no longer
allocates a copy of every reservation and every historical deletion merely
to filter them out. Ordered reservation keys allow the frontier to advance
from its current offset until the first OPEN range; completed prefixes are
not rescanned. OPEN enumeration uses a bounded page from that frontier, and
foreign-owner checks stream their rows without a burst-sized allocation.
Legacy APPEND_RES log replay retains its original deletion semantics.

The measured 200-reservation reverse-completion unit visits **399 frontier
rows**, replacing full-burst collection per completion. An eight-entry OPEN
page visits **8 rows**; a count-only query reads the cursor without scanning.
Prepared inode ownership is checked before scanning, exact image/CAS checks
still protect all mutations, and malformed reservation state fails closed.
Lost COMMIT, partial participant resolution, bounded retirement and abrupt
LSM recovery still pass. AMD targeted tests, actual meta/txn ASan+UBSan, full
units and the complete direct/TCP synchronous-write/append/replay/POSIX/GC/
shutdown/recovery gate all pass.
[Measured unit evidence](../../results/measure/20261008-replay-readiness/streaming-append-pass/unit-evidence.json),
[live console](../../results/measure/20261008-replay-readiness/streaming-append-pass/console.log).
Foreign-owner admission remains linear in the outstanding reservation rows;
this change bounds memory and removes completion's full-burst copies, not a
claim of thousand-client metadata scalability.

## D27 append retry budget — 07:40 UTC

The FUSE reservation loop previously kept sleeping after an expired RPC budget
and reacquired the append stripe with an unbounded mutex lock. It now refuses
new RPCs after expiry, clamps the backoff to remaining time and uses the
existing budgeted stripe acquisition. Reservation identity remains unchanged
through all retries. Success retains the caller's stripe; failure releases
only a stripe the caller still owns, including failed admission and extent
validation. A timed-out caller cannot unlock a competing writer's stripe.

Actual production-function tests cover expiry before/after RPC, a one-millisecond
remaining budget, contended reacquisition, identity across three attempts and
all failure ownership paths. AMD targeted and full units pass. The complete
AMD direct/TCP sync/append/replay/POSIX/peer/GC/shutdown/cold recovery run also
passes on this final code. [Unit evidence](../../results/measure/20261008-replay-readiness/append-budget-pass/unit-evidence.json),
[live console](../../results/measure/20261008-replay-readiness/append-budget-pass/console.log).
This closes this retry-loop timing gap; durable unknown-outcome ownership and
whole-write fault/RSS acceptance remain D27/W72/W88 work.

## W73 failed synchronous publication recovery — 07:43 UTC

The live gate now cuts off all four private daemons after opening a synchronous
file, then submits a 46-byte partial body through each O_SYNC/O_DSYNC callback.
Both writes return EIO; the actual callback trace proves the body was accepted
before publication failed. After daemon recovery, fsync succeeds without
resubmitting the write. A further daemon crash/restart preserves exact bytes
for both flags, and delete/reclamation returns physical inventory to zero.
This closes the live failed-publication retention gate for these two callback
paths. Mount-sync, the final operating-mode matrix and hardware power-loss
acceptance remain owed. AMD installed service was restored direct-I/O/TCP.
[Failure/recovery evidence](../../results/measure/20261008-replay-readiness/sync-fault-pass/synchronous-write-fault.json),
[live console](../../results/measure/20261008-replay-readiness/sync-fault-pass/console.log).

## Evidence

[Recorded evidence](../../results/measure/20261008-replay-readiness/README.md)
contains compact manifests, exact replay replies, POSIX/peer tables, cold append
hashes, physical GC outcomes and the rejected synchronous-write trace.

- W64 leader-death fixture: `/data1/efs/gc-direct-efv7nr7n`;
  AMD console `/tmp/efs-replay-leader-live.log`.
- W71 combined fixture: `/data1/efs/gc-direct-amp_v4y6`;
  AMD console `/tmp/efs-durable-live.log`.
- W73 failed candidate: `/data1/efs/gc-direct-ezsewfi2`;
  AMD console `/tmp/efs-sync-live.log`.
- Combined committed-source unit suite: `/tmp/efs-readiness-final-unit.log`,
  exit 0. Resolver ownership also passes AMD ASan/UBSan.

Private sources/builds are under `/data1/efs/replay-readiness-src` and
`/data1/efs/readiness-durable-src`. Installed n1–n4 stores are untouched by
private fixtures; installed service is restored as direct-I/O/TCP after each
gate. Installed binaries were not upgraded by this package. AMD is one host;
process crashes and software RXE do not establish hardware power-loss or
physical multi-host RDMA acceptance. The final four-mode release matrix is
still owed, and these partial packages do not authorize activation.

## One-hour work batch checkpoint — Oct 8, started 06:52 UTC

Six focused production changes were committed on devel:

| Commit | Change |
| --- | --- |
| `593becf3` | Bounded coordinated append completion and DONE receipt cleanup. |
| `3a2033d8` | Cross-group authoritative EOF/reservation/completion routing. |
| `a6ff3d4c` | Explicit stop flushes retained partial bodies and waits for REPORT ownership. |
| `8bf38bfa` | Both FUSE write callbacks publish O_SYNC/O_DSYNC before replying. |
| `dc9fded0` | Stream append completion and bound OPEN capture memory. |
| `141cd73c` | Append backoff/stripe reacquisition obey inherited deadlines and lock ownership. |

`e2fd20da` adds the live synchronous publication-failure/recovery gate;
`120d8292` reconciles current W64/W73 documentation without including the other
chat's staged design proposal or W90/W91/W92 edits.

The final production-source AMD direct/TCP broad run passes 216 POSIX tests
(0 fail, 1 skip), all 64 peer cases, durable replay/leader death, append/fsync
and peer EOF extension, sync writes before close, exact 800-record cold append
integrity during GC, graceful shutdown and dirty-stop recovery. Full AMD units
and the actual metadata/transaction ASan+UBSan tests pass. The failed
partial-body drain run is retained in `shutdown-before-fix`; subsequent
recovery gates pass. Both the exact committed documentation tree and the
shared working documentation pass all five architecture checks.

No benchmark code, installed AMD binaries/stores, release refs or D25 activation
were changed. Remaining blockers are production publication replay identity,
real session/PUT-ticket fencing and recovery, bounded transaction history,
D27 whole-write fault/RSS acceptance, W23/W89 pressure remedies, mount-sync and
the final mode matrix, plus W71 hardware power-loss evidence. D28/D29/D30 policy
decisions remain unresolved. Installed AMD service is restored after each gate;
the six-hour heartbeat remains authorized until its existing deadline.

Final evidence review tightened the live failure gate to match only trace bytes
emitted by the current write, preventing O_DSYNC from matching the prior O_SYNC
trace. The stricter rerun passes with both flags and full GC/restart cleanup
(`/data1/efs/gc-direct-rd5k0ll3`); `sync-fault-pass` now contains that rerun.
Installed four-daemon/two-mount direct/TCP service was restored at 07:46 UTC.

## D28–D30 acceptance — Oct 8, 2026

The user accepted the recommendations after the one-hour batch: D28 accepts
loss of unpublished buffered writes after client death and defers salvage;
D29 adds durable queryable committed/apply-pending receipts without releasing
bytes or satisfying fsync; D30 bounds pre-proposal admission and uses
replica-aware pacing with recovery/cleanup reserves. Implement D29 alongside
W88, then D30; preserve existing validation until equivalence is proven.
[Accepted contracts and gates](../backlog/decision-contracts.md#d28--client-death-and-unpublished-writes)
supersede earlier unresolved-policy statements in this dated ledger. No code
activation or new gate pass is claimed by recording these decisions.

## Merged query branch acceptance — Oct 8, 2026

`devel-find-stats` was merged into devel at the user's request. The merged
production source passes full AMD units and direct/TCP private query/POSIX/peer
and replay/sync/GC/shutdown/cold recovery gates. Installed AMD service restored;
no installed-binary rollout, push or activation. [Merge checkpoint](find-stats-merge-20261008.md)
records exact commits, source/binary hashes, results and remaining obligations.
