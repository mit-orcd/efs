# SUMMARY — results/measure/20261009-lock-revocation

Legacy lock rejection floors and audible apply verdicts: revoked identities get
STALE from legacy HOLD and FLOCK while the file stays live.

## Purpose and linked item

- Lock revocation floors: management revocation must gate legacy HOLD/FLOCK.

## Verdict

**PASS** (per README.md): actual client deferred OPEN/CLOSE/probe and cold
exact CLOSE replay pass; management durably revokes that identity; actual
legacy HOLD and FLOCK then both return STALE. The file remains live during
rejection (absent inode state cannot explain success); deletion subsequently
reclaims its fragments.

## Source commit and build identity

Per README.md.

## Environment

AMD private fixture gc-direct-2_cgyb1f, direct I/O/TCP, pressure enabled.

## Method

Deferred OPEN/CLOSE/probe + cold CLOSE replay → revoke → HOLD/FLOCK STALE →
delete/drain. Uses the explicitly chosen legacy big-endian owner UUID.

## Results

All steps pass; STALE after revocation with the file live.

## Limitations and remaining gates

Does not claim mount-wide FUSE session identity integration (explicit scope
note in README.md).

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  fixture logs in this dir
