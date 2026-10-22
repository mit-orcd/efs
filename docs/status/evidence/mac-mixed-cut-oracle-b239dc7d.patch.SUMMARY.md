# mac-mixed-cut-oracle-b239dc7d.patch — evidence summary

## Purpose and linked item
Test-only patch to `tests/test_query_capture_retry.py`, added with its
validation output by commit `ce3280047` ("Isolate mixed cut safety
oracle before scheduler negative controls", 2026-10-11). Linked status
page: `docs/status/mac-mixed-cut-oracle-isolation-20261012.md`. The
patch moves the `scheduler_oracle()` call to after the mixed-distributed-
cut scenario, so the safety oracle runs before the scheduler negative
controls instead of before the mixed-cut check.

## Verdict
PASS (as a fixture change): the reordered test still passes all
positive configurations and every negative control still fails as
expected — see the sibling output file
`mac-mixed-cut-order-b239dc7d.txt` (all positive runs exit=0, all
`expected_failure=1` runs exit=1).

## Source commit and build identity
Applies to the frozen test tree `b239dc7d`
(`/private/tmp/efs-mac-cut-review-b239dc7d/tests/test_query_capture_retry.py`).
Test-only; no product code changed, no binary identity recorded.

## Environment
macOS, Python 3.9 harness driving compiled C test binaries, modeled
scheduler; sanitizer off/on matrix in the companion output.

## Method
Apply the patch (delete the early `scheduler_oracle();` call, re-insert
it after the mixed-cut PASS print with the comment "Safety oracle
precedes scheduling negative controls."), then rerun
`tests/test_query_capture_retry.py` across sanitizer on/off and the
three unsafe flags (`--unsafe-no-transaction-fence`,
`--baseline-cut-backoff`, `--unsafe-all-busy-retry`).

## Results
With the reordered oracle:
- Positive runs (sanitizer off/on): all four scenarios PASS, exit=0.
- All three negative controls abort at their oracle assertions,
  exit=1, matching `expected_failure=1`, in both sanitizer modes.

## Limitations and remaining gates
Confirms oracle independence/ordering only; does not itself qualify any
live gate. The change is confined to the test harness.

## Evidence index
- `mac-mixed-cut-oracle-b239dc7d.patch` sha256:45c2bbe423202d6b — the ordering patch (this file)
- `mac-mixed-cut-order-b239dc7d.txt` sha256:ad198142ce03a488 — post-patch validation matrix
- `mac-cut-review-b239dc7d.txt` sha256:747f190564ad480e — pre-patch baseline run of the same test
