# Xorinox benchmark review — 2026-10-06 (SUMMARY)

## Purpose and linked item

Review of the external "xorinox" benchmark run
`logs/efs-bench-20261006T222653.471481Z` and handoff of harness fixes.
Added by commit `0c4114579 docs: record xorinox benchmark findings and
validation handoff`. Not tied to a numbered W/D item (unlinked; purpose
inferred from the review itself): validate a third-party raw-I/O/BLAKE3
benchmark and fix the harness's failure handling.

## Verdict

FAIL for the benchmark as-run: of 128 cases, 106 passed, 18 baseline
failures, 4 perf-workload failures (buffered QD64/256 with zero timed
operations — likely busy-wait start-gate starvation at 256 workers on 16
hardware threads). No evidence of storage corruption; reported
`File exists` was stale errno from scratch setup; failed-case rates
(e.g. 239.66/268.09 GiB/s) are invalid. Harness fixes were implemented but
only syntax/static checks ran — execution testing was handed to the user,
no speedup claimed.

## Source commit and build identity

Reviewed run: commit `023685d839443ab73a30c1218c3644bd886e622b`, binary
SHA256 `f5186c9ed9d6f6ba856ea57efa3d558d82fd81d26e99a5bfc8872aedb9774b26`.

## Environment

Host: Ryzen 9 7940HS, 8 cores / 16 hardware threads; local XFS NVMe. The
run selected raw I/O, I/O+BLAKE3 and CPU BLAKE3 only — it did not measure
the production storage engine or daemon.

## Method

Analysis of the recorded benchmark logs/profiles, then harness changes:
sleeping all-ready start gate replacing start spinning; removed
per-operation modulo, floating-point deadline arithmetic and two clock
calls; worker counters local until completion; latency redefined as the
complete operation cycle including bookkeeping/scheduling (compare
throughput across the change with that in mind); failures now preserve
actual error/phase, emit BENCH_FAIL, expose idle/min/max worker counts;
analysis rejects failed baselines, separates time/unresolved samples,
records affinity/perf policy. Validation handoff commands:

```sh
python3 tests/test_bench_cli.py
python3 tests/test_bench_profile.py
./efs-bench.sh --modes io,io-blake3,blake3 --storage-root /data1/maik/efs
```

## Results

Pure 1 MiB direct I/O (saturated ≈QD16; more workers add latency, not
throughput):

| QD | Write GiB/s | Read GiB/s | Write avg ms | Read avg ms |
|---|---:|---:|---:|---:|
| 16 | 6.276 | 6.740 | 2.488 | 2.317 |
| 64 | 6.214 | 6.735 | 10.034 | 9.257 |
| 256 | 6.250 | 6.722 | 39.637 | 36.842 |

Other findings: buffered 64 KiB QD1 read profile attributes ~25–28 % of
worker samples to deadline pointer/value reloads and 14 % near window
division — benchmark overhead, fixed in the harness. BLAKE3 already uses
AVX-512 (97–99 % of CPU-only samples); one-shot 1 MiB hashing 7.158 GiB/s
at 1 thread, 58.982 GiB/s at 16 threads — no hash change justified.
Passing buffered rates measure warm page cache, not NVMe.

## Limitations and remaining gates

- Review only; harness fixes unexecuted at review time — rerun owed
  (commands above); expect no idle workers in normal-duration cases, and
  failed cases must show their real reason and be excluded from throughput
  comparisons.
- Perf reports recorded `cycles:u` with unresolved vDSO/kernel addresses;
  those samples cannot be attributed to storage functions.

## Evidence index

- `REVIEW.md` (936de0f368be20f2) — the full review (findings, harness changes, handoff)
- Reviewed raw run: `logs/efs-bench-20261006T222653.471481Z` (outside this run dir)
