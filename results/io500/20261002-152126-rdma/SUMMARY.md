# SUMMARY — results/io500/20261002-152126-rdma

Debug IO-500 9×4 (36 ranks, fcstor007–015), RDMA, stonewall 1 s — the Oct 2
gate IO-500 leg alongside the posix gates (W50/D26 closure). W38 not
reproduced.

## Purpose and linked item

- Oct 2 15:21Z gate run on the same table as the Oct 1 07:18Z mkfs, together
  with posix jobs=1 200/201 (`results/posix/20261002-151947`) and posix2 63/63
  (`results/posix2/20261002-152031`), closing the W38 follow-up from
  `20261001-074905-rdma`. Added by commit `33b04eec8` ("W50 closed (GC deletes
  were not stuck) and the D26 scan cursor…").

## Verdict

**PASS**: every phase completed, no fsync failure, ior-easy-read 0 errors,
ior-hard-read 0 incorrect-data lines (the Oct 1 W38 single error does not
reproduce). SCORE [INVALID] only because the debug stonewall is 1 s.

## Source commit and build identity

Servers `111a07527093-dirty --perf`; clients remounted 15:19Z (recorded in
NOTE.txt and `docs/archive/project-history.md`).

## Environment

9 hosts fcstor007–015, 4 ranks each, `tests/perf/io500/config-debug.ini`,
stonewall 1 s, `EFS_TRANSPORT=rdma`. Same (non-empty) table as the Oct 1 mkfs
(leftover io500 trees, Used FS 0.8 %). Launched 15:21Z (rec-gsuite-iorcopy).

## Method

`io500` standard driver, `config-debug.ini`; per-phase output `result.txt`,
`result_summary.txt`, driver log `last-run.log`.

## Results

Every phase completed; 0 read-verification errors; 0 fsync failures.
Reads are same-mount — do not quote as cold.

## Limitations and remaining gates

Debug stonewall invalidates the score; same-mount reads; table not fresh.

## Evidence index

- `NOTE.txt` — 7fa449f93f920adf (gate record)
- `config.ini` — 4206a2247e4e39a9
- `result.txt` — 0d7f69cb64bae0ff
- `result_summary.txt` — 82440361249da6e0
- `last-run.log` — 1a2f18705ac4dbe6
