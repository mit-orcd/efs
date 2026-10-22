# SUMMARY — results/measure/20261009-revoked-client-identities

Authoritative terminal recovery after durable revocation: a retained
CREATE/lease request may stop retrying only after a stored verdict or an
authoritative global-session ReadIndex proves ACTIVE at a newer epoch.

## Purpose and linked item

- Client identity revocation semantics: FENCING, missing owner data, malformed
  state and transport failure are insufficient for terminal retry-stop.

## Verdict

**PASS** (per README.md): the completed global barrier has fenced every
registered shard; the client does not invent a new identity or automatically
revoke its mount; FUSE release and pending-token cleanup accept the separate
terminal proof and release ACK slots.

## Source commit and build identity

Per README.md.

## Environment

Fixture per README.md.

## Method

Revocation/barrier scenarios per README.md.

## Results

All listed behaviors verified.

## Limitations and remaining gates

Per README.md scope.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  logs in this dir
