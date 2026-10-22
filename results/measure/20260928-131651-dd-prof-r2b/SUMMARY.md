# SUMMARY — results/measure/20260928-131651-dd-prof-r2b

DD CPU profile, round r2b: clean 9-client 8 GiB dd+fsync with perf record —
2810.5 MiB/s aggregate.

## Purpose and linked item

- **W4** dd-wall profiling series (r2b), after the degenerate r2 round
  (`20260928-102121-dd-prof-r2`).

## Verdict

**PASS** (measurement completed): aggregate **2810.5 MiB/s** (9 × 8192 MiB /
slowest dd wall 26.2 s); per-client walls tight at 25.1–26.2 s.

## Source commit and build identity

`db2b88c4802a-dirty`; leaders g0=fcstor003, g2=fcstor006; record_s=70.

## Environment

9 clients fcstor007–015, 8 GiB dd+fsync per client; perf on fuse + efsd.

## Method

dd wall per client + 70 s perf record on the leaders; report-split timing;
per-client logs `dd-fcstor*.txt`.

## Results

| metric | value |
|---|---|
| aggregate | 2810.5 MiB/s |
| per-client dd wall | 25.05–26.23 s |
| report-split | 51 lines; pack 59.8 s, push 39.1 s, finish 24.1 s |
| fuse top symbol | blake3_hash_many_avx2 21.03 % |

## Limitations and remaining gates

Perf overhead included in walls; see r5b for the repeat.

## Evidence index

- `SUMMARY.txt` — timing + profile (run `shasum -a 256 SUMMARY.txt | cut -c1-16`
  here)
- `dd-fcstor*.txt`
