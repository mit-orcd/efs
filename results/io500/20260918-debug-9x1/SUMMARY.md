# SUMMARY — results/io500/20260918-debug-9x1

Debug IO-500, 9 ranks × 1/host on fcstor007–015 (stonewall 1 s, score INVALID by
design). First IO-500 on the W1–W5 write-path tree; driver aborted mid-run.

## Purpose and linked item

- Harness shakeout for the IO-500 debug configuration alongside the W1–W5
  write-path work. Added by commit `43e3e4e6a` ("W1-W5 write path, replicated
  salt, Raft snapshot catch-up, IO-500 harness").
- Explicitly NOT a list submission.

## Verdict

**NOTRUN** (incomplete): the driver aborted after `mdtest-hard-delete` when the
extra phase `ior-rnd4K-easy-read` hit `blocksize == transferSize`; that phase
is now `run=FALSE` in the harness inis. `result_summary.txt` is empty because
of the abort. Phases that finished are recorded below.

## Source commit and build identity

IO-500 driver `02091cf254d8-13` (`$HOME/orcd/scratch/efs-io500`). efs build not
recorded in the run.

## Environment

9 clients fcstor007–015, 1 rank each, FUSE OK over **TCP** (0 "RDMA transport
up" lines), debug ini `tests/perf/io500/config-debug.ini` (mdtest on, find
off), stonewall 1 s. Writes used `IOR -e` (fsync on close); reads are
same-mount (dcache possible).

## Method

`io500` standard driver with `config-debug.ini`; result dir on mount
`/tmp/efs-mount/io500-results/2026.09.18-15.51.56`. Per-phase output in
`ior-*.txt` / `mdtest-*.txt`.

## Results

| phase | score | wall s | notes |
|---|---|---|---|
| ior-easy-write | 0.263 GiB/s | 70.2 | 97.5 MiB/s IOR line; close 84 s |
| mdtest-easy-write | 0.0532 kIOPS | 4.1 | 53 creates/s |
| ior-hard-write | 0.0254 GiB/s | 329.6 | 26.0 MiB/s; close 57 s |
| mdtest-hard-write | 0.0264 kIOPS | 3.6 | |
| ior-easy-read | 0.600 GiB/s | 25.4 | INVALID: IOR -R 512 errors (driver 1024) |
| mdtest-easy-stat | 0.360 kIOPS | 1.6 | |

Do not quote the stonewall GiB/s (14.24 easy / 6.19 hard) as the number.

## Limitations and remaining gates

Debug config (1 s stonewall) invalidates the score; read phase invalid via
block/transfer mismatch; run aborted before ior-rnd/find; reads are warm-mount.

## Evidence index

- `NOTE.txt` — 6ea3540ced8fb892 (run narrative, phase table)
- `config.ini` — a1be60307497d813
- `result.txt` — d8f3180b62bb0549
- `result_summary.txt` — e3b0c44298fc1c14 (empty, driver abort)
