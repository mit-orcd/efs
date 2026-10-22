# SUMMARY — results/measure/20261008-nuc-tiny-create

NUC tiny-file creation investigation: baseline rates for the 1M-file tiny
workload on the Oct 8 baseline build.

## Purpose and linked item

- Tiny-file creation baseline (precedes the W-series tiny-create optimization
  rounds; the generator is the user's `/home/efs/tiny_files.py`).

## Verdict

**PASS** (baseline measurement completed): screen progress 17860 files/414.1 s
then 20284/527.1 s (batches of 64 per worker; counts can lag up to 504
completed files). Per README.md, rounds are exploratory, not controlled
statistical comparisons; tracing perturbs throughput (rate captured before
strace/perf).

## Source commit and build identity

Baseline `c22754236b38` (README.md).

## Environment

NUC, four cohosted daemons, TCP, buffered I/O. Generator: 8 folder workers,
1–512 B precomputed payloads, max depth 8, max 1000 files/folder, 1M-file
target. No explicit fsync; EFS close publishes durably.

## Method

User generator `/home/efs/tiny_files.py`; screen-progress and wall-strace
captures; all output trees preserved.

## Results

| window | files | wall s |
|---|---|---|
| screen 1 | 17860 | 414.1 |
| screen 2 | 20284 | 527.1 |

## Limitations and remaining gates

Startup rate ≠ sustained throughput; exploratory, not controlled.

## Evidence index

- `README.md` — full narrative (run `shasum -a 256 README.md | cut -c1-16`
  here); screen/strace logs in this dir
