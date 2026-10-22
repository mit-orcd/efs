# SUMMARY — results/measure/20260929-040800-idle-trace

Idle-cluster trace capture: perf + strace recordings of all four servers and
the fuse client while the cluster sat idle, with analysis detail files.

## Purpose and linked item

- Idle-cost investigation (what efsd/efs-fuse do with no workload): perf.data +
  strace per node, then reduced detail files under `ana/`.

## Verdict

**PASS** (capture completed): per-node traces recorded and reduced —
`ANA_DONE fcstor003 04:30:03` marker present; fcstor003 inventory lists
efsd.data (30 MB), efsd.strace (2.4 GB), fuse.data, fuse.strace (65 MB).

## Source commit and build identity

Not recorded in the run.

## Environment

Servers fcstor003–006 + fuse client, idle cluster, 2026-09-29 00:27–04:30Z;
perf=2 strace=2 recorders per node.

## Method

perf record + strace on every node while idle; reduction into
`ana/idle-detail(2)-fcstor*.txt`; node inventories `node-*.txt`.

## Results

Detail reductions per node under `ana/` carry the idle-CPU/syscall findings;
the raw perf.data/strace are large captures retained in this run dir.

## Limitations and remaining gates

Captures are heavy (2.4 GB strace) — consult `ana/idle-detail*` before the raw
files.

## Evidence index

- `node-003.txt` — inventory + ANA_DONE marker (run `shasum -a 256
  node-003.txt | cut -c1-16` here)
- `ana/idle-detail-fcstor003.txt` … `ana/idle-detail2-fcstor006.txt`
- `node-004.txt`, `node-005.txt`, `node-006.txt`
