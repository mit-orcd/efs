# SUMMARY — results/perf/20260918-w4-honest

W4 "honest" fio perf measurement, morning of 2026-09-18 before the table
grew: 1-client bandwidth matrix on fcstor007, TCP 19810.

## Purpose and linked item

- **W4** (performance): honest single- and multi-client fio numbers for the
  FUSE mount (writes include fsync-on-close; reads same-mount). Added by commit
  `30c41ee47` ("W6 correctness batch + the 100 ms metadata floor").

## Verdict

**PASS** (measurement completed): all fio jobs err=0 with FUSE_OK. Numbers
below are the W4 baseline; the W5 50 GiB seq-write completed at 341 MiB/s
(later loaded runs EIO on fsync — recorded in the W5 note).

## Source commit and build identity

Not recorded in the run.

## Environment

1 client fcstor007 (FUSE_OK), TCP 19810, morning run before table growth.

## Method

fio 3.35 psync iodepth=1, per-matrix job files (1-ow-1m, 1-rr/rw-4k/128k/1m,
1-sr/sw-1m, 1-sr/sw-50g); per-client 4-way variants on fcstor007–010.

## Results (MiB/s, 1-client)

| test | bw | test | bw |
|---|---|---|---|
| sw-1m | 694 | sr-1m | 4102 |
| ow-1m | 694 | rr-1m | 2556 |
| rw-1m | 699 | rr-128k | 1577 |
| rw-128k | 712 | rr-4k | 154 |
| rw-4k | 385 | sw-50g | 341 (W5; completes) |
| | | sr-50g | 2203 |

4-client fio (007–010, own trees, morning): sw-1m AGG 1589 (609/316/332/332)
= 2.3× the 1-client 694; ow-1m AGG 636; rw-1m AGG 343.

## Limitations and remaining gates

Reads are same-mount (dcache warm); morning/unloaded table; see
`20260918-w4-honest4` for the loaded-table rerun (timeouts).

## Evidence index

- `raw.tsv` — machine-readable matrix (run `shasum -a 256 raw.tsv | cut -c1-16`
  in this dir)
- `1-ow-1m-fcstor007.txt` — full fio output incl. latency percentiles
- `1-sr-50g-fcstor007.txt`, `1-sw-50g-fcstor007.txt` — W5 50 GiB runs
