# SUMMARY — results/measure/20261009-retained-sequenced-lease

Retained sequenced client lease ownership: a caller-serialized request object
owns the full mount UUID, epoch, sequence, contiguous ACK slot, exact inode
generation, flags and pending token.

## Purpose and linked item

- Sequenced lease ownership (baseline `582aedb3`).

## Verdict

**PASS** (per README.md): ambiguous session admission/submission/probe retains
the immutable request and slot; reset refuses unresolved work; retry probes
before resubmitting the original edge; read-only probe never admits a session;
only durable stored result proof releases the slot, including negative
verdicts; completed objects reclaim (see README.md tail).

## Source commit and build identity

Baseline `582aedb3` (README.md).

## Environment

AMD private fixture, October 9.

## Method

Sequenced-lease scenarios per README.md.

## Results

All listed behaviors verified.

## Limitations and remaining gates

Per README.md scope.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  logs in this dir
