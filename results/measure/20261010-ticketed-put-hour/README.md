# Ticketed PUT and exact FINISH checkpoint — Oct 10 UTC

Branch: `devel-w66`. One-hour continuation started 05:44:32 UTC. No activation,
push or forced write loss. AMD: one host, four daemons, direct I/O/TCP; private
stores and borrowed mounts, installed service restored after each fixture.

## Exact FINISH business verdict

`bb48c24a` adds FINISH to the committed business-result apply policy. Incomplete
BUSY/STALE must reach callers and advance apply; IO/NOMEM must remain retryable.
Before this fix, `namespace-barrier-l2to3opx` left group 2 applied=6/commit=7 after
incomplete FINISH and subsequent ReadIndex hung. The focused fixed fixture
`namespace-barrier-cgbd_4pq` passes the exact frozen touched-set gate; see
[raw isolated output](namespace-isolated.log). The four-daemon full fixture
`gc-direct-2oo6ou8k` passes core 212/212 and peer 64/64, the namespace gate,
certificate-sync fault recovery, all-member cold rejection floors and physical
GC/restart; see [full output](namespace-full-live.log).

## Staged ticketed data-plane receiver

New opcodes 129/130 use a fixed-endian immutable ticket header plus exact
fragment bytes. The receiver validates destination, checksum, ADMITTED authority,
fresh FileID and configured geometry, then takes the durable local session guard
around storage. Authority RPCs precede the guard to avoid seal/apply deadlock;
a revocation completing meanwhile is rejected by the local durable floor.
Ticket traffic cannot change export geometry. This staged receiver supports
namespace export 1 and never supplies the legacy NEW hint on ambiguous retries.

The retained client RPC caps its deadline at 15 seconds or the caller's earlier
deadline, probes fresh ticket/membership state, sends the exact original body,
and retains ownership across lost replies. No automatic downgrade to legacy PUT.

Native server/client tests cover exact framing, checksum/FileID/authority
rejection, configured geometry mismatch, IO retention, retry, revocation between
authority and storage, inherited deadlines and lost responses. Targeted ASan and
UBSan runs instrument these new modules, not the full linked library.

Full candidate `gc-direct-ii4qefwm` passes core 212/212, peer 64/64 and existing
cold/GC gates. Actual staged publication tests now use ticketed uploads,
corrupt-checksum rejection, a discarded successful upload result and exact retry,
then rejection after PUBLISHED/retirement. Separate unpublished tickets upload
three real fragments; cold GET checks their bytes before any retry, then completed
revocation rejects actual late PUTs on all three destinations. Final geometry
candidate and merged-code results will be recorded below.

## Remaining dependencies

Default FUSE typed flush remains staged. Durable captured per-ticket server member
identities, all-member abandoned-upload deletion acknowledgements/collection,
revoked unknown-intent reconciliation and active legacy W88 readiness remain
open. Current roster validation is not durable dynamic-membership proof. These
gates do not establish hardware power-loss durability or distributed hardware RDMA.

## Integrated follow-up gates

`2fed12ef` merges the persistence branch's append receipt/EOF race and recovery
fixture fixes. `2526d4c1` merges bounded query cache code; `67fc6a2f` adds its final
documentation/evidence. `e8c6178d` makes partition fixture mode explicit and
records it; its direct/TCP rerun passes. All inspected local tips are included.

Final receiver fixture `gc-direct-mehuigdc` passes core212/212, peer64/64 and cold
physical GC. Integrated fixture `gc-direct-k7dik9wu` independently repeats those
counts, exact ticketed cold bytes, publication/retirement rejection, certificate
fault/recovery, namespace completion and physical deletion. See [merged live
output](merged/live.log), [regressions](merged/final-make-test.log) and [direct
partition output](merged/partition-direct.log). Both restored installed service.
Selected local/AMD source hashes match; the private build still reports unknown
Git identity. New-module ASan/UBSan reruns pass after the geometry/deadline tests.

WAL fault fixture `gc-direct-s24q2nrm` proves periodic and shutdown checkpoints
remain unchanged after a real latched follower KV fdatasync failure, then cold
Raft replay recovers exact original/link/follow-up bytes and nlink=2 before
physical deletion. [Raw log](merged/wal-fault.log).

Fixture `gc-direct-rgtf3fdp` passes cross-group append/fsync peer-before-close/cold
bytes, 32 identical APPEND requests with one allocation, ACK retirement, retained
partial-body recovery after failed stop and acknowledged fsynced bytes across
leader shutdown. [Raw log](merged/append-drain.log).

The additional session-cleanup gate initially failed in `gc-direct-x0braao2`:
its helper assumed one manually registered inode shard represented the complete
frozen touched set. Lease admission registers additional guarded participants.
The committed FINISH BUSY now exposed that incomplete barrier instead of hiding
it. [Retained failure](merged/session-cleanup-before.log). The updated helper
requires incomplete BUSY, reads and ACKs every frozen bit, verifies the storage
epoch and retries exact FINISH; it never relaxes the production barrier. Corrected
fixture `gc-direct-d_48g00a` passes exact first-eight replay, remaining owners
across cold restart, management resume, zero locks/markers, physical reclamation,
and deferred CLOSE with 64 cross-group reservations, cold holes, retired
obligations and exact reopened bytes. [Raw output](merged/session-close-fixed.log).
The revoked-append helper had the same single-shard assumption and now uses the
exact frozen set and storage epoch too; its corrected `gc-direct-0gb77sxi` run passes exact 32+32 transactions, cold
original bytes/holes, rejected late appends, zero remaining reservations/receipts,
bounded lease release and physical reclaim. The helper holds a real session lease
so unrelated CLOSE cleanup cannot race the exact half-complete snapshot; the
prior open=31 setup failure is retained in [its log](merged/revoked-append-before.log).
This does not weaken the open=32 assertion. Actual O_SYNC/O_DSYNC callback failure
also retains exact bytes through fsync and cold recovery.
[Successful run](merged/revoked-append-fixed.log).

## Final late-arriving integration

`8109d954` integrates the atomic KV apply/cursor changes and logical WAL tear
recovery test from the persistence branch. `12d2580b` includes the latest devel
integration commit points; a new `devel-intel` tip points to that included history.
`32eaa009` contains the corrected retained-lease/exact frozen-shard live helpers.
The final source hashes are separate under `final-integration/` and match the AMD
private checkout. Complete native regressions pass after this code integration;
the final full AMD live run `gc-direct-vs3gw66k` passes core212/212, peer64/64,
typed same/cross cold replay, namespace completion, certificate fault/recovery
and physical GC. [Raw integrated output](final-integration/live.log). Earlier fault/cleanup proofs above must
not be attributed to the late atomic-cursor code. W93's broader exact committed
build and sustained acceptance gates remain open; this private build reports
unknown Git identity. No downgrade, activation or service-store migration occurs.


### Last-tip boundary — 06:42 UTC

The captured `devel`, `devel-intel`, `devel-find-stats` and persistence tips are
all included, with final integration at `200d6aee`. Late arrivals add the isolated
Raft verdict test boundary, retained CREATE BUSY routing and aggregate retained
query-cache generation/candidate bounds. All captured branch tips and matching
local/AMD sources are under `last-tips/`. Complete AMD Linux regressions and the
warning-free build pass. Final direct/TCP fixture `gc-direct-7xbnrwx1` passes core212/212, peer64/64,
server-side deletion counters, zero quota/physical inventory and cold GC/rejection
floors. [Final raw gate](last-tips/live.log). Installed service and both policy
mounts are restored; [restoration proof](last-tips/restored-service.txt). No owned
fixture remains active. Earlier fault/cleanup results remain attached to their
own candidate. No new implementation or test is started after 06:44:32 UTC.
