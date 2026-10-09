# Legacy/sequenced ownership handoff — AMD October 9

Baseline f9a3deaa. Legacy HOLD previously classified valid sequenced lease
records as protocol errors. Its reader now validates versions 1, 2 and 3.
Legacy OPEN can observe an already active sequenced normal owner without
changing its sequence or pending token. Legacy CLOSE, pending cleanup, and
OPEN of a sequenced closed/pending-only record return STALE; they cannot
remove or downgrade a sequenced record. Exact sequenced edges remain required
for ownership changes. Historical version-2 writes retain their 10-byte format.
This does not claim the previous fail-closed parser allowed unsafe releases.

Session regressions check byte-for-byte preservation, closed-record protection,
matching-token cleanup refusal, exact sequenced release, every version-3
truncation and zero-sequence rejection. Strict additional pending-token tests
pass. The old refusal regression was updated from PROTO to deliberate STALE.

AMD direct-I/O/TCP fixture gc-direct-etuzhfmr passes actual client-wrapper
OPEN/CLOSE/probe and the legacy mutation barrier, cold replay, local/cross-group
pending CREATE/adoption, core POSIX212/212, peer64/64, exact800 cold append
records, physical GC/restart, zero quota and late-PUT fencing. Full code
regression recipes pass, architecture excluded remotely and validated locally
5/5. Build warning-free. Both installed mounts independently verified restored;
reservation released. No installed stores or binaries changed.

Remaining: retain FUSE CREATE request/result across ambiguity, sequenced pending
adoption and OPEN/CLOSE lifecycle, namespace/REPORT fencing, production PUT
transport/storage fences/collection/retirement. FUSE still uses legacy HOLD;
D25 remains staged. No mixed-version activation, hardware power-loss or
distributed RDMA certification.
