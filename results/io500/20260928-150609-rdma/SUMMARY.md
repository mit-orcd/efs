# SUMMARY — results/io500/20260928-150609-rdma

Debug IO-500 9×4 (36 ranks, fcstor007–015) over **RDMA**, stonewall 1 s — every
phase completed; all scores INVALID only by the debug stonewall.

## Purpose and linked item

- Periodic IO-500 debug gate on the RDMA transport. Added by commit `2dd77dac8`
  ("Compact by folding L0 when the file cap forces it, drop the shard lock") —
  run pre-dates that commit's code; it is a same-day transport baseline.

## Verdict

**PASS** (debug completion): every phase completed with valid phases; the only
INVALID marks come from the 1 s stonewall. Total debug score: Bandwidth
1.414 GiB/s, IOPS 2.236 kiops, TOTAL 1.778 [INVALID].

## Source commit and build identity

Not recorded in the run (no deployed-version/identity files).

## Environment

9 hosts fcstor007–015, 4 ranks each, `tests/perf/io500/config-debug.ini`,
stonewall 1 s, `EFS_TRANSPORT=rdma`. Result dir
`/tmp/efs-mount/io500-results/2026.09.28-11.06.31/`.

## Method

`io500` standard driver, `config-debug.ini`; per-phase results in
`result.txt`, score in `score.txt`.

## Results

| phase | score | time s |
|---|---|---|
| ior-easy-write | 2.917 GiB/s | 22.0 |
| mdtest-easy-write | 3.418 kIOPS | 2.9 |
| ior-hard-write | 0.291 GiB/s | 72.8 |
| mdtest-hard-write | 0.757 kIOPS | 2.5 |
| ior-easy-read | 3.092 GiB/s | 15.9 |
| ior-hard-read | 1.525 GiB/s | 12.4 |
| mdtest-easy-stat | 3.248 kIOPS | 3.0 |
| mdtest-hard-stat | 5.683 kIOPS | 1.3 |
| mdtest-easy-delete | 2.223 kIOPS | 5.5 |
| mdtest-hard-delete | 0.538 kIOPS | 3.1 |
| mdtest-hard-read | 4.895 kIOPS | 1.4 |

## Limitations and remaining gates

Debug stonewall invalidates the score; reads same-mount (warm); build identity
not recorded.

## Evidence index

- `score.txt` — cd1706642deae1e9 (all-phase result lines)
- `result.txt` — e4b28be9d1d61ade
- `result_summary.txt` — a0a6fb74f29405c1
- `config.ini` — 4206a2247e4e39a9
- `launch.txt` — 8e33cf6ef9e7c946
