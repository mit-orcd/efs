# SUMMARY — results/measure/20261009-query-clock

One fix: batched catalog deadline clock reads (commit/deployed build
`64cdf183d265`, devel-find-stats).

## Purpose and linked item

- Query deadline clock batching: monotonic deadline checked on the first
  cancellation callback and every 256 callbacks, and unconditionally before
  publishing.

## Verdict

**PASS** (per README.md): shutdown still checked atomically on every
cancellation callback; deadline detection may lag by at most 255 callbacks
(not a guaranteed wall-time bound — documented); an expired build cannot
publish; the counter belongs exclusively to the publisher thread.

## Source commit and build identity

`64cdf183d265`, devel-find-stats (README.md).

## Environment

Catalog/query path (NUC deployment line).

## Method

Targeted deadline tests per README.md.

## Results

Batched deadline checks verified with the documented 255-callback lag bound.

## Limitations and remaining gates

The lag bound is callback-count, not wall-time (explicit in README.md).

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  test logs in this dir
