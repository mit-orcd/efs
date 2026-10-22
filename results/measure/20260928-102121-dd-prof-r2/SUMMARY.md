# SUMMARY — results/measure/20260928-102121-dd-prof-r2

DD CPU profile, round r2: 9-client dd wall collapsed (per-client 13.7 s) —
recorded as INVALID aggregate because the workload degenerated.

## Purpose and linked item

- **W4** dd-wall profiling series (r2) feeding the blake3/REPORT cost analysis;
  siblings r2b (`20260928-131651-dd-prof-r2b`), r5b (`20260928-134637-dd-prof-r5b`).

## Verdict

**FAIL** (measurement invalid): `aggregate_MiB_s=INVALID` — slowest=fastest=
13.660 s for all 9 clients means the dd payload degenerated (ssh wall 150 s vs
76–78 s for the r2b/r5b siblings), so the rate is not quotable. Server REPORT
timing and client CPU profile were still captured: blake3_hash_many_avx2 tops
efs-fuse at 21.55 %.

## Source commit and build identity

`db2b88c4802a-dirty`; leaders g0=fcstor003, g2=fcstor004; record_s=70.

## Environment

9 clients fcstor007–015, 8 GiB dd+fsync per client; perf record on fuse + efsd.

## Method

dd wall per client + perf record (70 s) on leaders; report-split timing from
efsd logs; per-client logs `dd-fcstor*.txt`.

## Results

| metric | value |
|---|---|
| aggregate | INVALID (degenerate) |
| per-client dd wall | 13.660 s (all) |
| report-split | 961 lines; pack 13.5 s, push 24.8 s, finish 9.5 s |
| fuse top symbol | blake3_hash_many_avx2 21.55 % |

## Limitations and remaining gates

Use r2b/r5b for the quotable profile; this round stands as the invalid-marker
example.

## Evidence index

- `SUMMARY.txt` — timing + profile (run `shasum -a 256 SUMMARY.txt | cut -c1-16`
  here)
- `dd-fcstor*.txt`
