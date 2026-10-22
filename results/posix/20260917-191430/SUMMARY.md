# SUMMARY — results/posix/20260917-191430

POSIX correctness gate on the 9-host fcstor cluster (9 clients, per-host suite + XFS baseline comparison), 2026-09-17 19:14Z.

## Purpose and linked item

- Added by commit `5e9e885d0` ("Drop the deleted engine's benchmark history, and write down what is mi…").
- Project history: 9-node `20260917-191430`: every host hit the 385 s python cap at

## Verdict

**FAIL**: 53 test failure(s) across hosts, up to 71 baseline-vs-target mismatches ("EFS BUGS") per host: concurrent_create_unlink_two_proc, concurrent_creates_same_dir, dir_deep_nesting, dir_deep_nesting_beyond_64, dir_many_files, dir_readdir_listing.

## Source commit and build identity

Not recorded in the run.

## Environment

9 clients fcstor007–015 (FUSE, per-host suites), suite `posix_suite.py (~201 tests, jobs=16 vs XFS baseline)`; XFS baseline `xfs-baseline.tsv` where present. Transport/build per the era's roll log (not recorded in the run).

## Method

Per-host run of `tests/posix/posix_suite.py --jobs 16` on the mounted fs plus an XFS baseline, then `compare.py` per host (`compare-<host>.txt`).

## Results

| host | pass/total | fail | skip |
|---|---|---|---|
| fcstor007 | 131/137 | 5 | 1 |
| fcstor008 | 133/140 | 6 | 1 |
| fcstor009 | 137/144 | 6 | 1 |
| fcstor010 | 126/131 | 5 | 0 |
| fcstor011 | 132/140 | 7 | 1 |
| fcstor012 | 133/139 | 5 | 1 |
| fcstor013 | 137/144 | 6 | 1 |
| fcstor014 | 132/140 | 7 | 1 |
| fcstor015 | 133/140 | 6 | 1 |

Max EFS BUGS per host: 71 (concurrent_create_unlink_two_proc, concurrent_creates_same_dir, dir_deep_nesting, dir_deep_nesting_beyond_64, dir_many_files, dir_readdir_listing)

## Limitations and remaining gates

Known skips are platform items (mmap). Failing names above are the remaining gates for their era; later gates (e.g. 20260927-033723, 20261002-151947) are clean.

## Evidence index

- `compare-fcstor007.txt` — 25a31ffd07083452
- `compare-fcstor008.txt` — 876cb799894730b6
- `compare-fcstor009.txt` — 377753603e79afdc
- `compare-fcstor010.txt` — 45b19ab1006788e8
- `compare-fcstor011.txt` — 4486b21ce1e248cf
- `compare-fcstor012.txt` — 6342eeeaf514bd13
- `compare-fcstor013.txt` — 440df4a20896629f
- `compare-fcstor014.txt` — 4486b21ce1e248cf
- `compare-fcstor015.txt` — 876cb799894730b6
- `efs-fcstor007.tsv` — dc0aeb0d70d551e5
- `efs-fcstor008.tsv` — 18682367e7f8ff73
- `efs-fcstor009.tsv` — 49d2b72ec0c6eeb8
- `efs-fcstor010.tsv` — 127c71c41e54f9e8
- `efs-fcstor011.tsv` — 80731de01107564e
- `efs-fcstor012.tsv` — b2aaa3a1cee1375f
- `efs-fcstor013.tsv` — bf63254d810a0e31
- `efs-fcstor014.tsv` — 960861d2aad1710c
- `efs-fcstor015.tsv` — 67fd5a215141017f
- `xfs-baseline.tsv` — 1f1f207b466afcb7
