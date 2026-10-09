# Staged replay-identified close successor — Oct 9

SESSION sub16/17 adds a distinct deferred edge/probe identity domain. Successful
close commits lease state, bounded replay result/floor and a CLOSE_STEP34 marker
atomically. Its 36-byte value is generation, UUID, epoch and sequence. Replaying
an acknowledged or retired old close cannot recreate a removed obligation or
remove a later OPEN. The legacy wire/apply semantics remain unchanged.

CLOSE_STEP command36 applies capped lock cleanup or exact obligation retirement.
Retirement refuses while leases, locks, cursor or reservation rows remain.
BUSY is a business verdict; IO/NOMEM retain log-index retry. The worker selects
at most eight obligations per pass, completes one append transaction or prunes
at most eight DONE rows for an obligation. Exact marker PREPARE precedes append
images; leases are rechecked after PREPARE, and deferred OPEN honors that owner.
The append image collector now has a hard 69-image allocation cap.

RPC-only CLOSE_APPEND_NEXT37 delegates to a node hosting both metadata groups.
Its handler never recursively forwards: a dual-host follower obtains fresh
ReadIndex on both groups before coordinating exact images. Staged pressure
accounting charges finite row expansion and distinguishes OPEN from recovery.
This does not certify a finite scan-CPU or whole-call deadline: existing ordered
frontier scans may visit long DONE prefixes; the legacy apply drain is unchanged.

Final source/binary candidate passes full Linux regressions and targeted replay,
prepared-owner, failed-batch, reopening, retirement and admission/verdict tests.
Private AMD direct/TCP fixture gc-direct-9yl816j0, port29140, pressure off:
- Actual crash leaves 64 OPEN reservations and one obligation on all three inode
  replicas. Cold recovery uses node1 as group0 LEADER, hosting no group2.
- Recovered EOF9437184: original 1MiB exact bytes plus 8MiB zero holes.
- Stopped-store inspection: no reservations, cursor, locks or close obligations.
- Lost-reply probe, reopen and old retired-close refusal pass.
- Core POSIX212/212, peer64/64, append/fold GC, offline-member ledger retention,
  rejoin and final zero physical inventory/quota/late-PUT fencing pass.

Daemon SHA2564865dd7ab40c78d27a82efd1ac242179cd0a21a5f9c8edfafa268a39791dd543;
client18d6d300bd697be29e9ff9b10cbfb7398a7da8b64be920bc636083dcc9d6b665.
Installed978caaf7 restored; both mounts serving, reservation released.
The earlier fixture gc-direct-ev3ral8p motivated the routing correction and is
superseded by this fixed-candidate run; do not use it as single-host proof.

Production FUSE still sends legacy HOLD. It does not acquire the deferred edge
identity or honor its prepared marker: this successor is staged, not activated.
The parent package remains open for production lease identity/ambiguity cleanup,
session append revocation and receipt retirement, then session/PUT lifecycle
integration. No pressure or D25 activation, mixed-version/downgrade or hardware
power-loss certification follows from these gates.
