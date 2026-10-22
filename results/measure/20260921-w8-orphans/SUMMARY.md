# SUMMARY — results/measure/20260921-w8-orphans

W8 orphan-sweep regression: idle-cluster metadata latency collapsed ~10–20×
after build `9534e53`, tracked to the kv sweep.

## Purpose and linked item

- **W8** (posix storm) root-cause run: sweep regression on an idle cluster.
  Reference baseline: `results/perf/20260921-md-latency.txt` (Sep 21 02:43Z).

## Verdict

**FAIL** (regression demonstrated): `9534e53` shows mkdir med 99.5 ms,
create+1B+close 151.9 ms, append+close 201.9 ms, unlink 55.8 ms, rmdir 69.3 ms
vs the reference 7.2 / 6.7 / 9.0 / 1.8 / 6.4 ms — 10–20× worse. On this build
posix jobs=1 passed 200/201 but took 219 s (was 56 s); jobs=16 hit the 15 s
budget on 77 tests (`results/posix/20260921-211622`).

## Source commit and build identity

`9534e53` (recorded in `sweep-regression.txt`).

## Environment

Idle cluster, single client fcstor007, 21:28Z; `mdlat.py` micro-latency probe.

## Method

`mdlat.py`: 20 ops each in a fresh dir (mkdir, create+1B+close, append+close,
stat, unlink, rmdir); plus `kv_intents.txt` and `after-recovery.txt` state
dumps.

## Results

| op | med ms (9534e53) | med ms (reference) |
|---|---|---|
| mkdir | 99.5 | 7.2 |
| create+1B+close | 151.9 | 6.7 |
| append+close | 201.9 | 9.0 |
| stat | 0.5 | 0.4 |
| unlink | 55.8 | 1.8 |
| rmdir | 69.3 | 6.4 |

## Limitations and remaining gates

Pinpoints the sweep as the regression source; the fix is tracked in the W8
work item (later gates recover, e.g. posix/20260927-033723 200/201).

## Evidence index

- `sweep-regression.txt` — latency table + reference (run
  `shasum -a 256 sweep-regression.txt | cut -c1-16` in this dir)
- `kv_intents.txt`, `after-recovery.txt`
