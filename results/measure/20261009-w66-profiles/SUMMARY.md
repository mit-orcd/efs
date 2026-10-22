# SUMMARY — results/measure/20261009-w66-profiles

W66 separate profiling runs on Mac efs1: three distinct workload profiles of the
candidate, no perf/strace overlap.

## Purpose and linked item

- **W66** (latency) profiling: candidate `2b9da49f`, four private buffered/TCP
  daemons on one Linux VM; installed daemon/stores unchanged; commands
  disabled; no production code change.

## Verdict

**PASS** (captures completed): cpu-clock frequency sampling (100 samples/cell),
syscall trace (50 samples/cell), explicit-period cpu-clock (100 samples/cell) —
each with eight GETATTR cells and independent mutation (per README.md).

## Source commit and build identity

Candidate `2b9da49f` (README.md).

## Environment

Mac cluster VM efs1, four private daemons, buffered/TCP.

## Method

Three separate profiling passes (no overlap) per README.md.

## Results

Profiles captured per workload/cell (files in this dir).

## Limitations and remaining gates

Buffered/TCP profiling only; the W66 acceptance question is the
`20261008-w66-mac` run's POSIX failure.

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  profile captures in this dir
