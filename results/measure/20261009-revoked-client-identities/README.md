# Authoritative terminal recovery after durable revocation

A retained CREATE/lease request can stop retrying only after a stored verdict or
an authoritative global session ReadIndex proves ACTIVE at a newer epoch.
FENCING, missing owner data, malformed state and transport failure are insufficient.
The completed global barrier has fenced every registered shard; the client does
not invent a new identity or automatically revoke its mount. FUSE release and
pending-token cleanup accept this separate terminal proof and release ACK slots.

AMD private fixture `gc-direct-g4su3w6s`, direct I/O/TCP, four daemons on one host:
actual client transport proves current epoch remains live, completed revocation
is terminal, future epochs and missing UUIDs do not prove revocation, and retained
OPEN resets safely after a completed fence. Core212/212, peer64/64, lost CREATE
reply, adoption/CLOSE/cold replay, append800 and server GC deletion counters pass.
Extracted production client tests additionally exercise retained CREATE terminal
recovery, unfinished fences and malformed replies. No hardware power-loss proof.

Captured live log and lease proof are stored beside this file. Production PUT
transport/storage tickets and broader namespace/REPORT fencing remain open;
D25 stays staged. Installed AMD service restored: both mounts verified and reservation released.
All code regression recipes pass across initial plus corrected continuation.
The first full run exposed the extracted append test missing `<unistd.h>` and
its obsolete512-position expectation; corrected to the enforced16-position
window. Its800000 concurrent unique identities and contiguous ACK gate passes.
Local architecture documentation gate passes5/5. Build has no warnings.
