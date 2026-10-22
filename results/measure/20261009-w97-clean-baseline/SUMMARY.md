# SUMMARY — results/measure/20261009-w97-clean-baseline

W97 clean-baseline measurement: eight-worker tiny-file workload, fixed seed
970001, fixed payload pool, cooperative stop at 300 s.

## Purpose and linked item

- **W97** clean baseline: source `4b149e88`; installed build
  `4b149e88-batchobs`. Dedicated fresh output tree, max 500,000 files to
  prevent an early finish.

## Verdict

**PASS** (measurement completed): timed 300 s run with fixed seed/payloads;
per-folder counts, depths and sizes retain the original generator's
distributions; no compilation or functional suites overlapped the timed run;
all data retained.

## Source commit and build identity

Source `4b149e88`; installed build `4b149e88-batchobs` (README.md).

## Environment

TCP/direct I/O, command features disabled; eight-worker tiny-file workload.

## Method

Fixed-seed generator run, cooperative stop at 300 s; raw data in this dir.

## Results

Baseline measurement retained (numbers in the run's data files/README.md).

## Limitations and remaining gates

Baseline for the W97 comparison runs (w97-flow-*, w97-group-isolation etc.).

## Evidence index

- `README.md` — narrative (run `shasum -a 256 README.md | cut -c1-16` here);
  data files in this dir
