# Bounded lease/lock cleanup resume — AMD

Candidate based on 9e474f13, with management `cleanup-leases` and its live test.
AMD private fixture gc-direct-fom6diyh, port29240, direct-I/O/TCP, pressure off.

- Full Linux regressions pass; command-loop faults cover ambiguous transport,
  incomplete authority, malformed result, explicit empty proof, 4096-batch work
  cap and 30-second admission deadline (an already-started RPC may finish later).
- Thirteen leases plus eight locks are durably fenced. Eight lease rows are
  removed, exact replay leaves the next five leases intact, then all daemons
  crash. The production CLI resumes the same revoked epoch after restart and
  independent NEXT proves no lease/lock rows remain. Physical files reclaim.
- Guarded lease reorder/lost reply/read-only probe/cold replay/final-close pass.
- Core POSIX212/212, peer64/64, append audit, cold physical inventory, zero quota
  and late PUT fence pass. Both installed direct/TCP mounts restored.

`efs-mgmt raft-session NODE:PORT cleanup-leases UUID OLD_EPOCH SHARD` drains only
old lease/lock rows; it cannot claim append or PUT-ticket cleanup. The server
requires a completed global barrier and local floor. Ambiguous failures stop the
invocation; the explicit old-epoch command resumes without BEGINing a new epoch.
Full `fence` retains legacy append cleanup. Switching it to lease-only NEXT was
rejected before deployment because that would omit FENCED_HOLE resolution.
No pressure, session, or D25 production activation is claimed.

Frozen daemon SHA256: c9750bf4872e995a94f80e9366a053671086bb0d63759ddc5768655989d0cf2a
Frozen management SHA256: eaca22bf02ced32fcb1dda41abcb683d5720071b77615d4a7840e9cb55624dd7
