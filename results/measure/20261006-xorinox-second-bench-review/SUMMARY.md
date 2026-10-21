# Xorinox second benchmark/report review — 2026-10-06 (SUMMARY)

## Purpose and linked item

Follow-up review of the second xorinox benchmark run
`logs/efs-bench-20261006T230611.472138Z`, focused on perf-report recovery
and hot-path interpretation after the first review's harness fixes. Added by
commit `f743521d2 docs: review second xorinox benchmark and document perf
report recovery`. Unlinked to a numbered W/D item (inference: continuation
of the xorinox validation handoff from `20261006-xorinox-bench-review`).

## Verdict

PASS (with one invalid case): 127/128 cases pass. The remaining
io-blake3-write-65536-qd256-buffered perf rerun had 21 idle workers out of
256 despite the all-ready sleeping startup; its baseline (zero errors/idle)
is separately valid, and the rerun is kept invalid rather than accepted.
Python report-harness regressions pass; real perf report + annotations ran
successfully on three private copies. No workload was rerun; no production
optimization is justified from these profiles.

## Source commit and build identity

Reviewed run: commit `0c4114579dbda1a3458ce08a7e25a6c74f20fc57`, binary
SHA256 `7d2de1e0812b5ebde873df29e01e709078dade929c4878150d3f8dd79b020ad3`.

## Environment

Xorinox host (Ryzen 9 7940HS class per the first review; not restated
here). Modes: raw I/O, I/O+BLAKE3, CPU BLAKE3 only — no storage-engine or
daemon throughput conclusion follows. Original result directory owned by
`maik`; the `efs` SSH account cannot rewrite it.

## Method

Recovered the 128 perf.data report sets: the harness now writes
flat.txt/by_thread.txt/callers.txt (plus legacy .stdout fallbacks), moves
reporting after the workload matrix, runs bounded parallel report jobs
(`--report-jobs`, default 4) with progress, supports `--reports-only DIR`,
checks reporting permissions before launching workers, and fails validation
on missing/empty/failed reports — reports never turn a failed workload into
a pass. Verified with Python regressions and real perf runs on three
private copies (buffered read, CPU BLAKE3, previously skipped failed
workload).

## Results

| measurement | result |
|---|---|
| 64 KiB QD1 buffered read profile | `_copy_to_iter` 77.19 %, `copy_page_to_iter` 4.22 % — kernel/user copying dominates; 18.389 GiB/s is page cache, not NVMe |
| buffered write profile | `copy_page_from_iter_atomic` 74.34 % (15.609 GiB/s, same caveat) |
| 1 MiB direct QD16 / 64 / 256 write | 6.289 / 6.274 / 6.293 GiB/s (avg 2.483 / 9.940 / 39.405 ms) |
| 1 MiB direct QD16 / 64 / 256 read | 6.741 / 6.738 / 6.721 GiB/s (avg 2.317 / 9.253 / 36.826 ms) |
| CPU-only BLAKE3 1 MiB, 16 threads | 60.631 GiB/s; `blake3_hash_many_avx512` 98.13 % of samples |
| I/O+BLAKE3 1 MiB read QD16 direct | 6.739 GiB/s ≈ pure-I/O rate despite hashing at 77.71 % of CPU samples |
| Failed-workload profile (diagnostic) | AVX-512 hashing 74.11 %, kernel copying 14.72 % |

Conclusions: hashing is already SIMD-optimized; high CPU sample share does
not imply a throughput limit; kernel copying dominates hot-cache buffered
workloads, so metadata/daemon-lock changes would not help them.

## Limitations and remaining gates

- No workload rerun; the qd256-buffered rerun stays invalid.
- Next: run engine data/meta modes with the same report collection and
  compare real fragment-store/writer and KV/fsync hot paths against these
  raw ceilings before porting changes into efsd; compare throughput with
  tail latency, not just sample percentages.
- Regenerating all 128 report sets in the original directory requires the
  `maik` account.

## Evidence index

- `REVIEW.md` (cdadda10cac31b69) — the full review (report recovery, hot paths, verification)
- Reviewed raw run: `logs/efs-bench-20261006T230611.472138Z` (outside this run dir)
