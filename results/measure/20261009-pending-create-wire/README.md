# Opt-in pending CREATE wire and cold adoption — AMD October 9

Baseline77d9b7b0. New explicit CREATE_F_PENDING wire flag requires a valid
immutable op-id, established local shard session and authoritative active global
session. The replicated pending form carries full UUID/epoch in its lease slot,
uses the CREATE sequence as pending adoption token, and atomically writes
namespace/inode/replay result/pending lease through the existing metadata batch.
Historical CREATE command interpretation retains its legacy lease epoch1.
Malformed pending frames/UUID/epoch/flag combinations fail before namespace
apply. Global/local admission runs before cached CREATE replay as well.

First-use cross-group hashed lane creation refuses BUSY before its namespace
transaction; its pending lease participant format has not been implemented.
Current FUSE CREATE does not send the new flag. No activation or mixed-version
certification is claimed.

Strict production encoder/decoder test covers complete16-byte UUID, epoch7,
exact token/ino/layout, every truncated pending frame, unknown/missing flags,
identity/epoch mismatch, missing identity, combined legacy hold and unchanged
historical lease CREATE interpretation. AMD all code regression recipes pass.
Final daemon build is warning-free; local docs gate5/5.

AMD direct/TCP private fixture gc-direct-ml_diy3z: pending CREATE returns the
same inode on repeated request, crash/cold WAL replay retains it, exact original
identity adopts/CLOSEs it, and replay after CLOSE cannot reacquire pending lease.
The rejection returns STALE without proof initially; an exact read-only probe
retrieves the stored negative verdict (never confuse that initial reply with
completed ownership). The later inode deletion physically reclaims it. Core
POSIX212/212, peer64/64, CREATE/rejected replies, guarded lease cold replay,
append800 and physical GC/restart/zero quota/late-PUT fencing pass. Both installed
mounts restored; installed stores/binaries unchanged.

Initial helper runs failed by requiring known=1 on the first negative submission
reply. Diagnostics confirmed STALE known0; the corrected helper preserves the
identity and obtains stored proof by query, matching the retained request API.
One initial compiler range warning was removed and final strict/Linux checks
rerun. Those failures are superseded by the final gate, not counted as passes.

Remaining: first cross-group lane pending transaction, caller-owned pending
CREATE identity/adoption across FUSE ambiguity and descriptor rejection,
sequenced FUSE OPEN/CLOSE wiring, namespace/REPORT fencing, production PUT
manifest admission/transport/durable storage fences/all-member collection and
retirement. No D25/pressure activation, push/tag, power-loss or RDMA claim.
