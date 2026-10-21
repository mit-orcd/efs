# 20260921-161931-samedir-rate — same-directory op rate

## Purpose and linked item
Measures the create/mkdir/unlink/rmdir rate in one shared parent directory at
1/9/36 client procs. Linked from `docs/backlog/work-items.md` ("mdtest `rmdir`
rate" item): the flat ~138/134/159 ops/s recorded here was diagnosed as one
AppendEntries per create (a proposer arriving during a fsync hold broadcast a
single entry under the raft lock). Adding commit: `70bda9037` ("measure:
same-directory rate is a flat ~150 ops/s, not BUSY backoff"). Follow-up
remeasurement after the batching fix: `results/measure/20260927-211953-samedir-rate`.

## Verdict
Measurement run — no pass/fail gate. All three storm phases report PASS
(parent left clean). The measurement itself is the finding: the rate is flat
~150 ops/s independent of concurrency, and the cause is raft batching, not
BUSY backoff (`busy_n` ≤ 1 everywhere).

## Source commit and build identity
`b2184a5c7faf-dirty` on all four servers (preflight.txt, 2026-09-21T16:19:31Z).

## Environment
Cluster 19810: efsd on fcstor003–006, raft groups g0 (voters 0x7) and g2
(voters 0xe), commit==applied, idle (~1 entry/s) at start. Clients
fcstor007–015, FUSE (`fuse.efs-fuse`) mounted at /tmp/efs-mount, TCP
transport (pre-RDMA switch). Driver: node9901.

## Method
Script: `tests/measure/samedir_rate.sh` (ROUNDS=100; 4 ops per round —
mkdir, create, rmdir, unlink — in ONE parent). Levels 1x1, 1x9, 4x9 (4 procs
on each of 9 hosts). Client mounts were remounted before each level
(remount-*.txt); mounts restored afterwards. Server-side raft histograms and
PERF=1 profiles collected during the runs.

## Results
| level | procs | ops | wall_s | ops/s | ms/op/proc | busy_n | recv_s | create_avg_us | unlink_avg_us |
|-------|-------|-----|--------|-------|-----------|--------|--------|---------------|---------------|
| 1x1 | 1 | 400 | 2.9 | 137.9 | 7.25 | 0 | 3.2 | 2362 | 11974 |
| 1x9 | 9 | 3600 | 26.8 | 134.3 | 67.00 | 1 | 193.5 | 11602 | 33479 |
| 4x9 | 36 | 14400 | 90.5 | 159.1 | 226.25 | 0 | 875.4 | 25917 | 36626 |

Rate does not scale with procs; per-op latency grows linearly → server-side
serialization. recv_s (server+wire wait) dominates; BUSY backoff is ~0, so
the cap is not contention retry.

## Limitations and remaining gates
Diagnosis only; the fix (local append during fsync hold, burst fsync marks
covered indexes durable, one batch from the pump) is validated by the Sep 27
rerun. busy/recv accounting is client-side only.

## Evidence index
- `c03840240dd405d7` SUMMARY.txt — run-level numbers and column definitions
- `7983d91166bda244` table.tsv — machine-readable results table
- `15f3c56715e58c80` preflight.txt — build identity, raft state, mount check
- `f2d9fc5270f5cb27` rpcprof-4x9.txt — per-RPC profile at 36 procs
- `f0b29dd985611510` rpcprof-1x9.txt — per-RPC profile at 9 procs
- `e1328a216d70e9a2` raft-hist.txt — server raft apply/append histograms
- `c26de83abdc9496c` storm-4x9/result.txt — phase verdict (PASS)
- `8531ed4f27acec65` perf-samedir-g0-fcstor004.txt — PERF=1 server profile (g0)
