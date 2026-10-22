# SUMMARY — results/measure/20261009-w84-closure

W84 bounded namespace acceptance closes for the user-accepted D32 current
144-participant envelope.

## Purpose and linked item

- **W84** closure (bounded namespace envelope, D32).

## Verdict

**PASS** (closure): capacity returns the additive inode RPC OVERFLOW status and
POSIX EOVERFLOW; BUSY remains transient contention, INVAL remains an observed
cycle; server namespace-capacity diagnostics name phase, used count and limit;
admission still refuses before PREPARE; no guard omitted; the limit is
unchanged. Explicitly NOT claimed: fixed POSIX directory depth, unlimited
ancestry, or mixed-version acceptance.

## Source commit and build identity

Per README.md.

## Environment

Per README.md (W84/D32 envelope).

## Method

Closure gates per README.md.

## Results

W84 closed under the stated envelope.

## Limitations and remaining gates

No fixed-depth/unlimited-ancestry/mixed-version acceptance (explicit).

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  logs in this dir
