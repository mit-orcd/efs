# Dependency-linked correctness work — October 9 UTC

Branch devel-w66, baseline ab050595 (all then-current devel branches merged).
No other active agents per user. AMD direct/TCP is the default functional cycle;
commands remain disabled, D25 and pressure policy remain staged.

## Recommended sequence

1. D29/W88 and D30 cleanup/admission foundations: bound remote lane selection
   and append reservation/receipt cleanup; prove replay and charge state-machine
   expansion before pressure activation.
2. W72/W86: production mount session lifecycle, durable revocation and PUT
   tickets. Those provide identity/lifetime authority for publication and GC.
3. D27 plus W88 production publication: pre-write FileID/geometry admission,
   immutable accepted-byte ownership across every path, ordered receipt
   consumption/retirement, deadlines and recovery.
4. D25/W77: integrate typed publication/read views and request-wide observation,
   then lost-ACK, failover, cold-byte and partial-commit gates before activation.
5. W66: repeat pump/read measurements with the new foundations, then versioned
   routing/topology, compatibility and measured leadership scaling.

W55/W58 remain separate historical-trigger/NFS investigations; current passing
FUSE suites do not close those triggers. W71 hardware power-loss proof needs
physical fault infrastructure. These are not solved by the above gates.

## Implemented cleanup portion

978caaf7 introduces an RPC-only SWEEP_NEXT request. The lane leader validates
its route and converts it to at most eight exact chunk keys before logging;
remote reapers no longer propose the historical whole-lane sweep.
New REAP_BATCH entries carry at most eight exact reservation/receipt keys.
Their atomic delete retries cannot consume the next batch; the reap marker
remains until remaining rows are absent. Historical REAP_DONE/LANE_SWEEP
semantics remain unchanged for replay. Mixed-version/downgrade is not certified.

The first combined live gate exposed starvation: partial progress on the first
lane delayed all later lanes. Fixed the reaper to advance independent lanes on
BUSY, retain the marker, and stop on storage/authority failures. The original
180-second deletion-drain deadline is unchanged.

Validation: Linux full regression, strict decoder/verdict fixture, failed atomic
write/malformed keys, exact batch retry, actual LSM WAL crash/reopen, AMD private
GC/restart, core POSIX 212/212, peer 64/64, exact 800 cold append records, zero
physical fragments/quota after restart and late-PUT fencing pass. This does not
establish a universal memory/work bound or finish production publication.

[Raw evidence](../../results/measure/20261009-bounded-gc-cleanup/README.md).

## Installed deployment

978caaf7 is now installed on AMD, all four daemons direct/TCP. Full installed
core POSIX212/212, peer64/64 and persistence26/26 pass. Backup ab050595 retained;
reservation released, both mounts serving, both command masks disabled.
Next unresolved dependency is admission expansion accounting; do not activate
pressure/D25 from this cleanup acceptance alone.
