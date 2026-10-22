# SUMMARY — results/measure/20261005-040810-w23-stalled-compactor

W23/P2.3 stalled-compactor measurement: compactor parked on one node while
writes continue — observed via kv-obs/raft-obs sampling.

## Purpose and linked item

- **W23 / P2.3** (stalled compactor under load). Uses the fault hook documented
  in `20261005-014440-w23-stalled-compactor`.

## Verdict

**PASS** (measurement completed): node 3 (follower of both groups) ran
`EFS_FAULT_COMPACT_STALL=1` with `/tmp/efs/fault` present — compactor parked
between iterations, L0 flushes continued, L0 merges stopped; per-node sampling
every 5 s recorded kv-obs (mt_bytes/l0_bytes/n_l0/n_l1), raft-obs
pump_hold_max, apply-sleep and kv-compact backpressure line counts, and raft
status.

## Source commit and build identity

Fault build via `make EXTRA_DEFS=-DEFS_FAULTS=1` (see setup dir); scratch TCP
cluster.

## Environment

Private 3-node cluster on efs1 (`tests/rdma_first_inode.sh` layout, ports
19550–19552, TCP, scratch storage), one FUSE client; dd bs=1M conv=fsync of a
non-zero source to fresh 256 MiB files.

## Method

Fault injection on node 3; sampled observers every 5 s per node; dd workload
through the FUSE mount.

## Results

Compaction stalls isolated to node 3 while flushes continue — the kv-obs/raft
timeseries in this dir carry the per-node detail.

## Limitations and remaining gates

TCP scratch cluster; the RDMA/large-cluster behavior is not covered here.

## Evidence index

- `SUMMARY.txt` — setup + sampling description (run `shasum -a 256 SUMMARY.txt
  | cut -c1-16` here)
- per-node observer outputs in this dir
