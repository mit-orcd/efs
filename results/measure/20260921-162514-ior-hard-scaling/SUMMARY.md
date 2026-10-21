# 20260921-162514-ior-hard-scaling — IOR hard-write scaling 1/4/9/36 ranks

## Purpose and linked item
Scaling measurement for ior-hard-write (one shared file, 47008-byte
transfer/segment, 3000 segments per rank). Cited in
`docs/backlog/work-items.md` under the IO-500/ior-hard item: "372 / 33 / 69 /
82 MiB/s at 1/4/9/36 ranks" — the NP=4 collapse and sub-linear recovery are
the perf residual later owned by W17/D1 (span publish) and W38 (fold
tombstone). Adding commit: `59eb0ee93`.

## Verdict
Measurement run — no pass/fail gate. All four IOR launches completed; the
finding is the throughput collapse from 372 MiB/s (NP=1) to 33 MiB/s (NP=4)
and only partial recovery at NP=9/36 (69/82 MiB/s).

## Source commit and build identity
`b2184a5c7faf-dirty` on all four servers (preflight.txt, 2026-09-21T16:25:14Z).
REPORT path unprofiled on this build (noted in SUMMARY.txt).

## Environment
Cluster 19810: efsd on fcstor003–006 (g0 voters 0x7, g2 voters 0xe, idle).
IOR ranks spread over clients fcstor007+ (SLOTS=1 for NP 1/4/9, SLOTS=4 for
NP=36) on FUSE mounts at /tmp/efs-mount; TCP transport. Driver: node9901.

## Method
Script: `tests/measure/ior_hard_scaling.sh`. Per level: remount clients
(remount-*.txt), launch IOR `-t 47008 -b 47008 -s 3000` on one shared file
(launch-*.txt, ior-N.log), collect client RPC profile (rpcprof-N.txt) and
server `efsd report-split` timing lines (report-split-N.txt). Mounts restored
after the run (remount-restore.txt).

## Results
| np | slots | total MiB | launch_wall_s | ior_write_s | write MiB/s | busy_n |
|----|-------|-----------|---------------|-------------|-------------|--------|
| 1 | 1 | 134 | 6 | 0.36 | 372.26 | 0 |
| 4 | 1 | 537 | 21 | 16.25 | 33.11 | 0 |
| 9 | 1 | 1210 | 26 | 17.44 | 69.40 | 0 |
| 36 | 4 | 4841 | 91 | 58.78 | 82.36 | 0 |

Server REPORT timing (report-split-summary.txt): finish_ms sum grows 142 →
6660 → 10747 → 37103 ms across NP=1/4/9/36; push_ms appears only at NP=36
(sum 11540, max 3247 ms). Client busy_s is 0 at every level — the wall is
server-side report/finish, not client backoff. Baseline: NP=1 single stream
is the comparison point (372 MiB/s).

## Limitations and remaining gates
busy_n/busy_s cover rank 0's client only and exclude REPORT (unprofiled on
this build). NP=4 slower than NP=9 is unexplained here. Later gates (W17/D1,
W38) supersede these numbers; do not quote as current performance.

## Evidence index
- `f5258a410ea35324` SUMMARY.txt — run-level table and REPORT timing
- `84c21fb20e51ab3b` table.tsv — machine-readable scaling table
- `44c3ce7ddf331ed5` report-split-summary.txt — server pack/push/finish sums per NP
- `6893bdef4da41061` preflight.txt — build identity and raft state
- `6f9f8dafa99a6341` ior-4.log — IOR output for the NP=4 collapse point
- `f729fd72ff57c1a4` ior-36.log — IOR output at NP=36
- `19bd72ddae7bc398` rpcprof-36.txt — client RPC profile at NP=36
