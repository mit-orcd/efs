# W93–W96 implementation checkpoint — Oct 8, 2026

Baseline: `devel` merge `d4e70492` includes `e6926d85`. Work continues in
logical portions; none of the four performance packages is closed by this
initial checkpoint. Functional gates use AMD direct-I/O/TCP, one host/four
processes. Installed stores and binaries are preserved and service restored
between private fixtures. No physical power-loss or distributed-RDMA proof.

## Current state

| Item | Completed | Still open |
|---|---|---|
| W93 | Bounded batching, durability/fault/cold/full Linux/live gates and balanced long controls | Preserve regression coverage; W53 close tails remain separate |
| W94 | Atomic server lease, rejected replies, pending ownership/atomic CREATE, bounded sequenced-edge and cold tests | Client ownership/recovery integration, production runtime sessions and activation gates |
| W95 | Tiny/durable timing, independent retained/cold/crash tests and production churn | Indexed packing model and any P4.1 production-layout selection |
| W96 | Tail/interior-damage recovery, reservation/fallback/fault and final full live gates | Quiet NUC two-device performance acceptance; unrelated workload retained |

## W96 recovery prerequisite

Fixed replay leaving discarded bytes in front of later acknowledged writes.
Recovery validates a complete batch before callbacks, removes a torn/CRC-invalid
trailing record and durably establishes the valid EOF before append opens.
I/O and CRC-valid structural errors refuse recovery without truncating evidence.
A partial append or sync failure fences the WAL instance against later success
until reopen; reset/hold cannot erase or bypass that failure.

Validation: original production-WAL reproducer now recovers A+B. Repeated
process-crash tests cover short headers/payloads, zero padding and invalid CRCs;
post-repair acknowledged writes survive two further opens. Fault tests cover
ENOSPC after a partial write, sync EIO and malformed atomic batches; normal and
ASan/UBSan pass. AMD KV-LSM and Raft-store suites pass.

Private AMD `/data1/efs/gc-direct-20dpw3sk` passes full POSIX 216/0/1, peer 64/64,
append/sync/retry/stop/restart/GC gates, and deliberate invalid tails on all four
WALs followed by new fsynced files and a second crash. Both files match exact
cold bytes. Physical inventory/quota return to zero and late PUTs stay fenced.
The live fixture contains the tail-repair/validation fix; final sticky I/O
fencing was separately exercised by the focused AMD/Linux and sanitizer tests.

[Retained acceptance evidence](../../results/measure/20261008-w93-w96-implementation/w96-recovery/live.log).
KV preallocation and its performance/recovery gates remain separate work.

## Remaining packages

- W93: bounded cross-request follower batching, with ACK/apply/read visibility
  gated by persistence; measurement and faults before acceptance.
- W94: atomic CREATE/open lease, safe adoption and all namespace paths/retries.
- W95: extend existing independent storage experiments for true tiny logical
  payloads, durable completion latency and mixed churn; keep production separate.
- W96: bounded KV preallocation, recovery/reset/compaction/fault gates and matched
  performance measurements after the recovery prerequisite.

## W96 bounded preallocation

Linux durable WALs reserve EOF in bounded 1 MiB increments and write at the
logical valid EOF with pwrite. NOSYNC and other platforms retain exact growth.
Allocation failures disable the optimization and fall back to ordinary writes;
partial writes or failed barriers still fence the instance. Replay removes
reserved padding; reset clears physical and logical lengths together.

Focused AMD KV/Raft-store and fault tests pass, including unsupported allocation,
ENOSPC fallback, multiple reservation boundaries, reset/reopen, partial-write and
sync faults. Local ASan/UBSan passes. Private AMD `gc-direct-i0zfg2cp` passes
POSIX 216/0/1, peer 64/64, zero-tail repair and a second crash after new fsynced
files, plus final empty physical inventory. Installed service restored.

Three matched 1-KiB durable-append pairs: QD1 baseline 4016–4163 ops/s vs
preallocated 6182–6941; QD8 baseline 16727–17161 vs 17816–18046. This supports
keeping the bounded optimization on AMD, not claiming a NUC/two-device result.
[Measurements and fixture evidence](../../results/measure/20261008-w93-w96-implementation/w96-preallocation/README.md).

## W93 bounded follower request batching

The host retains inbox order and sends at most 16 contiguous same-group messages
and 1 MiB to the core. The core only defers stable-term, same-leader, strictly
extending data appends. Duplicates, conflicts/truncations, configuration changes,
reads and term changes flush first. No additional batching delay. Replies keep
the original term/read context; apply and ACK wait for the durable barrier.
A failed batch fences the instance against later receive/tick until restart.

Deterministic tests prove barrier reduction (three requests / one barrier),
ACK/apply ordering, byte/count bounds, append/sync faults, duplicate and term
boundaries. Real disk tests crash with another group's shared hold outstanding:
all acknowledged cold bytes survive, the other hold is not consumed. AMD
Raft/core/store tests and local ASan/UBSan pass. Full private AMD fixture
`gc-direct-qu6dh4p6` passes POSIX 216/0/1, peer 64/64, append/sync/replay/dirty
stop/shutdown, GC and deliberate WAL repair/second crash. Installed service restored.

Controlled queued-request cost: serial 6382–6579 durable entries/s versus
49982–50360 batched; p99 readiness-to-ACK 2555–2715 versus 366–416 us.
These are per-group protocol measurements, not filesystem throughput claims.
Mixed-group sustained and idle-latency measurements remain before W93 closure.
[Evidence](../../results/measure/20261008-w93-w96-implementation/w93-batching/README.md).

## W94 atomic server CREATE/lease

Ordinary CREATE now puts the initial lease into the same atomic KV batch as
namespace, inode, allocator and replay verdict. It no longer ignores a separate
lease-write failure. First-use cross-group hashed CREATE prepares the lease in
the namespace transaction, eliminating its post-COMMIT lease proposal and the
replay hole. Old replay does not recreate a lease that was already closed.

AMD metadata/session tests pass. Injected combined-batch failure leaves no name
or successful replay verdict. Private live `gc-direct-k1krefwt` creates held
files on ordinary and first-use cross-group hashed paths, crashes all four
daemons before adoption, retries the identical CREATE identity, unlinks while
held and verifies nlink-zero inode lifetime until final close and removal.
Full POSIX 216/0/1, peer 64/64 and GC/cold recovery pass; service restored.
[Evidence](../../results/measure/20261008-w93-w96-implementation/w94-atomic-server/live.log).

W94 remains open for client adoption ownership and ambiguous-result cleanup.
FUSE still uses separate HOLD; enabling the old flag now would retain the
last-close-before-adoption race. Production legacy mount sessions also lack
durable revocation integration, so new orphanable CREATE claims must not be
introduced as a shortcut.

## W95 independent tiny-file principles

The existing independent benchmark now accepts 1-byte through 1-MiB physical
payloads and separately models tiny logical content inside full fragments.
Pattern generation is safe below eight bytes; direct/punch records stay aligned.
Packed buffered containers and conditional tombstones are explicit experiments;
tombstoned allocation is retained until actual reclamation, never reported freed.
Directory sync topology can include every ancestor to model current production
cost. Fixture-directory entries are persisted outside timing before operations.

Every modified record's latency reaches its cohort's successful durability
barrier; earlier records no longer omit their batch wait. Results/reports expose
logical/physical geometry, topology, p50/p95/p99 and latency semantics. Existing
profile harness generates valid tiny/packed cases and rejects invalid direct
packing/punch combinations. Nine benchmark contract tests, 20 profile tests,
24 geometry/persistence cases, 12 packed read/write/delete cases and AMD
ASan/UBSan pass. Eight separate baseline/perf/strace runs produced all three
requested perf report types. Sampled hot paths are mainly filesystem/locking;
CPU sample percentages are not wall-time attribution. No production format port.

[Evidence and remaining gates](../../results/measure/20261008-w93-w96-implementation/w95-principles/README.md).
Mixed churn and durable index/body/ACK/GC crash-phase recovery remain before
W95 closure. Packing is not an approved backend migration.

## W96 NUC two-device performance follow-up

Three matched pairs on each NUC device improve single-writer rates, but QD8
results are mixed (including slower candidate pairs). Balanced A/B/B/A repeats
and separate syscall-count profiles were collected. Unrelated NUC daemons were
busy during measurement and were preserved. This is not clean universal
performance acceptance; the remaining gate is an isolated repeat, with no
claim that reservation always wins. [All measurements and limits](../../results/measure/20261008-w93-w96-implementation/w96-nuc/README.md).

## W93 follower failure and committed-prefix protection

Expanded tests reproduced application of an unmatched follower tail. Commit now
stops at the prefix proven by the request, remains monotonic, and rejects any
conflict that would truncate committed entries. Hard-state, truncate, append
and batch durability failures fence the instance until restart; no success
reply or apply escapes those failures. Focused AMD tests and local sanitizers
pass. Full private AMD direct/TCP fixture `gc-direct-aw0f7val` passes POSIX
216/0/1, peer 64/64, append/replay/fsync, held CREATE, shutdown, two cold WAL
recovery cycles and empty final physical GC inventory. Installed service restored.
[Evidence](../../results/measure/20261008-w93-w96-implementation/w93-fault-recovery/live.log).

## W94 rejected descriptor replies

CREATE reply-construction failure and rejected CREATE/OPEN descriptor replies
now release the acquired local handle/open reference. The namespace effect is
retained. Kernel rejection cannot be followed by RELEASE, so leaving those
references behind leaked their server leases. Production callback fault tests
pass on Mac and AMD. A live LD_PRELOAD shim completes each request without a
descriptor, returns the rejected-reply status, then verifies unlink reclaims
the server inode. Full private AMD `gc-direct-czn4utnr` passes POSIX 216/0/1,
peer 64/64, replay/fsync/shutdown and repeated cold WAL/GC gates. Service restored.
The full Linux regression suite also passes in an isolated generated-doc tree.
[Evidence](../../results/measure/20261008-w93-w96-implementation/w94-rejected-replies/live.log).

This fixes a lifecycle leak; it does not enable CREATE lease piggybacking.
Client adoption and ambiguous CREATE/lease-close recovery remain open.

## W93 durable ACK ordering and W95 production churn

Follower cross-request ACKs now follow successful log durability without waiting
for the whole cohort's state-machine/KV apply. Inbox processing still completes
apply before returning. A real-disk child exits at the last ACK before any apply;
all acknowledged records and the other group's held bytes survive cold reopen.
Focused Raft/disk tests, local ASan/UBSan and the full Linux suite pass.
Full AMD direct/TCP `gc-direct-_moyfi4u` passes POSIX 216/0/1, peer 64/64,
rejected descriptor replies, append/replay/fsync/shutdown and repeated WAL/GC
cold recovery. Installed service restored. Mixed-load performance acceptance
for this ordering is being measured separately.

W95's new production churn gate fsyncs 300 create/overwrite/delete/recreate
generations across 100 files, payloads 1/17/128/512/65536 bytes. It verifies
every final generation after abrupt private-daemon restart, then proves
physical deletion and queue drain. This is functional correctness evidence,
not an independent packed-index crash test or a power-loss claim.
[Evidence](../../results/measure/20261008-w93-w96-implementation/w95-live-churn/live.log).

## W93 / W53 end-to-end mixed-load finding

Three baseline and candidate mixed-load samples pass exact cold bytes and
physical reclamation. The two balanced uncontaminated pairs show baseline
tiny-close p99 237–249 ms versus candidate 266–282 ms; this tail gate fails
acceptance despite lower median latency in some samples. A four-request bound
experiment did not establish a fix, so no default-bound change was made.
[All samples and contamination limits](../../results/measure/20261008-w93-w96-implementation/w93-mixed-load/README.md).
The subsequent durable-ACK-before-apply change is being measured separately.

## W95 independent cold-process verification

The principles benchmark can now retain successful durable write fixtures and
verify them read-only from another process with exact geometry seals. Default
empty-root cleanup is unchanged. Failed recovery preserves evidence; corrupt,
truncated, missing and mismatched records cannot produce BENCH_OK. Seal sync
failure reports failure and cleans only the owned incomplete fixture. Ten
contract tests and twelve two-root sanitizer cases pass.
[Evidence and limits](../../results/measure/20261008-w93-w96-implementation/w95-cold-principles/README.md).

## W94 remaining activation prerequisite

The per-inode edge mutex orders local RPC calls, but does not make an ambiguous
HOLD safe: a timed-out OPEN can commit after cleanup CLOSE, and a timed-out
CLOSE can arrive after a later OPEN. CREATE adoption must not assume those
legacy edges are sequenced. The safe integration needs guarded lease-operation
identity and pending-CREATE ownership, with bounded replay/tombstone retirement
through W72's durable runtime session fence. A new pending claim alone does not
solve ambiguous edges or client death; a second independent lease per CREATE
would violate the one-lease/session invariant and add orphanable state.

Server atomicity and rejected-reply cleanup are implemented, but production
piggyback activation stays blocked on that protocol integration and its lost-
reply/adoption/last-close/fencing gates. No global open/close lock, age-based
claim deletion or new unbounded claim table is introduced to declare W94 done.

## W96 detectable interior damage

Recovery now refuses to truncate when nonzero records follow a damaged frame,
including the historical invalid-header/acknowledged-append sequence. It
preserves evidence and returns PROTO; ordinary torn/zero reservation tails
still repair. AMD KV/compaction, Raft/disk tests and Linux/Mac sanitizers pass.
This is not a general corruption salvage guarantee. The final guard's full
live cold rerun is pending after isolated timing.
[Evidence](../../results/measure/20261008-w93-w96-implementation/w96-interior-damage/README.md).

## W95 independent process-crash phases

Twelve independent file/container cases now crash after body write/sync,
fixture-seal sync/parent publication, printed acknowledgement and GC unlink.
Incomplete unsealed fixtures are rejected read-only; complete sealed data
survives cold verification, and every printed success is recoverable. Evidence
is preserved after failed recovery. All eleven contract tests pass.
[Evidence and model limits](../../results/measure/20261008-w93-w96-implementation/w95-crash-phases/README.md).
This covers the existing fixed-geometry principles model, not a production
packed-index protocol or a selected/migrated backend.

## W94 staged combined ownership primitive

A versioned pending-CREATE token can now share the existing FileID/session lease
key with normal OPEN ownership. Normal CLOSE preserves pending ownership; exact-
token cleanup preserves a separate normal owner, and stale tokens cannot delete
a later pending claim. Malformed ownership is rejected rather than overwritten.
AMD `test_session` and `test_meta_apply` pass, including unlink/reclaim protection,
one-key accounting, stale cleanup and session removal. This primitive is not
used by production CREATE or enabled on the wire. Guarded ambiguous HOLD edges,
atomic token composition/adoption and durable runtime-session recovery remain
activation requirements.

## Longer W93 gate and W94 cold ownership

Six isolated 4,096-file mixed-load fixtures passed exact cold verification and
physical deletion/reclamation on AMD direct/TCP. Three balanced pairs compare
the same current code with receive coalescing enabled versus limited to one.
Mean close-p99 is 309.124 ms versus 311.454 ms; overlapping samples show no
consistent tail regression, not a universal speedup. Explicit fixture quotas
and bounded file counts make the longer test reproducible. The earlier 1 GiB
quota-pressure failure is retained separately, with healthy eventual GC drain.
[Measurements](../../results/measure/20261008-w93-w96-implementation/w93-mixed-long/README.md).

W94 now has four real process-exit/cold-reopen ownership-edge tests. A fresh
AMD Linux build and complete `make test` pass with these tests and the final
W96 interior-damage guard. Production pending ownership remains inactive.

## Final W96 guard live gate

Fresh Linux build/full test and AMD direct/TCP live fixture
`gc-direct-bk9aqz_4` pass with the final WAL guard. POSIX/peer, rejected replies,
CREATE lease replay, tiny churn, append/replay/sync/shutdown and second-cold WAL
tail recovery pass; final physical inventory/quotas are zero and late PUT is
fenced. Installed service was restored. W94 ownership code/tests additionally
pass a focused ASan/UBSan executable (normal linked KV library).
[Full gate evidence](../../results/measure/20261008-w93-w96-implementation/final-live/README.md).
The quiet two-device W96 performance gate remains open: NUC currently runs a
million-file generator and its daemons are busy; that workload was preserved.

## W94 pending ownership in the atomic CREATE batch

The staged pending helper derives its token/session from the CREATE identity
and writes ownership with namespace/inode/replay state in one batch. Pending
mutation refuses exhausted replay windows, invalid/pre-acknowledged identities
and retired verdicts that cannot identify an inode. Injected failure/retry,
late replay and cold atomic CREATE/ownership tests pass. The exact final Linux
full suite passes; session/metadata sanitizer coverage passes. Shared-helper
legacy integration additionally passes full direct/TCP live POSIX/peer and cold
recovery in `gc-direct-6tmcbm26`, before the final pending-only guards.
[Evidence](../../results/measure/20261008-w93-w96-implementation/w94-atomic-pending/README.md).
No production caller activates pending ownership. Cross-group pending composition,
sequenced HOLD edges, client adoption and runtime-session cleanup remain open.

## W94 bounded sequenced-edge state machine

Inactive guarded edges now enforce per-file sequence ordering and exact request
payloads, with read-only durable-verdict probes. One lease key retains pending/
normal ownership or a closed replay record. A separate session/shard window
holds at most 16 verdicts (973 encoded bytes); contiguous ACK retirement, effect
and verdict share one KV batch. Old CLOSE cannot drop a newer OPEN, and closed
records do not block inode reclamation. Floors retire only after the global
barrier and local rejection floor, with old ownership removed first.

All 24 arrival orders, pressure/fault/malformed/epoch tests and cold edge/floor
recovery pass. Exact full Linux tests and focused sanitizers pass. Shared scan
integration passes full live `gc-direct-ofxe2t00` with service restored; the final
read-only probe/negative checks are additionally covered by the exact unit suite.
[Evidence and activation limits](../../results/measure/20261008-w93-w96-implementation/w94-guarded-edges/README.md).
Wire/client handoff, unknown-CREATE recovery and production sessions remain;
these server primitives do not activate piggyback ownership or D25.


## W94 guarded wire and replay cleanup follow-up

`95ac399e` exposes sequenced lease effects and read-only result queries through
existing session commands. Requests carry the exact FileID, session epoch,
sequence, contiguous acknowledgement and pending token; outcome queries cross
ReadIndex and never acquire ownership. Required last-close cleanup errors hold
apply rather than being converted to a successful verdict. FUSE retains its
legacy HOLD path: this is server integration, not piggyback activation.

`1cecf502` durably records pending CREATE EEXIST results without namespace or
lease effects. A delayed retry cannot create abandoned pending ownership after
the conflicting name is removed. Injected batch failure, replay pressure and
real process-exit/cold-reopen coverage (`8a919c5a`) pass.

`e28c1244` retires redundant guarded verdict windows after the local rejection
floor is durable. The rejection floor remains; active ownership waits for the
completed global barrier and LEASE_DROP. Unit and cold tests prove those distinct
conditions, including lane-local behavior without consulting a missing owner row.

AMD direct/TCP fixtures `gc-direct-gfy2f_ho` and `gc-direct-mma_iq6m` pass
lost-reply probing without resubmitting the successful OPEN, reversed OPEN/CLOSE,
cold replay, final close and physical reclamation. The second adds an unknown
probe that acquires no ownership and durable fencing of an active unlinked lease.
Both full runs also pass POSIX 216/217 (0 fail, 1 skip), peer 64/64,
append/synchronous-write/replay/shutdown/dirty-stop recovery, two cold WAL-repair
restarts and physical inventory/quota zero. Installed AMD service was restored.

The third repeat (`gc-direct-ejr5ca22`) exposed explicit NOT_PRIMARY without a
leader hint during cross-group ESTABLISH after cold rejoin. It failed closed and
restored service. `7a56625d` adds bounded retries only for that explicitly
non-applied response; ambiguous transport failures still return immediately.
Deterministic no-hint/exhaustion/transport tests and full Linux regressions pass.
Further exact-code live and CREATE apply-failure repeats are in progress.

[Wire evidence](../../results/measure/20261008-w93-w96-review/w94-guarded-wire/).

Remaining W94 activation dependencies are real runtime mount sessions, atomic
pending ownership on every namespace transaction path, bounded retention and
resolution of ambiguous client outcomes, and safe local descriptor adoption.
Do not enable the CREATE flag merely because server primitive gates pass.
W95 indexed packing semantics/experiments and W96 quiet NUC acceptance remain
open; NUC still runs unrelated tiny-file and storage-engine experiments.


The corrected cross-group admission/window-retirement repeat
`gc-direct-1vk60ch4` passed the complete live gate and restored service.
`2e890738` fixes two storage-failure behaviors found while strengthening W94:
CREATE IO/NOMEM now holds the Raft apply position, and a follower retries already
committed but unapplied entries on unchanged valid heartbeats. A private
IO-then-NOMEM injection reproduced EBUSY after cold restart in
`gc-direct-mgwf9eza`; the follower stalled at applied=31/commit=32 because no
later write advanced commit. The corrected repeat `gc-direct-5dywwrr_` has passed
POSIX/peer and ownership gates; final physical/WAL checks are still running.
Both ordinary and batched deterministic retry tests, the full Linux test target,
Mac Raft ASan/UBSan and metadata/session ASan/UBSan passed. The metadata/session
sanitizer test links the normal KV library; this is not full-library instrumentation.

Guarded retirement now propagates missing authoritative session evidence instead
of confusing that with an already-absent replay-window key. Unit/sanitizer tests
prove active ownership and its recorded verdict survive that failure. The
production cross-group retirement coordinator remains a separate integration
requirement; a missing owner row never constitutes revocation proof.


The CREATE failure repeat `gc-direct-5dywwrr_` completed successfully: actual
IO/NOMEM phase markers, POSIX 216/217 (0 fail/1 skip), peer 64/64, cold WAL
acknowledged-byte recovery, physical inventory/quota zero and installed-service
restoration. All 120 five-edge pending-adoption/normal OPEN/CLOSE/cleanup arrival
orders now pass, including exact replay verdicts and safe fresh cleanup of a
stale pending owner after every earlier outcome is resolved. ASan/UBSan passes.
This verifies server ownership semantics; production client adoption remains
inactive pending its session and bounded ambiguous-outcome integration.
