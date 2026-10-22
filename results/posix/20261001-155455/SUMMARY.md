# SUMMARY — results/posix/20261001-155455

POSIX correctness gate on the 9-host fcstor cluster (9 clients, per-host suite + XFS baseline comparison), 2026-10-01 15:54Z.

## Purpose and linked item

- Added by commit `dc6b0af19` ("W39 RDMA zero-copy send; read path R1-R5 (owned rdcache put, in-place …").
- Project history: (`results/posix/20261001-155455`) — the same one-host signature as
- Project history: - **Client, deployed to fcstor003–015 (`dep680`) and fstor007's `/tmp/efs` (the user's running efs-fuse is untouched):** (1) **W39** — `efs_rdma_zc_region_add` registers the bufpool slabs lazily per HCA (`zc_lkey`, `ibv_

## Verdict

**FAIL**: 1 test failure(s) across hosts, up to 1 baseline-vs-target mismatches ("EFS BUGS") per host: dir_deep_nesting.

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
| fcstor009 | 199/201 | 1 | 1 |
| fcstor010 | 200/201 | 0 | 1 |
| fcstor011 | 200/201 | 0 | 1 |
| fcstor012 | 200/201 | 0 | 1 |
| fcstor013 | 200/201 | 0 | 1 |
| fcstor014 | 200/201 | 0 | 1 |
| fcstor015 | 200/201 | 0 | 1 |

Max EFS BUGS per host: 1 (dir_deep_nesting)

## Limitations and remaining gates

Known skips are platform items (mmap). Failing names above are the remaining gates for their era; later gates (e.g. 20260927-033723, 20261002-151947) are clean.

## Evidence index

- `compare-fcstor007.txt` — 277fe100d42fb3de
- `compare-fcstor008.txt` — 277fe100d42fb3de
- `compare-fcstor009.txt` — b1e81083ac67bd21
- `compare-fcstor010.txt` — 277fe100d42fb3de
- `compare-fcstor011.txt` — 277fe100d42fb3de
- `compare-fcstor012.txt` — 277fe100d42fb3de
- `compare-fcstor013.txt` — 277fe100d42fb3de
- `compare-fcstor014.txt` — 277fe100d42fb3de
- `compare-fcstor015.txt` — 277fe100d42fb3de
- `efs-fcstor007.tsv` — 88d045807f9cfadc
- `efs-fcstor008.tsv` — f045dd32bc167465
- `efs-fcstor009.tsv` — 8a12bcdebed1faae
- `efs-fcstor010.tsv` — e997e15517d1b51b
- `efs-fcstor011.tsv` — be8a5031bd6942d3
- `efs-fcstor012.tsv` — 4319b092558adb37
- `efs-fcstor013.tsv` — 34673f58f3250f0c
- `efs-fcstor014.tsv` — 00d51c6eb50d091f
- `efs-fcstor015.tsv` — 0695f7aacd1dd619
- `xfs-baseline.tsv` — c4969d11da0d37e1
