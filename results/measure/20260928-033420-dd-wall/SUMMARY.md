# SUMMARY — results/measure/20260928-033420-dd-wall

9-client 8 GiB dd+fsync wall-time gate (own file per client), with server-side
REPORT timing attribution for the fsync tail.

## Purpose and linked item

- **W4/W3** perf gates — dd wall after the Sep 18 numbers (639/251/202 MiB/s at
  1/4/9 clients). Referenced by the 20260928-034049 posix gate and the
  samedir-rate run.

## Verdict

**PASS** (measurement completed): 9 clients, aggregate **1326.6 MiB/s**,
ssh wall 59.1 s, per-client dd wall 28.6–55.6 s; sum of per-client rates
1967 MiB/s = 3.01 % of ceiling.

## Source commit and build identity

`db2b88c4802a-dirty` (recorded in SUMMARY.txt).

## Environment

9 clients fcstor007–015, 8 GiB per client, own files; server REPORT timing via
efsd report-split lines.

## Method

dd+fsync per client (flush in the clock); agg = clients×8 GiB / slowest dd
wall; sum_rates = Σ per-client 8 GiB/wall. Perf captures `perf-ddwall-*.txt`
(PERF=1). Raw per-client logs `dd-*-*.txt`.

## Results

| metric | value |
|---|---|
| aggregate | 1326.6 MiB/s |
| ssh wall | 59.06 s |
| per-client dd wall | 28.55–55.57 s |
| sum of rates | 1967.4 MiB/s (3.01 % of ceiling) |

REPORT timing (9 clients): report-split 108 lines — pack 42.1 s total
(max 2978 ms), push 32.7 s (max 4175 ms), finish 16.9 s (max 1276 ms).

## Limitations and remaining gates

ssh_wall includes ~4 s ssh+check overhead; the REPORT tail is the documented
fsync cost center.

## Evidence index

- `SUMMARY.txt` — wall table + REPORT timing (run `shasum -a 256 SUMMARY.txt |
  cut -c1-16` here)
- `perf-ddwall-*.txt`, `dd-*-*.txt`
