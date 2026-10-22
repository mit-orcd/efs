# SUMMARY — results/posix2/20261002-152031

POSIX correctness gate on the 9-host fcstor cluster (two-client remote run), 2026-10-02 15:20Z.

## Purpose and linked item

- Added by commit `33b04eec8` ("W50 closed (GC deletes were not stuck) and the D26 scan cursor resumes…").
- Project history: posix2 63/63 (`results/posix2/20261002-152031`), IO-500 9×4 debug

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
| fcstor008-fcstor009 | 63/63 | 0 | 0 |

## Limitations and remaining gates

Known skips are platform items (mmap). Failing names above are the remaining gates for their era; later gates (e.g. 20260927-033723, 20261002-151947) are clean.

## Evidence index

- `compare-fcstor008-fcstor009.txt` — 5e396ca8a4ed4032
- `efs-fcstor008-fcstor009.tsv` — cabfe7c62648d6eb
- `xfs-baseline.tsv` — f302161b0891bd3f
