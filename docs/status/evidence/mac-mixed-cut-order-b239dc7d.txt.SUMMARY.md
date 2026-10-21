# mac-mixed-cut-order-b239dc7d.txt — evidence summary

## Purpose and linked item
Validation output for the reordered mixed-cut safety oracle (patch in
`mac-mixed-cut-oracle-b239dc7d.patch`), added by commit `ce3280047`
("Isolate mixed cut safety oracle before scheduler negative controls",
2026-10-11). Linked status page:
`docs/status/mac-mixed-cut-oracle-isolation-20261012.md`.

## Verdict
PASS. Both positive configurations exit 0; all six negative-control
invocations (3 flags x 2 sanitizer modes) abort at their intended
oracle assertions with exit=1, matching `expected_failure=1`.

## Source commit and build identity
Frozen test tree `b239dc7d` (harness paths under
`/private/tmp/efs-mac-cut-review-b239dc7d`), with the test-only oracle
reordering patch applied. No binary hashes recorded in the run.

## Environment
macOS, Python 3.9 harness driving compiled C test binaries in temp
dirs; modeled scheduler, no live cluster. Sanitizer off/on matrix.

## Method
Run the patched `tests/test_query_capture_retry.py` with no flag
(sanitizer False/True), then with each of
`--unsafe-no-transaction-fence`, `--baseline-cut-backoff`,
`--unsafe-all-busy-retry` in both sanitizer modes, verifying each
unsafe variant trips its oracle.

## Results
| Configuration | Outcome |
|---|---|
| positive, sanitizer off/on | mixed-cut rejection + stable recapture; quiet cut at 590 ms (9 pins/18 proofs); 5-cohort contention over 1100 modeled ms with no publication; query capture bounded — PASS exit=0 |
| `--unsafe-no-transaction-fence` off/on | BUSY-fence assertion test.c:267, exit=1 (expected) |
| `--baseline-cut-backoff` off/on | scheduler oracle assertion test.c:212, exit=1 (expected) |
| `--unsafe-all-busy-retry` off/on | retry-bound assertion test.c:235, exit=1 (expected) |

## Limitations and remaining gates
Same scope limits as the pre-patch run: modeled timing, harness-level
negative controls only; no live-cluster qualification is implied.

## Evidence index
- `mac-mixed-cut-order-b239dc7d.txt` sha256:ad198142ce03a488 — post-patch full matrix (this file)
- `mac-mixed-cut-oracle-b239dc7d.patch` sha256:45c2bbe423202d6b — the reordering under test
- `mac-cut-review-b239dc7d.txt` sha256:747f190564ad480e — pre-patch baseline
