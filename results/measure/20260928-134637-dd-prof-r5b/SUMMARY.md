# SUMMARY — results/measure/20260928-134637-dd-prof-r5b

DD CPU profile, round r5b: third clean 9-client dd+fsync profile —
2551.5 MiB/s aggregate, blake3 at 22.8 % on the fuse CPU.

## Purpose and linked item

- **W4** dd-wall profiling series (r5b), after r2b (`20260928-131651-dd-prof-r2b`).

## Verdict

**PASS** (measurement completed): aggregate **2551.5 MiB/s**; per-client walls
24.8–28.9 s.

## Source commit and build identity

`db2b88c4802a-dirty`; leaders g0=fcstor003, g2=fcstor004; record_s=70.

## Environment

9 clients fcstor007–015, 8 GiB dd+fsync per client; perf on fuse + efsd.

## Method

dd wall per client + 70 s perf record; report-split timing; per-client logs
`dd-fcstor*.txt`.

## Results

| metric | value |
|---|---|
| aggregate | 2551.5 MiB/s |
| per-client dd wall | 24.84–28.90 s |
| report-split | 101 lines; pack 35.4 s, push 28.2 s, finish 10.0 s |
| fuse top symbol | blake3_hash_many_avx2 22.84 % |

## Limitations and remaining gates

Consistent with r2b within run-to-run variance; the blake3 share is the
standing CPU finding.

## Evidence index

- `SUMMARY.txt` — timing + profile (run `shasum -a 256 SUMMARY.txt | cut -c1-16`
  here)
- `dd-fcstor*.txt`
