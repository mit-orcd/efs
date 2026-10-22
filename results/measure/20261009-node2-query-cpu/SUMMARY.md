# SUMMARY — results/measure/20261009-node2-query-cpu

Node-2 query CPU profile: perf record of the query path under load, ~24.08
billion cycles, zero lost samples.

## Purpose and linked item

- Query-path CPU attribution on node 2 (perf record of query workload).

## Verdict

**PASS** (capture completed): perf.data with 1K samples of cycles:P
(~24,077,785,988 events), 0 lost samples; reports in perf-report.txt /
perf-inclusive.txt; thread attribution in perf-threads.txt /
perf-thread-pids.txt; recorder log perf-record.log.

## Source commit and build identity

Not recorded in the run.

## Environment

Node 2 of the NUC deployment, query workload.

## Method

perf record over the query workload; inclusive + thread reports.

## Results

Perf captures retained for the query CPU analysis (inclusive/thread reports).

## Limitations and remaining gates

Single-node capture; the conclusions live in the W92 query analysis runs.

## Evidence index

- `perf-report.txt`, `perf-inclusive.txt`, `perf-threads.txt`,
  `perf-thread-pids.txt`, `perf-record.log`, `perf.data` (run
  `shasum -a 256 <file> | cut -c1-16` here)
