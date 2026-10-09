# Full mount UUID for append, HOLD and locks — AMD October 9

Baseline5c7a3332. Production append no longer replaces the full namespace UUID
with native-order token bytes. HOLD and lock RPCs carry the existing explicit
session suffix (full UUID/epoch); their64-bit owners still distinguish lock
open descriptions, not session UUIDs. Grants establish their session/shard
before RPC; CLOSE, unlock and GETLK do not register. Existing raw legacy wire
formats and historical replicated commands keep their server semantics.
Namespace operation IDs and publication IDs already use this same seeded UUID.
REPORT completes exact inode/range append reservations, not the removed token
identity. This is identity alignment, not ticketed/fenced REPORT activation.

Full16-byte/epoch wire tests prove grants fail before mutation on revoked
admission, FCNTL unlock and GETLK do not register, and ranges/lock owner are
preserved.800,000 concurrent append IDs keep full UUID and shared contiguous ACK.
All AMD code regression recipes pass; compiler has no warnings. Local docs5/5.
AMD direct/TCP private fixture gc-direct-k9zcaap5 passes core212/212, peer64/64,
CREATE/rejected replies, guarded cold lease replay, registered HOLD/FLOCK STALE
after durable revocation, exact800 cold append records and physical GC/restart/
zero quota/late-PUT fences. Both installed mounts restored; stores untouched.

The live client fixture's revoked stage now deliberately seeds its mount UUID
to the revoked identity; it validates registered full-UUID wrappers. It no
longer labels those wrappers raw legacy grants. Legacy metadata-floor unit and
hot cached raw legacy append tests remain separate.

Automatic review initially refused live execution after confusing mocked
force-discard unit output with the runner. Inspection showed clean stops only;
the approved retry completed with no forced installed write loss.

Remaining: atomic pending CREATE with versioned full UUID/epoch and its adoption
or compensating release; wire retained sequenced requests into FUSE descriptor
lifecycle; namespace/publication revocation authority on every mutation; PUT
admission/manifest transport, durable storage fences, all-member collection and
retirement. No full package/session/D25/pressure activation or mixed-version
certification, push, tag or power-loss/RDMA claim.
