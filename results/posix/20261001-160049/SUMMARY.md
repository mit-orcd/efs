# SUMMARY — results/posix/20261001-160049

POSIX correctness gate on the 9-host fcstor cluster (9 clients, per-host suite + XFS baseline comparison), 2026-10-01 16:00Z.

## Purpose and linked item

- Added by commit `dc6b0af19` ("W39 RDMA zero-copy send; read path R1-R5 (owned rdcache put, in-place …").
- Project history: (`results/posix/20261001-160049`).
- Project history: (`dep691`): 200/201 on all nine (`results/posix/20261001-160049`).

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

- `compare-fcstor007.txt` — 277fe100d42fb3de
- `compare-fcstor008.txt` — 277fe100d42fb3de
- `compare-fcstor009.txt` — 277fe100d42fb3de
- `compare-fcstor010.txt` — 277fe100d42fb3de
- `compare-fcstor011.txt` — 277fe100d42fb3de
- `compare-fcstor012.txt` — 277fe100d42fb3de
- `compare-fcstor013.txt` — 277fe100d42fb3de
- `compare-fcstor014.txt` — 277fe100d42fb3de
- `compare-fcstor015.txt` — 277fe100d42fb3de
- `efs-fcstor007.tsv` — 6239669b1c01dc9c
- `efs-fcstor008.tsv` — 4c2a078815f225fe
- `efs-fcstor009.tsv` — d0eddddaa8c80b90
- `efs-fcstor010.tsv` — 4d07ff811812f896
- `efs-fcstor011.tsv` — 848e1d3ac207e2ff
- `efs-fcstor012.tsv` — 8660b8a4026218a1
- `efs-fcstor013.tsv` — 3e487a8cc269c00d
- `efs-fcstor014.tsv` — df1fe10711b46640
- `efs-fcstor015.tsv` — 246983a785700f3c
- `xfs-baseline.tsv` — 37a48dd573505155
