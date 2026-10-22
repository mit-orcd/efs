# SUMMARY — results/posix2/20261001-073048

POSIX correctness gate on the 9-host fcstor cluster (two-client remote run), 2026-10-01 07:30Z.

## Purpose and linked item

- Added by commit `7d93f2254` ("docs: Oct 1 review — prune stale docs and results, README as quick gui…").
- Project history: posix2 62/63 then 63/63 (`results/posix2/20261001-073048`,
- Project history: **Oct 1 07:45Z: cluster UP fresh (wipe 06:37Z → mkfs 07:18Z), posix suites run; two bugs fixed on the way, two follow-ups open.** Servers `4b2844487831-dirty` on fcstor003–006 (`restart408`, no recorders), g0 term 5 lead

## Verdict

**FAIL**: 1 test failure(s) across hosts, up to 1 baseline-vs-target mismatches ("EFS BUGS") per host: peer_rename_vs_unlink_src.

## Source commit and build identity

Not recorded in the run.

## Environment

9 clients fcstor007–015 (FUSE, per-host suites), suite `posix_2client.py (two-client visibility, 63 tests)`; XFS baseline `xfs-baseline.tsv` where present. Transport/build per the era's roll log (not recorded in the run).

## Method

Per-host run of `tests/posix/posix_2client.py --remote` on the mounted fs plus an XFS baseline, then `compare.py` per host (`compare-<host>.txt`).

## Results

| host | pass/total | fail | skip |
|---|---|---|---|
| fcstor008-fcstor009 | 62/63 | 1 | 0 |

Max EFS BUGS per host: 1 (peer_rename_vs_unlink_src)

## Limitations and remaining gates

Known skips are platform items (mmap). Failing names above are the remaining gates for their era; later gates (e.g. 20260927-033723, 20261002-151947) are clean.

## Evidence index

- `compare-fcstor008-fcstor009.txt` — 46238f0b7e4c2171
- `efs-fcstor008-fcstor009.tsv` — db3f0c7bba0e7ab9
- `xfs-baseline.tsv` — b234ca9c115c50cd
