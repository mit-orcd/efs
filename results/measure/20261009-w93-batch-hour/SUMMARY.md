# SUMMARY — results/measure/20261009-w93-batch-hour

W93 batch hour: earlier correctness evidence and the superseded controller.

## Purpose and linked item

- **W93** (persistence efficiency): full-service POSIX/peer and private
  read/session/publication/configuration partition gates on `c2ab465b`; private
  checkpoint injected-EIO/cold replay on `0bd7b5be`.

## Verdict

**PASS with retained negative control**: private checkpoint injected-EIO/cold
replay succeeds on `0bd7b5be` (`checkpoint-positive/result.json`); the
identical fixture FAILS on pre-fix `c2ab465b` —
`checkpoint-negative/marker-observations.json` records applied.0 advancing 4→5
after a failed KV sync. No service data was used by those fault fixtures.

## Source commit and build identity

Gates on `c2ab465b`; checkpoint fix on `0bd7b5be` (README.md).

## Environment

Service POSIX/peer gates + private fault fixtures (README.md).

## Method

Full-service suites + private partition/checkpoint fault fixtures per
README.md.

## Results

Positive/negative checkpoint pair demonstrates the fix; suite gates on
c2ab465b recorded.

## Limitations and remaining gates

The earlier controller is superseded (README.md title note).

## Evidence index

- `README.md`, `checkpoint-positive/result.json`,
  `checkpoint-negative/marker-observations.json` (run `shasum -a 256 <file> |
  cut -c1-16` here)
