# SUMMARY — results/posix/20260927-033723

POSIX correctness gate on the 9-host fcstor cluster (9 clients, per-host suite + XFS baseline comparison), 2026-09-27 03:37Z.

## Purpose and linked item

- Added by commit `718be2fb0` ("raft: truncate the log with a chunked snapshot file…").
- Project history: (`results/posix/20260927-033723`). W9's pin rules are on the nine
- Project history: 30.4–31.3 s (`results/posix/20260927-033723`). The failures that

## Verdict

**PASS**: all hosts clean (1800 passes total; only known skips: mmap unsupported / platform).

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

## Limitations and remaining gates

Known skips are platform items (mmap). Failing names above are the remaining gates for their era; later gates (e.g. 20260927-033723, 20261002-151947) are clean.

## Evidence index

- `compare-fcstor007.txt` — 2cc0e0068b16197f
- `compare-fcstor008.txt` — 2cc0e0068b16197f
- `compare-fcstor009.txt` — 2cc0e0068b16197f
- `compare-fcstor010.txt` — 2cc0e0068b16197f
- `compare-fcstor011.txt` — 2cc0e0068b16197f
- `compare-fcstor012.txt` — 2cc0e0068b16197f
- `compare-fcstor013.txt` — 2cc0e0068b16197f
- `compare-fcstor014.txt` — 2cc0e0068b16197f
- `compare-fcstor015.txt` — 2cc0e0068b16197f
- `efs-fcstor007.tsv` — 3ffb2c75159ae93a
- `efs-fcstor008.tsv` — 848f5256cd074f9d
- `efs-fcstor009.tsv` — 79d9ef0b086999f8
- `efs-fcstor010.tsv` — e6d67571c7d5748d
- `efs-fcstor011.tsv` — 20a883e747fd3fb7
- `efs-fcstor012.tsv` — 5e3a41a40749579c
- `efs-fcstor013.tsv` — 0815a55e5f293aa7
- `efs-fcstor014.tsv` — 3129014b4950b1b7
- `efs-fcstor015.tsv` — 4f4be0dd042817db
- `xfs-baseline.tsv` — 454eb30ac57e622a
