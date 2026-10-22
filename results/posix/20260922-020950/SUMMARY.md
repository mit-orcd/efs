# SUMMARY — results/posix/20260922-020950

POSIX correctness gate on the 9-host fcstor cluster (9 clients, per-host suite + XFS baseline comparison), 2026-09-22 02:09Z.

## Purpose and linked item

- Added by commit `aa968574a` ("docs: W8 9-host suite at 191-195/201 on all nine; what remains and why…").
- Project history: (`results/posix/20260922-020950`). What failed on those older runs: the six
- Project history: ~62 s** (`results/posix/20260922-020950`, timeline

## Verdict

**FAIL**: 60 test failure(s) across hosts, up to 10 baseline-vs-target mismatches ("EFS BUGS") per host: concurrent_appends, concurrent_create_unlink_two_proc, concurrent_creates_same_dir, concurrent_write_and_readdir, dir_deep_nesting, dir_deep_nesting_beyond_64.

## Source commit and build identity

Not recorded in the run.

## Environment

9 clients fcstor007–015 (FUSE, per-host suites), suite `posix_suite.py (~201 tests, jobs=16 vs XFS baseline)`; XFS baseline `xfs-baseline.tsv` where present. Transport/build per the era's roll log (not recorded in the run).

## Method

Per-host run of `tests/posix/posix_suite.py --jobs 16` on the mounted fs plus an XFS baseline, then `compare.py` per host (`compare-<host>.txt`).

## Results

| host | pass/total | fail | skip |
|---|---|---|---|
| fcstor007 | 195/201 | 5 | 1 |
| fcstor008 | 193/201 | 7 | 1 |
| fcstor009 | 193/201 | 7 | 1 |
| fcstor010 | 193/201 | 7 | 1 |
| fcstor011 | 192/201 | 8 | 1 |
| fcstor012 | 191/201 | 9 | 1 |
| fcstor013 | 194/201 | 6 | 1 |
| fcstor014 | 194/201 | 6 | 1 |
| fcstor015 | 195/201 | 5 | 1 |

Max EFS BUGS per host: 10 (concurrent_appends, concurrent_create_unlink_two_proc, concurrent_creates_same_dir, concurrent_write_and_readdir, dir_deep_nesting, dir_deep_nesting_beyond_64)

## Limitations and remaining gates

Known skips are platform items (mmap). Failing names above are the remaining gates for their era; later gates (e.g. 20260927-033723, 20261002-151947) are clean.

## Evidence index

- `compare-fcstor007.txt` — 8e776110a6c35b8f
- `compare-fcstor008.txt` — 09386023b6f72434
- `compare-fcstor009.txt` — 09386023b6f72434
- `compare-fcstor010.txt` — 74a844059f817617
- `compare-fcstor011.txt` — 13ae3255778c2455
- `compare-fcstor012.txt` — b2499e006853467e
- `compare-fcstor013.txt` — 0d25f3d878c86a92
- `compare-fcstor014.txt` — 0d25f3d878c86a92
- `compare-fcstor015.txt` — 98d2ddb451ace1cf
- `efs-fcstor007.tsv` — f2ee7eb430302349
- `efs-fcstor008.tsv` — 4ec6756483795d7f
- `efs-fcstor009.tsv` — 68f0ac4d755bf380
- `efs-fcstor010.tsv` — 6049c13d44c2751e
- `efs-fcstor011.tsv` — 04b3d8a630587670
- `efs-fcstor012.tsv` — 730fecc22c00c749
- `efs-fcstor013.tsv` — 96a2716a58af4674
- `efs-fcstor014.tsv` — fe1983f34362af2f
- `efs-fcstor015.tsv` — 00a352472da25f49
- `xfs-baseline.tsv` — 7b85b40eca221e7e
