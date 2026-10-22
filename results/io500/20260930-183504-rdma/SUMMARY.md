# SUMMARY — results/io500/20260930-183504-rdma

Debug IO-500 9×4 (36 ranks, fcstor007–015), RDMA, stonewall 1 s — **every phase
completed cleanly**: START-HERE item 10, "one 9-client IO-500 that completes
ior-hard-write".

## Purpose and linked item

- Item 10 of the START-HERE roadmap: a 9-client debug IO-500 completing every
  phase including ior-hard-write. Added by commit `3a17a503c` ("IO-500 item 10:
  a 9-client debug IO-500 completes every phase; ior-har…").

## Verdict

**PASS**: every phase completed, **no fsync failure**, no read error in
ior-hard-read or ior-easy-read (grep over IOR result files = 0). All numbers
[INVALID] only because the debug stonewall is 1 s.

## Source commit and build identity

Servers `f2d3a7871b96-dirty` rolled --all 18:30Z (ROLL_OK); clients deployed
18:32Z on the same tree (nine fuse.efs-fuse MOUNT_OK). IO500_KEEP_DATA=1.

## Environment

9 hosts fcstor007–015, 4 ranks each, `tests/perf/io500/config-debug.ini`,
stonewall 1 s, `EFS_TRANSPORT=rdma`, run rec-ior340, launched 18:35:04Z.

## Method

`io500` standard driver, `config-debug.ini`; per-phase output in `result.txt`,
`result_summary.txt`.

## Results

| phase | this run | 20260930-072824 | 20260928-150609 |
|---|---|---|---|
| ior-easy-write | 4.563 GiB/s | 3.148 | 2.917 |
| mdtest-easy-write | 4.632 kIOPS | 3.696 | 3.418 |

## Limitations and remaining gates

Debug stonewall invalidates the score; reads same-mount; previous day's
shard-lock stall did not recur on this build.

## Evidence index

- `NOTE.txt` — 59544fa9a98a320a (comparison table, deploy record)
- `result.txt` — 839c5fa0be6b77d4
- `result_summary.txt` — 90870f986c16cbb9
