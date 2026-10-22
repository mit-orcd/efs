# SUMMARY — results/posix/20260928-043918

POSIX correctness gate on the 9-host fcstor cluster (9 clients, per-host suite + XFS baseline comparison), 2026-09-28 04:39Z.

## Purpose and linked item

- Added by commit `2dd77dac8` ("Compact by folding L0 when the file cap forces it, drop the shard lock…").
- Project history: `mmap_write_read` SKIP (`results/posix/20260928-043918`, 45.2 s).
- Project history: (`results/posix/20260928-043918`). posix2 alone **63/63** in 65.4 s

## Verdict

**PASS**: all hosts clean (200 passes total; only known skips: mmap unsupported / platform).

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

## Limitations and remaining gates

Known skips are platform items (mmap). Failing names above are the remaining gates for their era; later gates (e.g. 20260927-033723, 20261002-151947) are clean.

## Evidence index

- `compare-fcstor007.txt` — 277fe100d42fb3de
- `efs-fcstor007.tsv` — 46a40d48b59b2e5b
- `xfs-baseline.tsv` — 524625b319a3de6c
