# SUMMARY — 20261009-bounded-edge-retirement

## Purpose and linked item

Unit/regression evidence for separating bounded staged lease-edge retirement
from ownership cleanup: `efs_lease_guarded_retire` now requires completed
global revocation, a local rejection floor, and no remaining old-epoch
lease/lock rows; residual ownership returns BUSY and exact eight-row cleanup
stays separate. Part of the 2026-10-09 dependency-linked correctness package
(`docs/status/dependency-package-20261009.md`). Added to raw-evidence by
commit `920eb217c` ("fix(leases): separate bounded receipt retirement from
ownership cleanup").

## Verdict

PASS for the scoped change: full AMD Linux regression passes; the final
session regression additionally proves failed receipt deletion returns IO and
retains the window for retry; five fresh child-process LSM opens prove
eight-plus-one cleanup, retirement replay, and old-epoch OPEN rejection after
cold recovery; prepared receipt ownership returns BUSY. The previous
implementation fails the new relevant assertions (15 failures in the retained
`before-session.log`; other child failures there cascade from inherited
failure counts).

## Source commit and build identity

Not recorded in the run (no version string or binary hash in the retained
files; the raw-evidence adding commit is `920eb217c`).

## Environment

AMD Linux build/test host, off-line unit/regression execution only. No
cluster, topology, transport or fixture data is recorded — the run explicitly
states "No installed service or NUC changes; installed AMD mounts remain
restored."

## Method

Build the changed lease code; run the new session assertions against the old
implementation (retained as `before-session.log`, expected failures), then the
full Linux regression with the fix (`linux-tests.log`) and the focused final
session regression (`final-session.log`), including five fresh child-process
LSM cold-recovery opens. Test sources under `tests/` (e.g.
`tests/test_session.c`); exact build invocation not recorded in the run.

## Results

| Check | Result |
|---|---|
| Old implementation, new session assertions | 15 failures (expected counter-example) |
| Full AMD Linux regression with fix | pass (`linux-tests.log`, all OK) |
| Final session regression | `test_session: OK` |
| Cold-recovery child opens (5) | 8+1 cleanup, retirement replay, old-epoch OPEN rejection — pass |
| Prepared receipt ownership | BUSY — pass |

## Limitations and remaining gates

- The helper remains staged and assumes the KV includes session authority; a
  cross-group production coordinator must check global authority
  independently.
- Does not retire publication/append/PUT receipts and does not activate
  sessions. No live-cluster or installed-service validation in this run.

## Evidence index

| File | sha256 (16) |
|---|---|
| README.md | 042cc09e504e446d |
| linux-tests.log | 72bf0a37051cdbe8 |
| before-session.log | 2dd4431ba68fe5ef |
| final-session.log | 464c32dbe2e1df78 |
