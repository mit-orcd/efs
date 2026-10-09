# Legacy append resolution admission expansion

APPEND_RES is a 38-byte command but its legacy apply loads all reservations,
may search the entire set for the completion frontier, and may delete all rows
in one batch. It now carries an unbounded pending-work charge, consistent with
historical whole-lane sweep/reap and session LEASE_DROP commands. With staged
pressure enabled, new admission refuses it rather than claiming a finite bound;
committed apply/replay is unchanged. Installed pressure policy remains off.
Current production append completion uses the transaction path.

The exact charge regression fails before this fix and passes after it. Tests
also prove enabled admission refuses and disabled policy preserves legacy
admission. AMD full build/regression and combined staged-pressure direct/TCP
session/GC/core POSIX212/212/peer64/64 gate PASS (gc-direct-_42jqqug).
Installed service restored between fixtures; no production activation.

This does not bound last-close work: LEASE_CLOSE, close-bearing LEASE_EDGE and
CLOSE_CLEANUP can still call file-wide lock cleanup and append drain. Those
require bounded, versioned work preserving append frontier and durable close
obligations before pressure/session activation; merely refusing all production
closes would prevent required progress. No universal memory/CPU bound is claimed.
