# GETCHUNKS failover routing correction

Parent `a4aa757f`; AMD direct-I/O/TCP, staged typed tickets and writeback.

Production GETCHUNKS formerly returned NET on a single send/receive failure and
stopped on NOT_PRIMARY without a different nonzero leader hint. It now retries
through another voter, clears dead hints and retains the exact FileID/range,
caller deadline and output ownership. Sixteen attempts remain the hard retry
cap; protocol/authoritative failures are not converted to successful reads.

The extracted production-function test covers permanent and one-off receive
failure, send failure, unavailable connection, missing/self leader hints,
whole-request deadline exhaustion, unchanged outputs on failure and reply frees.
The preceding function fails the new budget assertion (negative control).
The earlier live readback assertion had no return-code diagnostic, so this is
not claimed as proven causal attribution for that historical failure. The
new diagnostic preserves the next actual return code if the live gate fails.

`w66-routing-make-test.log`: full native/Python/documentation suite passed.
`live.log`: fixture `gc-direct-gokbz2iy`, POSIX 212/212 and peer 64/64, actual
lane leader death and retired publication replay, cold exact bytes, sync-write,
retained session/lease and physical GC gates pass. Installed service restored
and both policy mounts present. Build log has no warnings. Selected source
SHA256s match the local candidate; private builds report identity `unknown`,
so this is not an exact-build-identity W93 closure.

Two-group routing remains; this corrects retry behavior, not W66 topology scale.
