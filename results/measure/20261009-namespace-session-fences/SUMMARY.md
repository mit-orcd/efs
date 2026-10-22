# SUMMARY — results/measure/20261009-namespace-session-fences

Namespace session admission and revocation gates: served-receipt shard
registration before the first effect, local authoritative views, epoch floors.

## Purpose and linked item

- Namespace session fencing (epoch floor + shared guards).

## Verdict

**PASS** (per README.md): normal identified CREATE/mkdir/unlink/rmdir/link/
rename register their served receipt shard before the first effect; existing
shard participants use a fresh local authoritative view instead of consulting
the global owner per request; cached replies and single-log effects check the
local epoch floor; receipt transaction PREPAREs acquire shared epoch guards;
revocation cannot advance that floor until every guard resolves; shared guards
allow parallel operations from one mount; RESOLVE never writes an older epoch
snapshot.

## Source commit and build identity

Per README.md.

## Environment

Fixture per README.md (details there).

## Method

Admission/revocation gate suite per README.md.

## Results

All listed fence behaviors verified.

## Limitations and remaining gates

Per README.md scope.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  logs in this dir
