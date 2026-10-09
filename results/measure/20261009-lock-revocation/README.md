# Legacy lock rejection floors and audible apply verdicts

AMD private direct-I/O/TCP pressure-enabled fixture gc-direct-2_cgyb1f.
Actual client deferred OPEN/CLOSE/probe and cold exact CLOSE replay pass.
Management durably revokes that same identity; actual legacy HOLD and FLOCK
then both return STALE. The file remains live during rejection, so absent inode
state cannot explain success. File deletion subsequently reclaims its fragments.
The test uses the explicitly chosen legacy big-endian owner UUID; it does not
claim mount-wide FUSE session identity integration.

Full Linux, core POSIX212/212, peer64/64, session/append/deferred cleanup and
physical GC counters/restart/inventory/quota/late-PUT gates pass. Final lock
unit additionally injects grant batch IO, proves no effect and exact retry.
Previous lock implementation fails four new floor assertions (before-fix.log).
Actual apply decoder + Raft classification extraction checks audible STALE,
BUSY, AGAIN, NOLCK and NOT_FOUND, conflict advancement, retained IO/NOMEM and
short-frame rejection. The first full-suite attempt found a missing constant in
an existing extraction fixture; repaired to read the actual source definition
and the full suite rerun passes. Final build has no warnings.

Installed978caaf7 restored; both mounts independently verified, pressure off.
Test log labels were corrected afterward (previous APPEND_NEXT printed a UUID
byte as subcommand); strict AMD compile passes and runtime logic is unchanged.
Production full UUID/epoch unification and retained lease-edge ownership, plus
PUT tickets, remain open. No pressure/D25 activation or hardware/distributed
RDMA proof is claimed.
