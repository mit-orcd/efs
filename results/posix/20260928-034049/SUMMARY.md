# SUMMARY — results/posix/20260928-034049

POSIX correctness gate on the 9-host fcstor cluster (9 clients, per-host suite + XFS baseline comparison), 2026-09-28 03:40Z.

## Purpose and linked item

- Added by commit `2dd77dac8` ("Compact by folding L0 when the file cap forces it, drop the shard lock…").
- Project history: (`results/posix/20260928-034049`), posix2 63/63, morning
- Project history: on all nine, 56.7–59.1 s (`results/posix/20260928-034049`); posix2

## Verdict

**PASS**: all hosts clean (1791 passes total; only known skips: mmap unsupported / platform).

## Source commit and build identity

Not recorded in the run.

## Environment

9 clients fcstor007–015 (FUSE, per-host suites), suite `posix_suite.py (~201 tests, jobs=16 vs XFS baseline)`; XFS baseline `xfs-baseline.tsv` where present. Transport/build per the era's roll log (not recorded in the run).

## Method

Per-host run of `tests/posix/posix_suite.py --jobs 16` on the mounted fs plus an XFS baseline, then `compare.py` per host (`compare-<host>.txt`).

## Results

| host | pass/total | fail | skip |
|---|---|---|---|
| fcstor007 | 199/201 | 0 | 2 |
| fcstor008 | 199/201 | 0 | 2 |
| fcstor009 | 199/201 | 0 | 2 |
| fcstor010 | 199/201 | 0 | 2 |
| fcstor011 | 199/201 | 0 | 2 |
| fcstor012 | 199/201 | 0 | 2 |
| fcstor013 | 199/201 | 0 | 2 |
| fcstor014 | 199/201 | 0 | 2 |
| fcstor015 | 199/201 | 0 | 2 |

## Limitations and remaining gates

Known skips are platform items (mmap). Failing names above are the remaining gates for their era; later gates (e.g. 20260927-033723, 20261002-151947) are clean.

## Evidence index

- `compare-fcstor007.txt` — 5c70c99cced0058a
- `compare-fcstor008.txt` — 3f2f3fd00a973446
- `compare-fcstor009.txt` — 7ecb6eb29f13f93d
- `compare-fcstor010.txt` — e68bf3a328c5b339
- `compare-fcstor011.txt` — f16c3a0e669b3c4c
- `compare-fcstor012.txt` — 9c974f7239da9fce
- `compare-fcstor013.txt` — 79382a6b45172197
- `compare-fcstor014.txt` — 223885bd1dc1fee6
- `compare-fcstor015.txt` — f5f95bbc7ba11385
- `efs-fcstor007.tsv` — 0422e6490ed22ea8
- `efs-fcstor008.tsv` — c8fa9fa801830329
- `efs-fcstor009.tsv` — 1b8de3a321cccd01
- `efs-fcstor010.tsv` — 10961f4e5adb0429
- `efs-fcstor011.tsv` — 601569992958356a
- `efs-fcstor012.tsv` — 91742644e0d7838c
- `efs-fcstor013.tsv` — 8a0f30ddc2e912ef
- `efs-fcstor014.tsv` — 8a6eca01dd6fc8cf
- `efs-fcstor015.tsv` — d42bb51c6a39d980
- `xfs-baseline.tsv` — 1a55bd7908354f3f
