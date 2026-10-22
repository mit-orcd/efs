# mac-client-reconciliation-d26e6874.txt — evidence summary

## Purpose and linked item
Functional test output for the Mac FUSE client CREATE-ownership
reconciliation, added by commit `46609ad7f` ("Reconcile Mac CREATE
ownership with retained release adoption", 2026-10-11). Linked status
page: `docs/status/mac-client-reconciliation-20261012.md`. It compares
the `devel-mac` tree against the frozen `efs-mac-client-reconcile-d26e6874`
tree on the two client-ownership suites.

## Verdict
PASS. All eight test invocations report PASS and `exit=0` (2 trees x
2 test scripts x sanitizer on/off).

## Source commit and build identity
Trees are named in the log: `devel-mac` and
`efs-mac-client-reconcile-d26e6874` (frozen test source `d26e6874`;
this short SHA also appears as `frozen_amd_test_source` in
`docs/status/v030-owner-commit-reconciliation.json` history). No binary
hashes recorded in the run.

## Environment
macOS host (test harness under `/private/tmp/efs-mac-client-reconcile-d26e6874`
in related logs), Python harness running compiled C test binaries, each
configuration run twice with `sanitizer=False` and `sanitizer=True`.
Transport/topology/quota: not recorded in the run (in-process FUSE
client unit tests, no live cluster).

## Method
Run `tests/test_fuse_lease_retention.py` and
`tests/test_pending_create_cleanup.py` from each source tree, with and
without sanitizers. Coverage: ambiguous OPEN/CLOSE lease retention,
combined CREATE adoption (one lease, two descriptors, last-close
ownership), negative verdict, reopen barrier, clean-stop retry, and
pending-CREATE cleanup (bounded admission, unattempted cancel, unknown
proof retention, old-close barrier, immutable token retry, negative
proof).

## Results
| Tree | Suite | sanitizer=False | sanitizer=True |
|---|---|---|---|
| devel-mac | test_fuse_lease_retention.py | PASS exit=0 | PASS exit=0 |
| devel-mac | test_pending_create_cleanup.py | PASS exit=0 | PASS exit=0 |
| efs-mac-client-reconcile-d26e6874 | test_fuse_lease_retention.py | PASS exit=0 | PASS exit=0 |
| efs-mac-client-reconcile-d26e6874 | test_pending_create_cleanup.py | PASS exit=0 | PASS exit=0 |

The reconcile tree additionally passes the CREATE-adoption/competing-OPEN
barrier scenario (one lease, two descriptors, last-close ownership).

## Limitations and remaining gates
Unit-level harness only; no live-cluster or mounted qualification is
covered by this file. No binary identity recorded. Cross-host
qualification and release acceptance are out of scope here.

## Evidence index
- `mac-client-reconciliation-d26e6874.txt` sha256:bb811a07a7bfb427 — full PASS/FAIL matrix (the only raw file for this item)
- Related: `mac-cut-review-b239dc7d.txt` sha256:747f190564ad480e — same harness family, cut-retry review
- Related: `mac-retained-orphan-function-tests.txt` sha256:046c98e9826a67e6 — same harness family, orphan preflight
