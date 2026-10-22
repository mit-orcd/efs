# SUMMARY — results/posix/20260926-0330-diag

POSIX correctness gate on the 9-host fcstor cluster (9 clients, per-host suite + XFS baseline comparison), 2026-09-26 03:30Z.

## Purpose and linked item

- Added by commit `2dd77dac8` ("Compact by folding L0 when the file cap forces it, drop the shard lock…").
- Project history: (`results/posix/20260926-0330-diag`: fcstor004 `apply_max=1665075us`,

## Verdict

**FAIL**: 21 test failure(s) across hosts.

## Source commit and build identity

Not recorded in the run.

## Environment

9 clients fcstor007–015 (FUSE, per-host suites), suite `posix_suite.py (~201 tests, jobs=16 vs XFS baseline)`; XFS baseline `local-baseline.tsv` where present. Transport/build per the era's roll log (not recorded in the run).

## Method

Per-host run of `tests/posix/posix_suite.py --jobs 16` on the mounted fs plus an XFS baseline, then `compare.py` per host (`compare-<host>.txt`).

## Results

| host | pass/total | fail | skip |
|---|---|---|---|
| fcstor007 | 196/201 | 4 | 1 |
| fcstor008 | 196/201 | 4 | 1 |
| fcstor009 | 198/201 | 2 | 1 |
| fcstor010 | 199/201 | 1 | 1 |
| fcstor011 | 199/201 | 1 | 1 |
| fcstor012 | 197/201 | 3 | 1 |
| fcstor013 | 197/201 | 3 | 1 |
| fcstor014 | 199/201 | 1 | 1 |
| fcstor015 | 198/201 | 2 | 1 |

## Limitations and remaining gates

Known skips are platform items (mmap). Failing names above are the remaining gates for their era; later gates (e.g. 20260927-033723, 20261002-151947) are clean.

## Evidence index

- `efs-fcstor007.tsv` — 3dd64b30bf6985c4
- `efs-fcstor008.tsv` — 9848a4c37d16b924
- `efs-fcstor009.tsv` — 117a36c79dabcb57
- `efs-fcstor010.tsv` — b382c58a5f18c51b
- `efs-fcstor011.tsv` — 2163ba00d5ca64ce
- `efs-fcstor012.tsv` — 4ecc0600fa5b86c7
- `efs-fcstor013.tsv` — 37141848f82f3702
- `efs-fcstor014.tsv` — ce68110bab62fdb1
- `efs-fcstor015.tsv` — 06cb5bcf6346b017
- `efsd-fcstor003.txt` — 13433687d01bcfc8
- `efsd-fcstor004.txt` — 8ab841d8ef6f5ada
- `efsd-fcstor005.txt` — 7c0dfd0bc3b23ce5
- `efsd-fcstor006.txt` — f485d5f3aceb4b06
- `fuse-fcstor007.txt` — ad75ec129549b0f5
- `logstart-fcstor003.txt` — feed82ae06377179
- `logstart-fcstor004.txt` — c360bddeac14b171
- `logstart-fcstor005.txt` — 33ef19e2c2c06e85
- `logstart-fcstor006.txt` — 1d64fbd7a58b7966
- `run-fcstor007.txt` — d0752acc18f1fe84
- `run-fcstor008.txt` — 88d8eec5d5601510
- `run-fcstor009.txt` — 3e9ce9c63e706d6c
- `run-fcstor010.txt` — 411aaa2c53efbe28
- `run-fcstor011.txt` — 99ae8732de17ed6b
- `run-fcstor012.txt` — 70b0c9e5013b31d3
- `run-fcstor013.txt` — 6c145da61df7d56f
- `run-fcstor014.txt` — 169387c45a2b4ebe
- `run-fcstor015.txt` — 47e7ec4d6b657e88
