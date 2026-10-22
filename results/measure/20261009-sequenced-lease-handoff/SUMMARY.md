# SUMMARY — results/measure/20261009-sequenced-lease-handoff

Legacy/sequenced ownership handoff: legacy HOLD's reader now validates sequenced
lease record versions 1, 2 and 3 instead of classifying them as protocol errors.

## Purpose and linked item

- Legacy↔sequenced ownership handoff. Baseline `f9a3deaa`.

## Verdict

**PASS** (per README.md): legacy OPEN can observe an already-active sequenced
normal owner without changing its sequence or pending token; legacy CLOSE,
pending cleanup, and OPEN of a sequenced closed/pending-only record return
STALE — they cannot remove or downgrade a sequenced record; exact sequenced
edges remain required.

## Source commit and build identity

Baseline `f9a3deaa` (README.md).

## Environment

AMD fixture, October 9.

## Method

Handoff scenarios per README.md.

## Results

All listed behaviors verified.

## Limitations and remaining gates

Legacy reader scope; sequenced protocol per its own runs.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  logs in this dir
