# SUMMARY — results/measure/20261008-w92-query-catalog

W92 offline catalog validation on a NUC private build.

## Purpose and linked item

- **W92** (offline catalog). Builds run only in NUC `/data1/efs/w92-build`; the
  running cluster is unchanged and the population loader continues.

## Verdict

**PASS** with a retained initial failure: the initial catalog test failed one
expectation — hardlink updates file ctime to 220, so the test's expected
maximum 210 was wrong; the corrected test expects 220 (initial failure
retained). Final NUC catalog and name-index tests pass.

## Source commit and build identity

Rsynced build without git metadata identifies as unknown; the accompanying
source commit is the identity for this offline component (README.md), not a
deployed build ID.

## Environment

NUC `/data1/efs/w92-build`, offline; live cluster untouched.

## Method

Catalog + name-index unit tests on the private build; corrected expectation
after analysis.

## Results

| stage | result |
|---|---|
| initial catalog test | FAIL (wrong expectation, corrected) |
| final catalog + name-index | PASS |

## Limitations and remaining gates

Offline component only; not a deployed-build identity.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  test logs in this dir
