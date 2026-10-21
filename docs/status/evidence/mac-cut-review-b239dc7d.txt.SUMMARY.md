# mac-cut-review-b239dc7d.txt — evidence summary

## Purpose and linked item
Cut-capture / query-retry review output for frozen test source
`b239dc7d`, added by commit `0e24f213f` ("Qualify cheap cut retry
classification and independent negative controls", 2026-10-11). Linked
status page: `docs/status/mac-query-stage-disposition-20261012.md`.
Exercises `tests/test_query_capture_retry.py` positive paths plus three
unsafe-flag negative controls, and an isolated mixed-cut oracle run
(scheduler bypassed in the local harness only).

## Verdict
PASS, with the negative controls failing exactly as designed. Positive
configurations exit 0 in both sanitizer modes; each `--baseline-cut-backoff`,
`--unsafe-all-busy-retry` and `--unsafe-no-transaction-fence` run aborts
(SIGABRT, exit=1) at its oracle assertion, which was the expected
failure (`expected_failure=1`). The isolated mixed-cut oracle passes
with no args and fails with `--unsafe-no-transaction-fence`, as expected.

## Source commit and build identity
Frozen test tree `b239dc7d` (working copy at
`/private/tmp/efs-mac-cut-review-b239dc7d` per the tracebacks); test
binary compiled fresh per run into a temp dir. No binary hashes
recorded in the run.

## Environment
macOS, Python 3.9 (CommandLineTools framework Python) driving compiled
C test binaries; modeled/simulated scheduler, not a live cluster.
Each configuration run with `sanitizer=False` and `sanitizer=True`.

## Method
Run `tests/test_query_capture_retry.py` in both sanitizer modes with no
flag (positive), then with each unsafe flag (negative control, expecting
failure). Separately run `tests/test_query_capture_without_scheduler.py`
with and without `--unsafe-no-transaction-fence` to isolate the
mixed-cut oracle from the scheduler.

## Results
| Configuration | Outcome |
|---|---|
| positive, sanitizer off/on | quiet cut at 590 ms (9 pins/18 group proofs); 5-cohort contention (20 pins/40 proofs, 1100 modeled ms) no publication; mixed-cut rejection + recapture; query capture bounded — PASS exit=0 |
| `--baseline-cut-backoff` | oracle assertion at test.c:212, SIGABRT exit=1 (expected) |
| `--unsafe-all-busy-retry` | oracle assertion at test.c:235, SIGABRT exit=1 (expected) |
| `--unsafe-no-transaction-fence` | BUSY-fence assertion, SIGABRT exit=1 (expected) |
| isolated oracle, no args | mixed-cut + query-capture PASS exit=0 |
| isolated oracle, `--unsafe-no-transaction-fence` | assertion at test.c:268, exit=1 (expected) |

## Limitations and remaining gates
Negative controls mutate only the local harness/scheduler, not product
code; they prove the oracles detect the unsafe behaviors, not that a
live cluster is safe. Modeled timing, no real transport. Live pressure
gates remain separate (see v030 reconciliation history: quiet
original-budget direct runs still failing elsewhere).

## Evidence index
- `mac-cut-review-b239dc7d.txt` sha256:747f190564ad480e — full positive/negative control matrix
- `mac-mixed-cut-oracle-b239dc7d.patch` sha256:45c2bbe423202d6b — follow-up oracle-ordering patch on this same test
- `mac-mixed-cut-order-b239dc7d.txt` sha256:ad198142ce03a488 — rerun with the reordered oracle
