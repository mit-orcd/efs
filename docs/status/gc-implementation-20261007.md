# GC implementation checkpoint — Oct 7, 2026

[Status](README.md) · [Incident review](gc-reclamation-review.md) · [Decisions](decisions.md)

Review routing: [W61/W62/W63](README.md#1a-the-work-queue) identify the
committed repairs (`3a1b4a52`); [W86](../backlog/work-items.md#w86) tracks staged
PUT tickets, [D31](decisions.md#d31--recorded-abandoned-upload-policy) indexes
the policy recorded below, and [W87](../backlog/work-items.md#w87) retains the
buffered append failure. These remote fixture outcomes are checkpoint reports;
round 6 did not retrieve their raw logs or query current processes.

## Approved unpublished-body policy

The user approved: **discard fragments uploaded but never published only after
that client's session has been durably revoked; implement durable PUT tickets**.
Age, a disconnect, a local fence, and a missing publication receipt are not
permission to delete a body. This resolves the GC policy for abandoned PUTs on
still-live files. It does not promise salvage of pending writeback or change the
successful-fsync requirement; the broader D28 durability scope remains separate.

Ticket admission must precede PUT and bind FileID, session epoch, immutable object
identity, body range, coding and fragment evidence. Publication must transfer
ownership atomically with its mapping. Collection requires the completed global
revocation barrier, the lane's durable rejection floor, and durable data-plane
fences on every configured member before physical deletion. Missing/offline
members retain the ledger. Retirement must leave a replay floor.

## Commits

- `3a1b4a52`: production reclamation correctness, fresh object identities,
  dead-inode fencing, diagnostics and physical delete gates.
- `6d6056c3`: W85 PUT retry/root-hint accounting.
- `b3a11877`: staged durable PUT tickets, atomic ownership, all-member ACK
  ledger, retirement floors, tests and public-header rebuild dependencies.

## Implemented production GC repairs

- W61: exact local/forwarded cleanup verdicts; KV I/O/resource failures retry apply
  instead of advancing a replica past failed state changes.
- W62: whole-record capacity reservation includes every delta identity. Legacy
  oversized truncate fails atomically rather than dropping unqueued references.
- W63: `efs-mgmt gc-status` reports pass/stage, actual removed bodies/bytes, quota,
  failures and sampled pending/reap/orphan state; independent health heartbeat.
  Marker ages are runtime observations, not a durable globally oldest deletion age.
- Conditional physical deletion uses stored geometry across buffered/direct mode
  changes; unreadable/short files retain their GC records. Parent directory sync
  precedes acknowledgements. Restart recounts physical payload usage; stale usage
  snapshots cannot overstate or understate current quota. Recount costs a startup
  inventory scan and fails closed on scan errors.
- Confirmed dead inodes collect unpublished bodies across **all** configured
  storage members, with bounded per-RPC work and durable compact death bitmaps.
  Late PUTs cannot resurrect these inodes after restart. Empty inode scaffolding
  is removed safely. Inode identities must never be reused while death bits exist.
- A zero-link candidate index and bounded startup discovery close the race where
  last close precedes transaction RESOLVE. Reclaim rechecks leases and transaction
  exclusion; a relinked inode is preserved. Session apply failures also retry.
- Fresh immutable PUT identities prevent an A→B→A content-name revival from racing
  superseded-object GC. REPORT requires local PUT ownership. Clients and servers
  must be upgraded together; old content-derived writers remain unsafe.
- Span-only truncate tails use the owned-image flush pipeline and preserve the
  server delta list rather than replacing it with a zero stub.

These changes are tested in isolated NUC stores. The existing NUC services and
xorinox incident stores have not been rolled or repaired by this checkpoint.
Previously lost references whose inode and reap ledger are already absent need
an authority-safe inventory reconciliation; blindly deleting inventory is unsafe.

## Tests and limits

`make test-gc-live GC_MODE=direct` creates four private NUC nodes across SATA and
NVMe roots; it does not mutate an existing cluster. The test checks physical file
inventory and allocated bytes independently of namespace visibility, hardlink and
open-unlinked retention, span folds, truncate tails, retryable disk failures,
crash/restart without a mounted client, offline fourth-member retention, quota
recount, and late-PUT rejection on every member. Each fixture records source
hashes and storage roots. Failed fixtures are retained for diagnosis.

Direct fixture `/data1/efs/gc-direct-m_92r0h2`: **216/217 single-client tests pass
(one mmap skip), 64/64 peer tests pass**, then physical inventory and quota are
zero after restart and late PUTs are fenced. Full production-fix unit run passed. The final post-W85/post-ticket direct fixture
`/data1/efs/gc-direct-yzlxfnne` also passed 216 single-client tests (one skip),
64 peer tests and the empty-inventory/zero-quota/restart gates. The full unit
suite passed with ticket metadata included, and the final ticket test also
passed after additional capacity, acknowledgement-order and fault checks.
An attempted ASan/UBSan run could not link because the NUC lacks its sanitizer
runtime libraries; no sanitizer pass is claimed.
Buffered fixture `/data1/efs/gc-buffered-3ohdte4d`: focused GC checks passed; broad
POSIX run failed both concurrent-append cases (missing records). Ten isolated
repetitions of those two cases passed. This does **not** clear the load-dependent
failure. At this checkpoint a traced full buffered rerun was in progress; record its
result before changing acceptance. This sentence is not live job status.

## Additional quota repair: W85

A PUT's first-send proof now belongs to its fresh object/attempt lifetime.
The evictable root-hint cache returns probe on a miss and is synchronized;
eviction cannot turn an ambiguous retry back into NEW. An extracted production
helper test exercises the reproduced collision and four concurrent colliding
writers. The two-root physical store test simulates a lost ACK and a retry
scheduled on the other root: it finds the original root, creates no duplicate,
charges quota once and fully reclaims afterward.

Incremental builds now rebuild production and test objects for all public-header
changes. Missing `meta_apply.h` dependencies had left objects using an older
publication-struct layout during ticket development.

## PUT ticket implementation status

`src/meta/put_ticket.c` implements the **staged pure metadata state machine**:
bounded admission (128 outstanding tickets per lane/session), immutable identities,
body digests independent of publication CAS retries, atomic PUBLISHED ownership
inside ticketed metadata publication, revocation-gated RECLAIMING, ordered
retirement and durable compact sequence floors. The durable ledger includes
fragment placement, checksums, range, coding, payload length, captured member mask
and per-member deletion acknowledgements; retirement waits for every configured
member, including a fourth member outside the three-fragment placement. Crash/restart tests verify that a
partial revocation cannot collect a ticket and retired identities remain rejected.
Published tickets never authorize deletion of a live mapping.

**Not active in production yet.** Remaining integration is the actual FUSE mount
session lifecycle (W72), ticket admission before every production PUT, versioned
PUT validation and storage session floors, distributed authoritative revocation
coordination, all-member deletion acknowledgements, bounded collector scheduling,
and live-cluster fault gates. Member-slot masks must be bound to a stable
membership identity before activation; they cannot be reinterpreted after
reconfiguration. The current host wire encoding does not carry the
internal ticketed publication fields; activation must add and gate a versioned
encoding. A replicated lane apply must not read another group's asynchronously
applied session record as proof of a global barrier. The staged colocated-KV
reclaim helper is not a distributed coordinator.

## Still open

- Activate the ticket pipeline and its distributed/storage fencing gates above.
- Diagnose the untraced buffered concurrent-append failure; a passing traced run
  is insufficient evidence to close it.
- Reconcile the historical xorinox inventory only with authoritative live-reference
  evidence; the original 40 GiB incident is not declared repaired.
- W83 is a separate metadata lifecycle gap: durable transaction decisions need
  participant completion evidence and a replay/authority barrier before retirement.
  This checkpoint does not implement transaction-decision GC.
- D25 live-file materialization/history retirement remains staged; the bounded
  dead-inode collector does not make legacy truncate/apply work fully bounded.

## GC I/O visibility and xorinox rollout follow-up

The xorinox rollout of `cb5e86de` completed using the cluster scripts with safe
client drains, full unit tests and gateway/test-client remounts. An observed
snapshot reclaimed 24.2 GiB of payload across the three nodes; physical free
space increased from approximately 1.1–1.2 GB/node to 9.8–10.8 GB/node. Queues
were still draining, and xefs3 had one reap error; this does not close historical
lost-ledger reconciliation or activate PUT tickets. Raw rollout evidence is in
`/private/tmp/efs-xorinox-gc-final.log` on the development Mac (temporary evidence,
not a repository fixture).

GC status version 2 adds cumulative completed-operation counts, bytes, errors
and elapsed microseconds for checksum reads, file unlinks and directory fsyncs.
Counts include retries; unlink bytes are zero and reclaimed payload is separate.
These describe application syscalls, not block-device IOPS or all GC traffic:
metadata WAL, directory scans and death-fence file reads/writes are outside these
three classes. GC pass/marker ages now use the same monotonic clock as the status
reader, fixing the rollout's invalid age fields. New efs-mgmt accepts both v1 and
v2 replies; old tools require an upgrade for v2 daemons.

The cluster portal places per-node GC totals, interval rates/latency, errors and
leader-only queue samples beside peak IOPS. Rates reset after daemon replacement,
counter reset or a failed probe; unsupported counters remain unavailable. The
new telemetry has Linux fault-injection tests, a full NUC unit-suite pass, real
v2 RPC/age checks and v1 compatibility checks against xorinox. These telemetry
changes are not yet deployed on xorinox.
