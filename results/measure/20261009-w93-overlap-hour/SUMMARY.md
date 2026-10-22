# SUMMARY — results/measure/20261009-w93-overlap-hour

W93 overlap suffix experiment on devel-persistence-pressure (NUC only).

## Purpose and linked item

- **W93** overlap suffix durability cohorts. Source commits: `e3ed79e8`
  (read-only matching-prefix/duplicate/conflict classification), `ca810bac`
  (default-off guarded overlap suffix durability cohorts, actual-core
  regressions).

## Verdict

**PASS** (per README.md): the classification (`e3ed79e8`) and the default-off
guarded cohorts (`ca810bac`) verified with actual-core regressions; preserved
production stores and prior test trees; no main devel checkout changes.

## Source commit and build identity

`e3ed79e8` + `ca810bac` on devel-persistence-pressure (README.md).

## Environment

NUC only, TCP/direct I/O, command service off.

## Method

Classification + guarded-cohort regressions per README.md.

## Results

Both commits verified per README.md.

## Limitations and remaining gates

Cohorts default-off (guarded).

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  logs in this dir
