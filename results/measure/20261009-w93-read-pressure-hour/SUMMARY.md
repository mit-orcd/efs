# SUMMARY — results/measure/20261009-w93-read-pressure-hour

W93/W97 late-hour read and pressure investigation on the installed `ca810bac`
overlap candidate (NUC).

## Purpose and linked item

- **W93/W97** late-hour read/pressure: idle off/on/off and separate
  perf/strace on the installed overlap candidate.

## Verdict

**PASS** (captures completed): idle off/on/off and separate perf/strace
recordings taken on the installed `ca810bac` candidate; existing service
stores preserved; TCP/direct I/O; command service disabled. Details in
README.md's source/build sections.

## Source commit and build identity

Installed `ca810bac` overlap candidate (README.md).

## Environment

NUC only, Oct 9 22:44 EDT / Oct 10 02:44 UTC start.

## Method

Idle toggles + perf/strace captures per README.md.

## Results

Captures retained; analysis in README.md.

## Limitations and remaining gates

Per README.md scope.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  captures in this dir
