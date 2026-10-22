# SUMMARY — results/measure/20261009-profile-repeat

Fresh four-node CPU profile repeat under the active 80k-file tiny loader —
method repeat of the all-efsd capture.

## Purpose and linked item

- Repeat of `20261009-all-efsd` under the loader tiny-clock-64cads (80,000-file
  target, eight workers, depth eight, 1,000 files per folder).

## Verdict

**PASS** (capture completed): simultaneous 30 s 199 Hz perf across all
production daemons: 6,761 samples, zero lost; CPU counters bracket 31.317 s
including capture overhead.

## Source commit and build identity

Build `64cdf183d265`, direct I/O/TCP, `.efs` enabled.

## Environment

NUC production service, four daemons; no code/service/generator/configuration
changes for the capture.

## Method

perf 199 Hz × 30 s per daemon + CPU counter brackets; filtered reports use
per-PID relative percentages (inclusive paths overlap — do not sum).

## Results

Sample counts and bracketing above; per-PID tables in README.md.

## Limitations and remaining gates

Relative per-PID percentages; inclusive rows overlap.

## Evidence index

- `README.md` — narrative + tables (run `shasum -a 256 README.md | cut -c1-16`
  here); perf reports in this dir
