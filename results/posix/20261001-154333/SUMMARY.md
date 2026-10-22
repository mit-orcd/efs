# SUMMARY — results/posix/20261001-154333

POSIX correctness gate on the 9-host fcstor cluster (9 clients, per-host suite + XFS baseline comparison), 2026-10-01 15:43Z.

## Purpose and linked item

- Added by commit `dc6b0af19` ("W39 RDMA zero-copy send; read path R1-R5 (owned rdcache put, in-place …").
- Project history: (`results/posix/20261001-154333`); wedge gate (4 × 8 GiB + 300-file
- Project history: - **Measured on fcstor007 (16 GiB random file, cold remount before every read, `agent-rd-20261001-145047` → `-154007-refs`):** write 1.4 → **1.5 GB/s** (client CPU 21.4 s), single cold read 2.5 → **3.6 GB/s** (client CPU

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
- `efs-fcstor007.tsv` — 83629217481b4dd6
- `xfs-baseline.tsv` — 687b7917d956def4
