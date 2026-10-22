# SUMMARY — results/io500/20261001-074905-rdma

Debug IO-500 9×4 (36 ranks, fcstor007–015), RDMA, stonewall 1 s, fresh table —
first IO-500 with W28–W35 + D20 in both daemons. Completes every phase; one
read-verification error captured (W38).

## Purpose and linked item

- First IO-500 on the W28–W35 + D20 tree (client rdcache pending fix +
  PUT-window read wait). Added by commit `292fc6da0` ("Oct 1: fresh-cluster
  posix + IO-500 run; client read/PUT-window and rd…").

## Verdict

**PASS** (debug gate — every phase completed, no fsync failure, ior-easy-read
0 errors) **with a recorded correctness defect**: `ior-hard-read` printed
"Incorrect data on read (1 errors found)" on a same-mount read — tracked as
**W38**. All scores [INVALID] only because the debug stonewall is 1 s.

## Source commit and build identity

Servers `4b2844487831-dirty` (restart408, no recorders); clients same tree plus
the rdcache pending fix and PUT-window read wait (dep419).

## Environment

9 hosts fcstor007–015, 4 ranks each, `tests/perf/io500/config-debug.ini`,
stonewall 1 s, `EFS_TRANSPORT=rdma`, fresh table (wipe 06:37Z, mkfs 07:18Z),
IO500_KEEP_DATA=1, run rec-ior432, preflight idle (`preflight.txt`).

## Method

`io500` standard driver, `config-debug.ini`; per-phase output `result.txt`,
`result_summary.txt`.

## Results

| phase | this run | 20260930-183504 | 20260930-072824 |
|---|---|---|---|
| ior-easy-write | 5.173 GiB/s | 4.563 | 3.148 |
| mdtest-easy-write | 6.185 kIOPS | 4.632 | 3.696 |

Correctness: ior-easy-read 0 errors; ior-hard-read **1 incorrect-data error**
(W38, same-mount read).

## Limitations and remaining gates

Debug stonewall invalidates the score; the W38 single read error requires the
later 20261002 rerun for closure (0 errors there).

## Evidence index

- `NOTE.txt` — 241cc2c951c44481 (comparison table, W38 note)
- `result.txt` — c2eafe3dafe5b687
- `result_summary.txt` — 7ff1ed0a1e1a9a7a
