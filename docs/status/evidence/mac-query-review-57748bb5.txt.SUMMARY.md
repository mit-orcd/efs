# mac-query-review-57748bb5.txt — evidence summary

## Purpose and linked item
Query stage-trace review output, added by commit `daa3f35af` ("Review
post-unlock query trace emission and timing limits", 2026-10-11).
Linked status page: `docs/status/mac-query-discriminator-review-20261012.md`.
Verifies the opt-in `query-stage` trace emission and the capture-retry
bounds, with tracing disabled and enabled.

## Verdict
PASS. All four configurations (sanitizer off/on x trace off/on) exit 0.
With `trace=1` the run emits the expected `query-stage` lines
(apply-g0/apply-g2, transaction-cut, import, authority, pin) showing
bounded retries: transaction-cut collisions converging, a 100 ms
elapsed cut, import/authority refusals (rc=-13), a generic error
(rc=-1) and pin refusals (rc=-15, rc=-2) — all refused without
publication, followed by the same PASS lines as the untraced runs.

## Source commit and build identity
Review commit `57748bb5` (frozen Mac test source); binaries compiled by
the harness. No binary hashes recorded in the run.

## Environment
macOS, Python harness driving compiled C test binaries; in-process
query capture fixture, no live cluster. Sanitizer off/on, trace off/on
matrix.

## Method
Run the query capture-retry test with `trace=0` and `trace=1`, each
under `sanitizer=False` and `sanitizer=True`; confirm traced stage
records carry exact stage names, return codes, elapsed_ms and
before/after revision counts, and that traced runs still pass the same
mixed-cut and capture-retry assertions.

## Results
| Configuration | Outcome |
|---|---|
| sanitizer=False trace=0 | mixed-cut + capture-retry PASS exit=0 |
| sanitizer=False trace=1 | stage trace emitted, refusals bounded, PASS exit=0 |
| sanitizer=True trace=0 | PASS exit=0 |
| sanitizer=True trace=1 | stage trace emitted, PASS exit=0 |

Notable traced values: transaction-cut deadline path with
elapsed_ms=100; import and authority stages refused rc=-13; pin stage
refusals rc=-15/rc=-2.

## Limitations and remaining gates
Harness-level only; trace contents are from a modeled fixture, not a
deployed daemon. Live query pressure gates remain separate.

## Evidence index
- `mac-query-review-57748bb5.txt` sha256:d882783ac08fd44a — trace on/off matrix (this file)
- `mac-query-review-1fb5339f.txt` sha256:200667386c15c132 — companion root-refusal/page-barrier review
