# Retained FUSE CREATE and sequenced ownership — AMD October 9

Baseline fdf053be. FUSE regular CREATE now retains a pending namespace request,
uses separate read-only routing/result proof, and keeps the resolved shard
across ambiguous replies. Namespace receipt proof crosses ReadIndex. Historical
inode reply framing is unchanged (new opt-in message types125/126). Returned
pending FileID/token is adopted through a sequenced edge before returning the
FUSE descriptor. Last close resolves unknown adoption/OPEN before its exact
compensating CLOSE. Abandoned CREATE cleanup clears its token and never unlinks
a possibly renamed name. A128-entry queue bounds that bookkeeping; clean stop
refuses pending CREATE or lease work. A background worker retries oldest lease
ownership and abandoned CREATE with a1-second RPC budget per pass.

The shared sequencer now bounds unacknowledged sequence positions to the
16-entry receipt cache, preserving capacity for the oldest unresolved operation.
Later completed replies cannot fill every slot and prevent that oldest operation
from committing. The physical tracker remains512 slots; this is stricter flow
admission. Bounded waiting never issues an unidentified mutation. ACK can advance
below the oldest unresolved slot; UUID/epoch/sequence/FileID/body/token stay fixed.
ACK refresh applies to CREATE, unlink, link, rename and retained lease/CREATE.

AMD direct-I/O/TCP fixture gc-direct-hbynxoog passes212/212 core POSIX (22.0s),
64/64 peer (15.5s), rejected FUSE CREATE/OPEN replies, pending local/cross-group
cold replay, retained CREATE discarded-reply recovery and durable negative
receipt, actual lease transport,800 exact cold append records, physical GC and
restart zero-quota/late-PUT fencing. Both installed mounts independently restored;
reservation released. Build warning-free. Local docs5/5. Full code regression
prefix passed until the former512-owner admission expectation; that test now
verifies16 positions, withholding admission even when a non-oldest slot frees.
It passes. The remaining recipes pass (including a repeated prefix); logs preserve
the superseded assertion. Strict local adoption, queue and stop tests also pass.

Earlier candidates failed concurrent POSIX with BUSY; server diagnostics proved
capacity count16 prevented the oldest CLOSE from committing. The final admission
fix resolves those failures. One helper ownership setting and prepared-marker
fixture assumption were corrected; those initial failed runs are not gate passes.
Append reservations have their own replay namespace: closing a sequenced lease
alone does not forbid an append reservation. No such new restriction was added.

Remaining: explicit terminal recovery from completed global revocation,
namespace/REPORT-wide fencing, production PUT-ticket admission/transport,
durable storage fences and all-member collection/retirement. D25/pressure remain
staged. No hardware power-loss, distributed RDMA or mixed-version certification.
