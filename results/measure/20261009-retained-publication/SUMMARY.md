# SUMMARY — results/measure/20261009-retained-publication

Retained typed publication writeback: exact post-snapshot range ownership,
pre-PUT snapshot race refusal, cold default-export lane bootstrap, budgeted
immutable publication.

## Purpose and linked item

- Typed publication writeback (AMD private four-daemon fixture).

## Verdict

**PASS** (per README.md): the listed behaviors implemented and verified —
exact post-snapshot range ownership; pre-PUT snapshot race refusal; cold
default-export lane bootstrap before any PUT; budgeted immutable publication.

## Source commit and build identity

Per README.md.

## Environment

AMD private four-daemon fixture, direct I/O/TCP. Installed stores preserved and
the installed service restored after each run. Software RXE/hardware power-loss
are NOT acceptance claims (explicit scope).

## Method

Fixture scenarios per README.md.

## Results

All listed behaviors verified.

## Limitations and remaining gates

No RXE/power-loss acceptance claims; installed service restored after runs.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  fixture logs in this dir
