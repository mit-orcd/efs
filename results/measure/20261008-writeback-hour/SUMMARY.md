# SUMMARY — results/measure/20261008-writeback-hour

Writeback-hour campaign: baseline `4198a9af` plus the focused active PUT
poll/backoff changes; includes the production TCP partial-ACK failure and its
repair (red/green), sanitizer acceptance, and the final full-acceptance dir.

## Purpose and linked item

- Writeback PUT path hardening; scope and results:
  `docs/status/writeback-hour-20261008.md`. The durability driver fix (no more
  forced TCP on remount) is devops commit `e0fcc1f`.

## Verdict

**PASS** (final state): full AMD units with peer-health and standalone PUT
fixes (`final-budget-unit.log`); `final/` holds latest-source full unit + AMD
direct/TCP broad acceptance, exact manifest, integrated hung-DNS
normal/sanitized gate, committed-document check and restored-service inventory;
`final/tcp-final.log` relinks the actual client against the final production
connection changes and repeats short/default PUT budgets and partial GET pairs.
**Retained failure**: `partial-reply-red.log` — the actual production TCP
partial-ACK failure, repaired in `partial-reply-green.log`.

## Source commit and build identity

Baseline `4198a9af` + focused PUT poll/backoff changes (README.md).

## Environment

AMD (units + direct/TCP acceptance), Mac sanitizer acceptance
(`sanitizer-frame.log`, `sanitizer-get.log`).

## Method

Hour-long writeback campaign; fixture source/binary manifest path in
`put-live.log`; installed-service restoration recorded in `put-driver.log`;
remote scripts invoked with explicit settings (SSH re-exec cannot drop the
matrix).

## Results

Final state green; the red/green pair documents the partial-ACK failure and
repair.

## Limitations and remaining gates

Sanitizer acceptance on Mac is ASan/UBSan only (no LeakSanitizer on this Mac).

## Evidence index

- `README.md`, `put-live.log`, `put-driver.log`, `final-budget-unit.log`,
  `partial-reply-red.log`, `partial-reply-green.log`, `partial-unit.log`,
  `sanitizer-frame.log`, `sanitizer-get.log`, `final/` (run
  `shasum -a 256 <file> | cut -c1-16` here)
