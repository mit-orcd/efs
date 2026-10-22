# SUMMARY — results/measure/20261009-fuse-lease-retention

FUSE retained legacy lease ownership (AMD, Oct 9): candidate keeps failed
last-close HOLD ownership until confirmed release; failed OPEN replies retain
compensating cleanup.

## Purpose and linked item

- W66-line lease retention: baseline `136fc54f` merges devel `da653c33` into
  devel-w66 (prior tip `87fb02d4`).

## Verdict

**PASS** (per README.md): reopen uses the same inode edge stripe to finish old
cleanup before acquiring a new HOLD; clean stop retries one retained release
per iteration and refuses unresolved ownership; authoritative NOT_FOUND on
CLOSE is terminal — the legacy handler observes the inode already reclaimed,
including a lost last-close acknowledgement; ambiguous replies retained.

## Source commit and build identity

Baseline `136fc54f` = devel `da653c33` merged into devel-w66 (README.md).

## Environment

AMD private fixture, direct I/O/TCP.

## Method

Retained-ownership scenarios per README.md: failed last-close, failed OPEN
cleanup, reopen edge ordering, clean-stop retry, NOT_FOUND terminal CLOSE.

## Results

All listed scenarios pass per README.md.

## Limitations and remaining gates

Legacy handler scope; production paths per the W66 series.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  logs in this dir
