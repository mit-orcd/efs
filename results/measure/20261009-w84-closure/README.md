# W84 bounded namespace acceptance — Oct 9 UTC

W84 closes for the user-accepted D32 current 144-participant envelope. No fixed
POSIX directory depth, unlimited ancestry or mixed-version acceptance is claimed.
Capacity returns the additive inode RPC OVERFLOW status and POSIX EOVERFLOW;
BUSY remains transient contention, INVAL remains an observed cycle. Server
namespace-capacity diagnostics name phase, used count and limit. Admission
still refuses before PREPARE; no guard is omitted and the limit is unchanged.

AMD direct/TCP, commands disabled, staged pressure only in private fixtures:
- Full normal Linux build/regression PASS.
- Extracted actual guard/participant and wire-to-FUSE errno tests PASS:
  65/144 same-shard guards; 145 overflow; mixed existing participants and
  ancestry at/beyond capacity; duplicate shard; observed cycle; authority IO
  and contention BUSY; canaries preserved.
- Fault daemon fixture gc-direct-m878za_9 PASS: 64-lane empty HASHED removal
  and replacement/restart; opposite rename cycles/progress; depth143 accepted,
  depth144 EOVERFLOW in 0.006s; exact144 PREPARE and COMMIT coordinator deaths,
  cold atomic recovery; core POSIX212/212 and peer64/64; full GC/restart.
- Normal daemon fixture gc-direct-jbhkfg6_ PASS: the same non-fault namespace
  gates; depth144 EOVERFLOW in 0.005s; independently queried authoritative source
  and destination attributes unchanged immediately and after cold restart;
  core POSIX212/212 and peer64/64; full physical GC/restart/quota/late PUT gates.

Initial normal fixture gc-direct-jr2t7t0n failed a cached-client attribute
comparison after restart. The original comparison did not capture its values;
its stores/client/server logs remain on AMD. The strengthened gate compares
independent server GETATTR values before/after refusal and cold restart and
retains cached/cold client values too. Those all match in the passing rerun.
The original client-view discrepancy's cause is not established; this closes
the authoritative bounded namespace contract, not a general cache investigation.

Normal private binaries carry unknown build labels; explicit source/binary
SHA256 identities are recorded. Installed978caaf7 restored direct/TCP with
pressure off and both mounts serving. No push, merge, tag or other activation.
Single-host/four-process crash acceptance is not hardware power-loss proof.
