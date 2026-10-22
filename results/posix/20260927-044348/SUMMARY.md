# SUMMARY — results/posix/20260927-044348

POSIX correctness gate on the 9-host fcstor cluster (9 clients, per-host suite + XFS baseline comparison), 2026-09-27 04:43Z.

## Purpose and linked item

- Added by commit `2dd77dac8` ("Compact by folding L0 when the file cap forces it, drop the shard lock…").
- Project history: `results/posix/20260927-044348`) and was rolled back. Sep 28
- Project history: (`results/posix/20260927-044348`), against the TCP 200/201 in 31 s.

## Verdict

**FAIL**: 45 test failure(s) across hosts, up to 8 baseline-vs-target mismatches ("EFS BUGS") per host: concurrent_appends, concurrent_creates_same_dir, dir_deep_nesting, dir_deep_nesting_beyond_64, hardlink_rename_one_name, hardlink_terminal_ln.

## Source commit and build identity

Not recorded in the run.

## Environment

9 clients fcstor007–015 (FUSE, per-host suites), suite `posix_suite.py (~201 tests, jobs=16 vs XFS baseline)`; XFS baseline `xfs-baseline.tsv` where present. Transport/build per the era's roll log (not recorded in the run).

## Method

Per-host run of `tests/posix/posix_suite.py --jobs 16` on the mounted fs plus an XFS baseline, then `compare.py` per host (`compare-<host>.txt`).

## Results

| host | pass/total | fail | skip |
|---|---|---|---|
| fcstor007 | 196/201 | 4 | 1 |
| fcstor008 | 196/201 | 4 | 1 |
| fcstor009 | 194/201 | 6 | 1 |
| fcstor010 | 195/201 | 5 | 1 |
| fcstor011 | 193/201 | 7 | 1 |
| fcstor012 | 195/201 | 5 | 1 |
| fcstor013 | 195/201 | 5 | 1 |
| fcstor014 | 195/201 | 5 | 1 |
| fcstor015 | 196/201 | 4 | 1 |

Max EFS BUGS per host: 8 (concurrent_appends, concurrent_creates_same_dir, dir_deep_nesting, dir_deep_nesting_beyond_64, hardlink_rename_one_name, hardlink_terminal_ln)

## Limitations and remaining gates

Known skips are platform items (mmap). Failing names above are the remaining gates for their era; later gates (e.g. 20260927-033723, 20261002-151947) are clean.

## Evidence index

- `compare-fcstor007.txt` — b67c6a31d48a390e
- `compare-fcstor008.txt` — 87355ed65ac76f69
- `compare-fcstor009.txt` — 840ed1a12c6d27da
- `compare-fcstor010.txt` — 27db5999e5fd9043
- `compare-fcstor011.txt` — 94eb5d872563390b
- `compare-fcstor012.txt` — 0f0c2c2498b885d3
- `compare-fcstor013.txt` — 229c409d0dc398de
- `compare-fcstor014.txt` — f49d797bdc90c99b
- `compare-fcstor015.txt` — 87355ed65ac76f69
- `efs-fcstor007.tsv` — a729e94d957c98eb
- `efs-fcstor008.tsv` — f98fb99c302b2f9c
- `efs-fcstor009.tsv` — 313436355b7efb6f
- `efs-fcstor010.tsv` — 867ab52297ac1334
- `efs-fcstor011.tsv` — 789c0304e9ea9c4e
- `efs-fcstor012.tsv` — 3ab12eec46a4e817
- `efs-fcstor013.tsv` — 6c355ea15d82aff7
- `efs-fcstor014.tsv` — e54ed0e99ca7cbe8
- `efs-fcstor015.tsv` — 3c9bb54aa37752d9
- `xfs-baseline.tsv` — 2b7c71d7536a8325
