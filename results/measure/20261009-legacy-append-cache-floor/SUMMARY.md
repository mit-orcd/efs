# SUMMARY — results/measure/20261009-legacy-append-cache-floor

Legacy cached append offsets honor revocation: a replayed legacy append at the
same offset returns STALE after durable revocation of its identity.

## Purpose and linked item

- Legacy append replay floor: cached offsets must respect revocation.

## Verdict

**PASS** (per README.md): the legacy 28-byte UUID/epoch/sequence suffix warms
an append replay entry, repeats at the exact same offset 45056, the identity is
durably revoked, and the identical retried request returns STALE. Original
45056 bytes plus the expected 8-byte zero hole verified. Legacy HOLD/FLOCK
after the same fence also return STALE; physical deletion then drains.

## Source commit and build identity

Per README.md.

## Environment

AMD private fixture gc-direct-03m5yh42, direct I/O/TCP, pressure enabled.

## Method

Warm replay entry → revoke identity → retry identical request → STALE +
byte-exact verification + HOLD/FLOCK fence checks + delete/drain.

## Results

STALE returned post-revocation; bytes exact; fences hold; drain completes.

## Limitations and remaining gates

Legacy-suffix scope; production append uses the transaction path.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  fixture logs in this dir
