# SUMMARY — results/measure/20260927-211953-samedir-rate

Same-directory op-rate scaling: mkdir/create/rmdir/unlink in ONE parent at
1/9/36 procs, with BUSY/recv time accounting.

## Purpose and linked item

- Directory-hotspot rate measurement (samedir storm), referenced by the
  dd-wall study (`20260928-033420-dd-wall`) and the 20260927-190509 posix2 run.

## Verdict

**PASS** (measurement completed): 1→36 procs scale 333→1241 ops/s aggregate
with near-zero BUSY backoff (busy_n=0/0/1, busy_s=0).

## Source commit and build identity

`ab458efab95b-dirty` (recorded in SUMMARY.txt).

## Environment

fcstor007–015 clients, ROUNDS=100, 4 ops per round (mkdir, create, rmdir,
unlink) in one parent; levels 1×1 / 1×9 / 4×9.

## Method

Same-dir storm driver; per-proc ms/op; busy_s/recv_s summed over clients;
raft histograms `raft-hist.txt`; perf captures `perf-samedir-g*.txt`
(PERF=1); raw `storm-*`, `rpcprof-*.txt`.

## Results

| level | procs | ops | wall s | ops/s | ms/op (proc) | busy n | recv s | create avg µs | unlink avg µs |
|---|---|---|---|---|---|---|---|---|---|
| 1×1 | 1 | 400 | 1.2 | 333.3 | 3.00 | 0 | 1.1 | 3504 | 2179 |
| 1×9 | 9 | 3600 | 2.6 | 1384.6 | 6.50 | 0 | 22.4 | 5471 | 5294 |
| 4×9 | 36 | 14400 | 11.6 | 1241.3 | 29.00 | 1 | 112.8 | 6653 | 6756 |

## Limitations and remaining gates

busy_s/recv_s are summed client-side estimates; see dd-wall for the cross-run
comparison.

## Evidence index

- `SUMMARY.txt` — rate table (run `shasum -a 256 SUMMARY.txt | cut -c1-16` here)
- `raft-hist.txt`, `perf-samedir-g*.txt`, `storm-*`, `rpcprof-*.txt`
