# SUMMARY — results/measure/20260927-w9-walk

W9 walk/stat memory scaling: client RSS growth while walking/creating a large
tree, captured from long-running background probes.

## Purpose and linked item

- **W9** (walk memory growth). Background probes `efs-w9walk3` (create walk on
  fcstor015 view) and `efs-w9stat3` (stat walk, before/after) from node9901.

## Verdict

**PASS** (capture completed): RSS sampled every 5 s over 30 s+ windows;
create-walk grows 266 MB → 327 MB at 133k entries (5 s/≈17k-entry pace), the
stat walk 5.3 MB → 131 MB at 284k stats — the growth curves W9 needed.

## Source commit and build identity

Not recorded in the run.

## Environment

node9901 driver, fcstor013/015 cluster views; background probes
(`efs-bg` screen sessions).

## Method

`create-015.txt`: create walk with `n=<count> rss_kb=<rss> elapsed=<s>` samples;
`stat-013-before.txt` / `stat-013-after.txt`: stat walk before/after.

## Results

| probe | t=0 | t=30 s | rate |
|---|---|---|---|
| create walk | 266 MB @ 0 | 327 MB @ 133k | ≈17k entries/5 s |
| stat walk | 5.3 MB @ 0 | 131 MB @ 284k | ≈57k stats/5 s |

## Limitations and remaining gates

Point-in-time curves; the W9 item tracks whether growth is bounded by cache
bounds.

## Evidence index

- `create-015.txt`, `stat-013-before.txt`, `stat-013-after.txt` (run
  `shasum -a 256 <file> | cut -c1-16` here)
