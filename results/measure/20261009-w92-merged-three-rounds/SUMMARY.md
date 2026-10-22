# SUMMARY — results/measure/20261009-w92-merged-three-rounds

W92 merged-source suites plus three further profile rounds on xorinox
(baseline `8aeb9c4b`, devel-find-stats).

## Purpose and linked item

- **W92** merged-source acceptance + profile rounds.

## Verdict

**PASS** (per README.md): native `make test` exit 0; all five docs gates;
POSIX harness self-tests; 212/212 local baseline and EFS core, equivalent;
three further profile rounds completed (details in README.md).

## Source commit and build identity

Baseline `8aeb9c4b`, devel-find-stats (Oct 9 EDT / Oct 10 UTC).

## Environment

Xorinox files engine, direct I/O, TCP; user explicitly granted exclusive
access; no wipe, push, shared-devel edits or durability relaxation.

## Method

Merged-source gates + three profile rounds per README.md.

## Results

All gates pass (see verdict).

## Limitations and remaining gates

Per README.md.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  round logs in this dir
