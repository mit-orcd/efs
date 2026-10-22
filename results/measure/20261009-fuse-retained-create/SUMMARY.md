# SUMMARY — results/measure/20261009-fuse-retained-create

Retained FUSE CREATE and sequenced ownership: pending namespace requests with
read-only routing/result proof across ambiguous replies.

## Purpose and linked item

- CREATE retention hardening. Baseline `fdf053be`.

## Verdict

**PASS** (per README.md): FUSE regular CREATE retains a pending namespace
request, uses separate read-only routing/result proof, keeps the resolved shard
across ambiguous replies; namespace receipt proof crosses ReadIndex;
historical inode reply framing unchanged (new opt-in message types 125/126);
returned pending FileID/token adopted through a sequenced edge before the FUSE
descriptor returns; last close resolves unknown adoption/OPEN before its exact
compensating CLOSE; abandoned CREATE cleanup clears its token and never
unlinks live entries.

## Source commit and build identity

Baseline `fdf053be` (README.md).

## Environment

AMD private fixture, direct I/O/TCP.

## Method

Retained-CREATE scenarios with ambiguous-reply injection per README.md.

## Results

All listed behaviors verified (see verdict).

## Limitations and remaining gates

Opt-in message types 125/126; historical framing unchanged.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  logs in this dir
