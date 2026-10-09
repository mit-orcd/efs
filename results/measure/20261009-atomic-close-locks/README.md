# Atomic capped last-close lock cleanup — Oct 9

Lock grants cap each of two domains at 32 rows. Final-close cleanup now validates
that envelope before any effects, honors prepared exclusive owners, and performs
one atomic batch of at most 64 deletes. A late prepared row cannot cause earlier
locks to disappear; a failed storage batch preserves every lock. Invalid or
imported over-cap records fail closed without an unbounded cleanup loop.

Production host defers BUSY from this lock phase into its durable CLOSE_CLEANUP
marker, letting a later RESOLVE advance. IO/NOMEM still hold the failed entry.
Actual host-function test proves marker retention, later resolution and retirement.
The old lock source fails six new assertions; fixed tests pass. Full AMD Linux
regression passes; the subsequently added actual host-function test also passes.

Private AMD direct/TCP fixture gc-direct-x1nxvbaq, port28940, pressure disabled
as in installed production: bounded session global barrier/exact replay/cold
remaining-owner cleanup, guarded lease reorder/lost reply/probe/cold/last-close,
append/fold/GC, core POSIX212/212 and peer64/64 all pass. Final cold physical
inventory/quota are zero and late PUT is fenced. Both installed AMD mounts
restored; installed stores unchanged.

Retained failed fixture gc-direct-nvqc6dgf first passed bounded session cleanup,
then legacy management fence returned BUSY with staged pressure enabled. Its
legacy LEASE_DROP has UINT64_MAX charge by design. Production pressure remains
off; this is not certification of pressure-enabled legacy revocation.

Candidate daemon SHA256 f55334381e775dd0cb720ed1d641a5862acec7a84cae83a455b0252462d05233.
This closes the lock-side loop/atomicity gap, not the whole package. Legacy
append drain, bounded successor/reopen serialization, session append cleanup,
receipt retirement and production lifecycle integration remain required.
