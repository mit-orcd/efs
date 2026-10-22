# SUMMARY — results/posix/20260927-123717

POSIX correctness gate on the 9-host fcstor cluster (9 clients, per-host suite + XFS baseline comparison), 2026-09-27 12:37Z.

## Purpose and linked item

- Added by commit `8e62ff12b` ("Stop a snapshot export from electing, and stop a client from publishin…").
- Project history: 200/201 on all nine (`results/posix/20260927-123717`, mmap SKIP
- Project history: 5. **Then, not before:** D14 (leader read lease) is a decision to ask for, and item 8 (libfuse thread growth) is a libfuse limit. The 9-host numbers to beat are 200/201 on all nine (Sep 27, `results/posix/20260927-123717

## Verdict

**FAIL**: 0 test failure(s) across hosts, up to 1 baseline-vs-target mismatches ("EFS BUGS") per host.

## Source commit and build identity

Not recorded in the run.

## Environment

9 clients fcstor007–015 (FUSE, per-host suites), suite `posix_suite.py (~201 tests, jobs=16 vs XFS baseline)`; XFS baseline `xfs-baseline.tsv` where present. Transport/build per the era's roll log (not recorded in the run).

## Method

Per-host run of `tests/posix/posix_suite.py --jobs 16` on the mounted fs plus an XFS baseline, then `compare.py` per host (`compare-<host>.txt`).

## Results

| host | pass/total | fail | skip |
|---|---|---|---|
| fcstor007 | 200/201 | 0 | 1 |
| fcstor008 | 200/201 | 0 | 1 |
| fcstor009 | 200/201 | 0 | 1 |
| fcstor010 | 200/201 | 0 | 1 |
| fcstor011 | 200/201 | 0 | 1 |
| fcstor012 | 200/201 | 0 | 1 |
| fcstor013 | 200/201 | 0 | 1 |
| fcstor014 | 200/201 | 0 | 1 |
| fcstor015 | 200/201 | 0 | 1 |

Max EFS BUGS per host: 1

## Limitations and remaining gates

Known skips are platform items (mmap). Failing names above are the remaining gates for their era; later gates (e.g. 20260927-033723, 20261002-151947) are clean.

## Evidence index

- `compare-fcstor007.txt` — a3915841912c14b9
- `compare-fcstor008.txt` — a3915841912c14b9
- `compare-fcstor009.txt` — a3915841912c14b9
- `compare-fcstor010.txt` — a3915841912c14b9
- `compare-fcstor011.txt` — a3915841912c14b9
- `compare-fcstor012.txt` — a3915841912c14b9
- `compare-fcstor013.txt` — a3915841912c14b9
- `compare-fcstor014.txt` — a3915841912c14b9
- `compare-fcstor015.txt` — a3915841912c14b9
- `efs-fcstor007.tsv` — 0e06e1148f420bd3
- `efs-fcstor008.tsv` — bf088c024f881bdb
- `efs-fcstor009.tsv` — 73f1759dc032af8e
- `efs-fcstor010.tsv` — 9693d97b778a5971
- `efs-fcstor011.tsv` — cff1e2fb274a5639
- `efs-fcstor012.tsv` — 29955ff0dbdf7920
- `efs-fcstor013.tsv` — be1694cf250ed112
- `efs-fcstor014.tsv` — 89fb3058e2cf8b0b
- `efs-fcstor015.tsv` — dffda9544ab7810c
- `xfs-baseline.tsv` — 77d704100c1ba55c
