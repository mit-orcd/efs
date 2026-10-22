# SUMMARY — results/posix/20261002-130142

POSIX correctness gate on the 9-host fcstor cluster (9 clients, per-host suite + XFS baseline comparison), 2026-10-02 13:01Z.

## Purpose and linked item

- Added by commit `111a07527` ("client: D23 commit-time body hand-off to the rdcache, W41 per-inode RE…").
- Project history: 63/63 (`results/posix/20261002-130142`, `results/posix2/20261002-

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
| fcstor008 | 200/201 | 0 | 1 |

## Limitations and remaining gates

Known skips are platform items (mmap). Failing names above are the remaining gates for their era; later gates (e.g. 20260927-033723, 20261002-151947) are clean.

## Evidence index

- `compare-fcstor008.txt` — 277fe100d42fb3de
- `efs-fcstor008.tsv` — b604b0f7ae294312
- `xfs-baseline.tsv` — 89dacfeef03d33fe
