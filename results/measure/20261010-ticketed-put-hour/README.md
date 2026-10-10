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
