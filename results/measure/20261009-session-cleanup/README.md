# Bounded session lease/lock cleanup

Private AMD direct/TCP, staged pressure enabled only in fixture. Installed
978caaf7 restored with pressure off. No production session activation.

Full regression and final unit/host decoder/admission/verdict tests pass.
Actual sync-WAL crash/reopen fixture passes (13 old leases, exact first-eight
replay, remaining five, retained close marker, other owner/new epoch preserved).

Live fixture gc-direct-igaguvk4: bounded lease/lock RPC, global revocation gate,
first-eight replay, five owners retained across crash, and final physical
reclamation PASS. Core POSIX 212/212. Peer 63/64: peer_append_while_truncate EIO.
The full candidate is NOT accepted by this first run. Failed client/server
logs and stores retained on AMD. Investigation points to client read retry
ignoring authoritative GETCHUNKS absence while retaining old staging mapping.
