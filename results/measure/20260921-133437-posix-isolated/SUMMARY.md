# SUMMARY — results/measure/20260921-133437-posix-isolated

Isolated rerun of the slow posix suite-1 tests, one host, after the W8 storm
investigation: are the failing tests intrinsically slow or only load-affected?

## Purpose and linked item

- **W8** (posix storm / slow-test investigation). Added with the W8-era commits;
  companion runs: `results/posix/20260921-*` gates and `results/measure/
  20260921-*` W8 studies.

## Verdict

**PASS**: all six slow tests pass in isolation on fcstor007 within their 15 s
budgets — the earlier failures were load contention, not defects.

## Source commit and build identity

`b2184a5c7faf-dirty` (recorded in SUMMARY.txt).

## Environment

Host fcstor007, single client, per-test budget 15 s (secs include ~1 s python
startup; a --filter run executes the matching tests' setup only).

## Method

`tests/posix/posix_suite.py --filter <name>`-style isolated runs, six tests:
concurrent_creates_same_dir, mtime_monotonic_many_writes, dir_deep_nesting,
dir_deep_nesting_beyond_64, names_crazy_dirs, dir_many_files.

## Results

| test | result | secs | ops | ms/op |
|---|---|---|---|---|
| concurrent_creates_same_dir | PASS | 4.886 | 160 | 30.5 |
| mtime_monotonic_many_writes | PASS | 1.077 | 80 | 13.4 |
| dir_deep_nesting | PASS | 6.057 | 64 | 94.6 |
| dir_deep_nesting_beyond_64 | PASS | 4.757 | 200 | 23.7 |
| names_crazy_dirs | PASS | 3.341 | 0 | - |
| dir_many_files | PASS | 5.182 | 0 | - |

## Limitations and remaining gates

Isolated timings say nothing about behavior under the storm load; that gate is
the 9-host suite runs of the same day.

## Evidence index

- `SUMMARY.txt` — `shasum -a 256 SUMMARY.txt | cut -c1-16` in this dir
- per-test logs `concurrent_creates_same_dir.txt` … `dir_many_files.txt`
- `preflight.txt`
