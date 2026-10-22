# SUMMARY — results/posix2/20260927-190509

POSIX correctness gate on the 9-host fcstor cluster (two-client remote run), 2026-09-27 19:05Z.

## Purpose and linked item

- Added by commit `2dd77dac8` ("Compact by folding L0 when the file cap forces it, drop the shard lock…").
- Project history: (`results/posix2/20260927-190509`). 1/9/36 not measured
- Project history: those clients: **63/63** (`results/posix2/20260927-190509`, 77.7 s).

## Verdict

**PASS**: all hosts clean (63 passes total; only known skips: mmap unsupported / platform).

## Source commit and build identity

Not recorded in the run.

## Environment

9 clients fcstor007–015 (FUSE, per-host suites), suite `posix_2client.py (two-client visibility, 63 tests)`; XFS baseline `xfs-baseline.tsv` where present. Transport/build per the era's roll log (not recorded in the run).

## Method

Per-host run of `tests/posix/posix_2client.py --remote` on the mounted fs plus an XFS baseline, then `compare.py` per host (`compare-<host>.txt`).

## Results

| host | pass/total | fail | skip |
|---|---|---|---|
| fcstor007-fcstor008 | 63/63 | 0 | 0 |

## Limitations and remaining gates

Known skips are platform items (mmap). Failing names above are the remaining gates for their era; later gates (e.g. 20260927-033723, 20261002-151947) are clean.

## Evidence index

- `compare-fcstor007-fcstor008.txt` — 5e396ca8a4ed4032
- `efs-fcstor007-fcstor008.tsv` — 5041805aaab1ec30
- `xfs-baseline.tsv` — bf63868a04409086
