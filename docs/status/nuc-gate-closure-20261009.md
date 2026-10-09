# NUC W73/W64 acceptance — October 9 UTC / October 8 New York

Branch: `devel-gate-closure`, isolated worktree. Original production tip:
`3e1d4c6e9c7d596c1389c8e639d423bed73ced6b`. The user-reported newer production
code was not present on this branch at the first check; devel was not silently
substituted. This report distinguishes that baseline from the repairs below.

NUC is reachable again without a host reboot. Installed stores and existing
user data are preserved. Tests use unique private stores and the two allowed
mount paths under `/data1/efs`; n3/n4 private roots use `/data2/efs`. Runners
serialize through `/data1/efs/w73-w64-gates-v2.lock`, gracefully drain installed
mounts/daemons, then restore direct I/O/TCP with `EFS_COMMANDS=0` and
`EFS_COMMANDS_SERVER=0`. No installed-store force discard, forced daemon kill,
reboot, main-checkout change, merge or push was used.

## Baseline results

| Run | POSIX | Peer | Independent append/GC audit |
| --- | --- | --- | --- |
| Initial direct | 212 pass / 1 fail / 4 skip; 107.4 s | Not reached | Not reached |
| Initial buffered | 212 / 1 / 4; 72.9 s | Not reached | Not reached |
| Clean direct | 213 / 0 / 4; 81.1 s | Append EIO, BUSY cascade, 600 s timeout | Not reached |
| Clean buffered | 213 / 0 / 4; 60.9 s | Append EIO, BUSY cascade, 600 s timeout | Not reached |

Initial failures exceeded the existing 30-second `concurrent_appends` budget.
The initial direct run overlapped full-unit storage churn, so it is not an
unconfounded performance comparison. Clean repeats had no unit/generator load.
Both clean runners completed and restored installed service while SSH was
unavailable. The four skips are unsupported shared mmap and three disabled
`.efs` feature tests. A timed-out peer suite has no completed peer TSV; it is
not reported as a passing prefix.

Both modes passed the focused O_SYNC/O_DSYNC small/large ordinary/append,
peer-before-close, cold crash, failed-publication retention/fsync recovery,
dirty-stop recovery, leader shutdown, durable append lost replies, 32 identical
requests, inode-group leader death, ACK retirement, changed-payload refusal,
and exact cold-byte gates reached before POSIX. Physical last-close deletion,
late-PUT fencing and offline-member/rejoin checks also passed.

The full unit target passed after restoring committed `results/` files needed
by the documentation link check. The initial source-only transfer omitted
those files; its failure log is retained as a fixture error.

## Mount-wide sync coverage

`tests/live/gc_reclamation.py --mount-sync` verifies the actual kernel `sync`
mount flag, then writes with ordinary O_WRONLY descriptors (plus O_APPEND for
append cases), with no explicit fsync before the peer check. The writer stays
open until a peer verifies all 31 bytes or 2 MiB. Peer EOF extension, private
server crash/restart exact hashes, and physical deletion drain are required.
Both baseline and final-candidate direct/TCP and buffered/TCP passed. The final
candidate repeats use the same frozen binaries as the full regression matrix.

`--continue-regression-failure` retains POSIX/peer nonzero exit status while
running later checks; final status remains failure. Its live continuation is
now exercised: both close-fix candidates failed one peer test but still ran
and passed their independent 800-record append/GC audit. A subprocess deadline
still terminates the fixture rather than claiming later gates ran.

## Confirmed defect: close blocks the transaction needed by its cleanup

Direct fixture `/data1/efs/gc-direct-agr8e5h7` records group 2 committed through
14053, with all three replicas applied only through 14028. Entry 14029 is
LEASE_CLOSE for inode 5516, generation 1. It falls between append PREPARE
entries 14027/14028 and 14030; ABORT decisions follow at 14031 onward.

Required last-close append draining sees the prepared inode's exclusive owner
and returns BUSY. Retrying the same Raft entry forever prevents the later
transaction records from releasing that owner. The node-2 log repeats
`required lease cleanup rc=-13 index=14029 ino=5516`. Disk counters report no
GET/PUT/disk errors and fixture usage is far below quota. Decoded committed
entries and failure-time status are retained in the evidence directory.

Commit `795a17c2` records a deferred close obligation (KV kind 33, anchor-shard
key `[shard:2][kind:1][ino:8]`, generation as eight-byte value) before advancing
past transaction BUSY. A bounded leader scan retries at most 32 obligations
per pass using Raft command 32, `[ino:8][generation:8]`. It rechecks generation,
leases and exclusive ownership. Surviving leases remain protected; actual
storage errors still hold apply for retry. A retry cannot clear a superseding
generation's marker. Uncontended close avoids extra durable writes.

The original failure stores remain unchanged. `replay_close_cleanup.py` copies
all four stores, requires log progress and committed deferred retries, then
uses `inspect_close_cleanup.c` only on stopped copies. The first experiment
stopped immediately after progress and still had one marker/288 OPEN rows;
it is explicitly retained as **progress-only evidence**. The stronger final
candidate replay includes recovery time and passes on all three group-2
replicas: `held=0`, `markers=0`, `nopen=0`. A second test of an already-persisted
marker through another restart also passes (`close-replay-v20e939a`).

## Remaining peer failure and bounded active append batches

First close-fix build `47583939-close-62be222a9563`:

| Mode | POSIX | Peer | Exact cold append/GC records | Final |
| --- | --- | --- | --- | --- |
| Direct/TCP | 213 / 0 / 4; 60.8 s | 63/64; 72.1 s | 800, pass | FAIL |
| Buffered/TCP | 213 / 0 / 4; 66.6 s | 63/64; 55.8 s | 800, pass | FAIL |

The prolonged apply stall/BUSY cascade is gone, but `peer_concurrent_append`
still returns EIO. Direct client A's 400 six-byte reservations on inode 7464
span 01:06:13.662Z–01:06:39.785Z (about 26.1 s). Client B ends its backoff loop
at 01:06:39.044Z, before A publishes. Ownership persists through the entire
unpublished burst, while the competitor's 400 attempts expire first. A's
subsequent REPORT also exceeds its eight-second budget while individually
completing hundreds of append reservations. This is software behavior, not
proof of hardware saturation.

The next candidate publishes before accepting another append after 32 write
attempts or one second of an active burst. It uses the existing queued-write →
append stripe → data PUT → REPORT barrier. Reflected reservations cannot be
published ahead of their bytes; failed publication refuses the next write
while retaining prior dirty ownership. This is checked on the next write,
not a new background timer or an idle-open-descriptor publication guarantee.

Build `47583939-append-8cfacc6e9fea` includes the close fix and bounded batching.
The complete unit target passes, and captured-deadlock cleanup replay passes.
Its full direct/buffered results exposed the additional defects described
below; the accepted final handoff candidate has a separate complete verdict.

Detached full units initially exposed a fake-process fixture inheriting
SIGINT ignored from `nohup`. Commit `04e9d616` explicitly restores the fake
recorder's signal defaults. The failed log is retained; the subsequent complete
unit target passes. Production process-ownership safeguards were unchanged.

## Evidence and remaining scope

[Raw evidence](../../results/measure/20261009-nuc-w73-w64/) contains manifests,
source/binary hashes, commands, runner logs, verdict JSON, POSIX/peer tables,
decoded deadlock records and bounded excerpts. Full daemon/FUSE logs and private
stores remain on NUC. Console suite wall times include cleanup; summed TSV
case times are a different measure.

No RDMA device is present on NUC, so RXE cells are unavailable, not passed.
W64 production-session fencing/unknown-outcome recovery remains open under
W72; these legacy/durable-receipt gates do not activate D25 or close that work.
Process crashes do not establish W71 hardware power-loss acceptance or physical
multi-host/RDMA behavior. W73 and W64 are not declared fully closed here.


## Append/truncate continuation

The bounded-batch candidate direct/TCP passed POSIX **213/0/4 (70.8 s)** and
`peer_concurrent_append` exact 800-record verification, but failed
`peer_append_while_truncate`: zero-filled gaps precede a retained record.
Peer total was **63/64 in 192.0 s**. This candidate is not accepted for rollout.

The failure exposed a deterministic legacy truncate defect. Its apply path
increments the epoch and resets base_size without refusing OPEN append
reservations, and does not remove a closed append cursor. In the failing trace,
80 reservations on inode 6187 remain monotonic from offset 0 through 480 while
peer truncates run. Periodic publication allows those conflicting epochs to
produce visible holes. Coordinated resize already rejects `cursor.nopen`, but
the production legacy truncate path lacked that rule.

A new in-memory test fails four assertions against the unfixed library:
refuse truncate with OPEN reservations; preserve epoch/size on refusal; retire
a closed cursor; reserve from the truncated EOF without a gap. The candidate
adds the OPEN check and atomically deletes the closed cursor with the new EOF.
The client also accepts the server's lower reserved EOF after a peer truncate,
rather than retaining a cached high-water and re-extending it via REPORT.

Candidate `04e9d616-trunc-ce7a75e0788e` was superseded during its unit phase,
before borrowing installed mounts. Its owned unit child completed successfully;
it has no claimed live acceptance. The final handoff candidate below includes
the truncate repair.


### Fair handoff refinement

The bounded-batch buffered run passed POSIX **213/0/4 (60.9 s)** and the 800-record
cold GC audit, but peer concurrent append still starved (63/64 overall, 90.2 s).
Client A published 27 records, then B repeatedly renewed ownership while A's
backoff missed the short free intervals. Publication opportunities alone are
not a fair handoff.

The metadata regression is corrected in `e5d95c1d`; its unit assertions
now pass. The superseded truncate-only runner was stopped during its unit phase,
before borrowing live mounts; its owned unit child finished. The revised
candidate is `04e9d616-handoff-de2f8b286694`.

A per-inode publication flag prevents every local append writer from renewing
ownership while the existing PUT/REPORT barrier drains. After success, all local
reservation attempts honor a 100 ms handoff interval, longer than the existing
64 ms maximum BUSY backoff. Waiters release the append stripe and retain the
original operation identity/deadline. Failure clears the scheduling flag,
returns the publication error, and retains existing dirty data for recovery.
The interval deliberately adds latency/publication traffic to long active
append bursts; this is correctness/progress acceptance, not a throughput win
or a physics-limit claim. No general multi-host fairness guarantee is inferred
from NUC tests. Full final validation is recorded below.


## Final candidate validation

Frozen build `04e9d616-handoff-de2f8b286694` contains the production fixes now
committed through `393f5bae2c00`. Its production-source SHA256 is
`7d080221cba7aec4163a3a2a1732d5a69e160dd7d10f8499e110dc6766150fc2`.
The focused, mount-sync and full regression fixtures use identical source and
binary hashes; no recompilation or generator load overlaps the timed suites.

The complete native unit target passes. The captured original deadlock replay
finishes with zero held entries, deferred markers and OPEN reservations on all
three group-2 replicas (`close-replay-qbf_w7lg`). Restarting a copy that already
contains a durable marker also drains it (`close-replay-v20e939a`). Eight focused
peer append/truncate races, with cold restart after four, and exact 800-record
concurrent append pass (`gc-direct-gxr09c66`). Both final mount-sync modes pass.

The final direct/TCP run passes POSIX 213/0/4 in 76.2 s, peer 64/64 in 193.9 s,
all requested focused recovery gates, exact 800 cold append records during GC,
and physical GC drain/restart verification (`gc-direct-dg0mpf4q`). Final
buffered/TCP also passes all these gates (`gc-buffered-6yeq7rfk`).

| Final mode | POSIX pass / fail / skip | POSIX wall time | Peer pass / total | Peer wall time | Cold append / GC |
| --- | --- | --- | --- | --- | --- |
| Direct / TCP | 213 / 0 / 4 | 76.2 s | 64 / 64 | 193.9 s | 800 exact records; PASS |
| Buffered / TCP | 213 / 0 / 4 | 60.8 s | 64 / 64 | 193.5 s | 800 exact records; PASS |

Both modes include O_SYNC/O_DSYNC peer-before-close and cold crash checks,
failed-publication retention/fsync recovery, dirty-stop recovery, shutdown
mutations, append sync-boundary checks, durable replay/identical-request races,
leader death, ACK retirement, physical last-close reclamation, late PUT fencing
and offline-member/rejoin recovery. Both final verdicts are PASS; runner exit
is zero after installed direct/TCP service restoration. The four POSIX skips
remain shared mmap plus the three explicitly disabled `.efs` tests.

The isolated 800-record focused handoff case took 4.5 s, while the full peer
suite took about 194 s. These are different fixture histories and workloads;
the evidence supports correctness/progress, not a general performance claim.
### Installed deployment

Committed production source `393f5bae2c00` was rebuilt cleanly on NUC after the
matrix completed, with the identical production-source SHA256. Its new build
identity and binary hashes are recorded separately from the frozen gate build;
build metadata changed, not production C source. `efsd`, `efs-fuse`, `efs-mgmt`,
`efs-bench` and `efs-query` are installed under `/data1/efs/src` and report this
commit and branch. The previous complete installation is preserved at the
path recorded in `deployment/source-backup.txt`.

Deployment gracefully drained both mounts and four daemons, retained installed
stores, then restarted direct I/O/TCP with both `.efs` switches disabled.
Verification finds exactly four installed daemons, two installed FUSE clients
and both mounts. Peer-before-close O_DSYNC ordinary and append writes, followed
by peer truncate and exact new append, pass on the installed cluster. The owned
smoke tree was removed. Deployment exit is zero. Commands, build log, source/
binary identities, process configuration, mounts and smoke JSON are retained
under the local raw evidence `deployment/` folder.

### Harness corrections retained in the evidence

A detached unit run exposed the fake perf fixture inheriting ignored SIGINT
from nohup. Commit `04e9d616` restores signal defaults in the fake executable.
The extracted append-reservation unit initially lacked the new helper; the
actual-function harness now covers publication wait/handoff, stripe release,
identity preservation and lower authoritative EOF. Full units pass afterward.

An initial focused harness placement ran before mounting, leaving its private
test tree on the underlying mount directory. A runner text replacement also
corrupted the restoration transport argument. Both failed logs/scripts are
retained. The owned orphan mount was gracefully stopped; the accidental tree
was moved into `gc-direct-pcvbssfk/unmounted-mount-setup-tree` and installed
service was restored and verified. The corrected focused run starts after
formatting and mounting and passes. Existing installed data was preserved.
