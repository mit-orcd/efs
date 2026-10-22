# SUMMARY — results/io500/20260920-debug-9x4

Debug IO-500 (1 s stonewall), 9 clients × 4 ranks = 36, fcstor007–015, TCP
(19810). First 9×4 run with every phase valid for correctness — the W6 gate.

## Purpose and linked item

- **W6** (IO-500 correctness on the write path). Added by commit `cc828d8ce`
  ("W6: 9x4 IO-500 debug with 0 read-verify errors after dup-ino fix; rule …").
- Root cause fixed by this run's build: duplicate ino allocation on concurrent
  CREATE (see `results/20260920-easy-dupino/NOTE.txt`).

## Verdict

**PASS** (debug gate): `ior-easy-read` and `ior-hard-read` both **0
verification errors** (Sep 19 -fixed run: 76108 / 4314, both [INVALID]); every
unlink succeeded (Sep 19: 27 of 36 easy files undeletable). Score remains
INVALID only because the debug stonewall is 1 s.

## Source commit and build identity

efsd `708b350` (dup-ino fix + lane-sweep scan-full fix), efs-fuse `a9e94a6`
(recorded in NOTE.txt).

## Environment

9 clients fcstor007–015, 4 ranks each (36 total), TCP 19810, debug ini
`tests/perf/io500/config-debug.ini`, stonewall 1 s. Run id 2026.09.20-00.10.42.

## Method

`io500` standard driver, `config-debug.ini`; driver log `driver.log`; per-phase
IOR/mdtest output alongside; result files `result.txt`, `result_summary.txt`.

## Results

| phase | Sep 18 9×4 | Sep 19 -fixed | this run |
|---|---|---|---|
| ior-easy-write GiB/s | 0.415 | 0.567 | 0.814 |
| mdtest-easy-write kIOPS | 0.0285 | 0.0577 | 0.0504 |

Correctness: 0 read-verify errors both read phases; 0 unlink failures.

## Limitations and remaining gates

Debug stonewall (1 s) → score not comparable to list runs; TCP (pre-RDMA);
reads same-mount.

## Evidence index

- `NOTE.txt` — 8a5dfa2ed9886263 (comparison table, dup-ino analysis)
- `config.ini` — 4206a2247e4e39a9
- `result.txt` — 23cd797843b517b5
- `result_summary.txt` — bf22810591742b99
