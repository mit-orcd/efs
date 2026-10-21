# mac-query-review-1fb5339f.txt — evidence summary

## Purpose and linked item
Query root-refusal / capture-retry / page-barrier review output, added
by commit `aaf6a3e8a` ("Review opt-in root and transaction stage
diagnostics", 2026-10-11). Linked status page:
`docs/status/mac-query-discriminator-review-20261012.md`. Runs three
query test suites in both sanitizer modes.

## Verdict
PASS. All six invocations (3 suites x sanitizer off/on) report PASS
and exit=0.

## Source commit and build identity
Review commit `1fb5339f` (frozen Mac test source); test binaries
compiled by the harness. No binary hashes recorded in the run.

## Environment
macOS, Python harness driving compiled C test binaries; in-process
query/capture fixtures, no live cluster. Sanitizer off/on matrix.

## Method
Run `tests/test_query_root_refusals.py`,
`tests/test_query_capture_retry.py` and
`tests/test_query_page_barriers.py`, each with `sanitizer=False` and
`sanitizer=True`. Coverage: root refusals (cached new root, live TXN,
catalog IO, parent/generation, binding missing/TXN/mismatch, expiry,
permission) with exact refusal and no output; opt-in root stages
(disabled quiet, exact labels, actual leaf durations, identity);
mixed-cut rejection and bounded query capture retry; first-page
authority, continuation rechecks, no bytes on failed cuts, permission
refusal on close.

## Results
| Suite | sanitizer=False | sanitizer=True |
|---|---|---|
| test_query_root_refusals.py | PASS exit=0 | PASS exit=0 |
| test_query_capture_retry.py | PASS exit=0 | PASS exit=0 |
| test_query_page_barriers.py | PASS exit=0 | PASS exit=0 |

## Limitations and remaining gates
Harness-level correctness only; live pressure/budget gates (quiet
original-budget direct runs) are tracked separately and were still
failing per the v030 reconciliation history. No binary identity.

## Evidence index
- `mac-query-review-1fb5339f.txt` sha256:200667386c15c132 — three-suite PASS matrix (this file)
- `mac-query-review-57748bb5.txt` sha256:d882783ac08fd44a — companion trace-emission review
- `mac-cut-review-b239dc7d.txt` sha256:747f190564ad480e — related capture-retry negative controls
