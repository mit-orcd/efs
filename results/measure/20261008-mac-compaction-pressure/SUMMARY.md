# SUMMARY — results/measure/20261008-mac-compaction-pressure

Mac D30/W23/W89 pressure foundations: real 1 GiB production KV cap with finite
tail growth, idle drain and durable reopen — engine-only, no Raft follower
bound claimed.

## Purpose and linked item

- **D30/W23/W89** compaction-pressure foundations (see
  `docs/status/compaction-pressure-20261008.md` checkpoint).

## Verdict

**PASS**: `kv-pressure.log` (real unchanged 1 GiB production KV cap, finite
tail growth, idle drain, durable reopen); `mac-pressure-final-tests.log` (full
Linux make test after the fixture isolation fix); `mac-pressure-private-build.log`
(isolated efs-owned fault-build tests); `mac-pressure-health.log` /
`mac-pressure-policy.log` (restored matching-build Mac daemons as efs,
direct/TCP, commands disabled, converged groups).

## Source commit and build identity

`source-sha256.txt` pins the sources; rsynced guest builds report unknown
versions.

## Environment

Mac cluster (guest daemons as efs, direct/TCP, commands disabled); fixture
isolation fixed before the final tests.

## Method

KV pressure fixture at the production 1 GiB cap; full unit run; private
fault-build tests; health/policy verification of the restored daemons.

## Results

All listed gates pass per the logs.

## Limitations and remaining gates

Engine-only: no Raft follower bound claimed; Mac VM timing is not
bare-metal representative.

## Evidence index

- `kv-pressure.log`, `mac-pressure-final-tests.log`,
  `mac-pressure-private-build.log`, `mac-pressure-health.log`,
  `mac-pressure-policy.log`, `source-sha256.txt` (run `shasum -a 256 <file> |
  cut -c1-16` here)
