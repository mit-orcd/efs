# W93 atomic apply recovery and sustained acceptance — Oct 10

Status: in progress; no closure or runtime default change yet.

All 15 branch tips captured in the retained merge manifest were integrated into
`devel-persistence-pressure`; other checkouts and branch pointers are preserved.
The first repair (`8c214f7e`) fixes a receipt race: a peer can complete an APPEND
between the initial receipt miss and the later authoritative receipt read.
That later read must return the canonical result rather than reserve again at
an advanced EOF. Six actual-source tests cover direct/captured requests,
completed/changed-payload/retired receipts. The old Linux library fails the
negative control; the repaired library passes. Linux full native tests and
local ASan/UBSan pass. The real ACK-bearing replay test subsequently passes.

The full private 100 ms gate initially exposed harness errors: its stop path
used `mountpoint`, whose GETATTR can fail while the FUSE client remains alive,
and could skip asking its own client to exit. Always run safe stop for the
owned client; allow its existing 30-second drain budget. The unchanged durability
assertions then pass: POSIX 212/212, peer 64/64, sync/append faults, dirty stop,
mutation shutdown and corrupt WAL tails. Dirty stop takes 26.728 seconds and
retains exact cold payloads. W66 now fences all storage members: the partition
test must retain an incomplete fence during partition and resume the same epoch
after healing. That current contract passes. The KV persistence failure gate
also passes after matching the current production diagnostic, with marker-byte
and cold metadata assertions unchanged.

## Real recovery defect and repair

The 1,000 ms retained-checkpoint fault revealed a real bug. After actual n3
checkpoint `pwrite` EIO and a private process crash, all eight hardlink counts
were **3 instead of 2**. Exact copied cold n3 KV confirms this. Replaying
already applied PREPARE/RESOLVE can increment the count again; a separate stale
checkpoint cannot make those operations idempotent. Do not weaken this gate.

One applied Raft entry now stages its KV effects and commits those effects plus
a validated node-local group cursor in one atomic WAL record. The staging bound
is 32,768 effect keys and 64 MiB. The applying thread sees its overlay, scans
merge it with committed state, other readers retain the old committed view,
and competing writers wait until commit/abort. No outer LSM mutex is held
across dispatch. Failed or oversized scopes cannot publish partial effects.
The cursor is committed before publishing the apply verdict or re-enabling a
previously incomplete replica's campaign. ACKs remain behind existing durable
watermarks and KV sync barriers.

The two fixed cursor keys use a shard prefix outside the valid range. Group
images neither export them nor erase the receiver's cursor. Startup restores
the maximum validated KV cursor and valid legacy checkpoint; missing KV keys
support upgrade, malformed or unreadable proof refuses startup, and a cursor
beyond the recovered Raft log refuses startup. Legacy files remain. An older
binary may only be restored after clean shutdown brings those hints current;
crash downgrade to code that ignores cursors is unsupported.

Meaningful tests cover staged get/delete/ordered scans, reader isolation,
writer exclusion, abort, failed commit, key/byte limit refusal, crash before
and after commit, and a torn final WAL record exposing neither effect nor
cursor. Actual-source cursor decoding covers upgrade, stale/torn hints,
malformed proof and IO/NOMEM/BUSY refusal. Group image tests preserve the target
cursor. Local ASan/UBSan passes these apply-scope cases. An initial Linux private
1,000 ms marker-fault smoke now passes all eight exact payload/link/count/mode/
truncate/absence checks and a cold offline n3 KV oracle. That offline helper
also had an invalid null decision callback; it now supplies the real local KV
transaction decision reader. Temporary smoke binaries identify as `unknown`;
final acceptance must use a frozen committed build and repeat the full gates.

## Remaining acceptance

Rebuild all five binaries with an exact committed ID. Repeat full native,
full POSIX/peer/sync/append/dirty-stop/WAL gates, candidate switches, KV EIO,
maximum checkpoint EIO/crash and partition/recovery gates on that code.
Only then deploy to the preserved NUC service and run four 900-second balanced
ABBA cells: 10 ms default cohorts versus 100 ms + overlap/no-op batching.
The treatment is a bundle, not isolation of individual switches. Retain three
300-second windows, per-group barrier/entry data, worker and idle/mixed read
p50/p95/p99, RSS/pressure/replica lag, exact identities and 512 cold payload
checks. No physical power-loss or distributed-RDMA acceptance is implied.

Raw controls and evidence: `results/measure/20261010-w93-closure/` locally and
`/data1/efs/w93-closure-20261010/` on NUC. Original failed private stores and
copied follower metadata remain preserved. The service cluster has not yet
been redeployed in this turn; experiments remain disabled there.

## Intermittent peer append gate

Frozen `ad69a9973384` full native passes. Its first full 100 ms live run passes
POSIX 212/212 but peer is 63/64: concurrent append returns EBUSY on client B.
The original fixture and raw logs remain preserved. A diagnostic-client full
repeat and a normal-client repeat with only open/close tracing pass 212/212 and
64/64, plus cold WAL recovery. Eight append/truncate races and an 800-record
handoff fixture also pass. These repetitions do not prove the first failure's
exact trigger or establish a hardware ceiling.

Review identifies a definite OPEN policy gap: unlike retained CREATE, OPEN
calls lease resume once and returns an unresolved BUSY to the application.
Transient transaction/close-step guards can legitimately leave this exact edge
unresolved. OPEN now waits on the retained edge within one 30-second budget,
including inode serialization, FileID and compensating cleanup. It never
extends an inherited deadline, replaces unknown identity, retries a completed
rejection, or discards pending ownership. Mutex acquisition is deadline aware.
IO/NET and terminal failures retain existing error/cleanup behavior.

Actual-source tests cover two BUSY replies then success using one identity,
terminal known BUSY, inherited deadline expiration, retained unknown edge,
blocked mutex timeout and IO ownership. The old-source negative control fails
its first transient-BUSY assertion; the repair passes. Existing shared-open,
ambiguous OPEN/CLOSE, reopen barrier and stop-retry tests remain passing.
Rebuild and repeat release-binary native and all live gates before deployment;
the original intermittent trigger is not claimed reproduced by this unit test.
