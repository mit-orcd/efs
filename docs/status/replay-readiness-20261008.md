# Replay, writeback and production readiness — Oct 8, 2026

Authorized on devel: replay/writeback correctness, followed by durability and
production activation readiness. AMD is one physical host, four daemons and
software RXE. Functional cycles use direct-I/O/TCP. No benchmark source or
benchmark harness changes belong to this package.

## Checkpoint

| Item | Current work / exit gate |
| --- | --- |
| W64 | `9ddcf7a2`: durable inode-shard reservation receipts, bounded ACK retirement and exact replay. Full AMD units, lost committed replies, 32 concurrent identical requests, explicit inode-leader death, cold replay and exact bytes pass. Cross-group EOF authority/routing remains open (see blocker below). |
| W88 | Production REPORT identity and receipt authority must replace evictable trailer history before activation. Actual host/FUSE eviction, overlapping writes, GC/re-PUT and cold bytes remain required. |
| D27/0a | `011ae854`: hostname resolution now respects inherited RPC deadlines; at most eight detached NSS jobs retain their own completion state. Hung NSS, saturation, late completion, FD cleanup and thread-creation faults pass, including AMD ASan/UBSan. Whole-write fault/recovery and small-host RSS acceptance remain open. |
| W23/W89 | Corrected follower pressure measurement and D30 remedy remain pending; no pressure closure from these runs. |
| Queue | Dated evidence reconciled below without removing hardware/multi-host obligations. |
| W71 | `e77e1f13`: production fragment contents, checksum/length and ancestor directory entries are fenced before PUT succeeds. Actual buffered/direct/bounce fault tests and direct/TCP live POSIX/replay/append/GC/restart gates pass. Physical power-loss acceptance remains owed. |
| W73 | Candidate callback integration tested; large synchronous append exposed the W64 routing blocker. Candidate removed from build sources and preserved as a rejected patch in the evidence directory. No claim of synchronous application-write support. |
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

## Cross-group append blocker

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
It is **not applied** to the committed FUSE callbacks. The new opt-in live
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
