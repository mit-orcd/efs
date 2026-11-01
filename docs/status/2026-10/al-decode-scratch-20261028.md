# AL — reads: decode scratch only for the path taken (2026-10-28)

Coordinator report: branch `agent/devel-al`, code `cbeb09621` (on devel
`9c4bfd171ccc5450ae852a2597351150e543730a`). Gate on intel2:
`TOTAL 546 FAIL 1 NOTRUN 11 STATUS FAIL (761s) OVERBUDGET 3` — the FAIL is the
untouched-base `test_b6_unavailable` slow-mode fsync EIO (K55, registered by
the AG/AJ gates; 3/3 deterministic on this devel). TSan 19/19 PASS. ASan on
this devel fails before any workload on clean and fixed trees alike
("efs-fuse: cannot establish allocator policy", 5 startup failures) —
pre-existing, needs an owner (K row suggested). Evidence: this page; raw data
intel2 `~/efs-artifacts/taskAL-20261028/`. NOTRUN: none for this task's scope.

**Verdict: FIXED (loopback, measured).** The packed branch no longer acquires
ordinary decode scratch at all, and in-place ordinary reads acquire one
fragment instead of three: retained decode scratch per mount drops
**17.3 → 8.0 MiB** (mixed, 32 readers), **16.0 → 8.0 MiB** (ordinary),
**5.3 → 4.0 MiB** (packed-only; the remainder is the unrelated 512 KiB
FUSE-read reply buffer). Read latencies stay inside the baseline spread and
every read is exact.

## Purpose and checklist item

Task AL of the AK–AM block (`docs/status/alpha-1.0-2plus1-checklist.md`, AL
checkbox): `src/client/read.c::efs_client_decode_placed_chunk_attempts` called
`decode_frag_scratch(3 * frag_len)` before knowing whether the mapping was
packed; the packed branch takes its own scratch and never touches the ordinary
fragment storage, and in-place ordinary reads (`2 * frag_len == chunk_size`)
need only parity in scratch. The cost is first allocation/growth and retained
TLS capacity per decoding thread, not a malloc per read.

## Tested commit and build identity

- Fix: `cbeb09621` on devel `9c4bfd171` (`agent/devel-al`).
- Baseline measurement build: clean devel `9c4bfd171` on intel2 (8 CPUs).
- Gate build: `efsd cbeb09621... (branch agent/devel-al, built 2026-10-29 on
  intel2)`; the measurement builds carried `EFS_BUILD_ID="unknown"` (plain
  `make`), commit identity from the checkout.

## Method

Existing `EFS_REPLY_BUFFER_TRACE=1` (no product patch for the measurement):
the trace logs every reply-buffer `new`/`grow`/`retire` with the per-owner
`read=` capacity, which is exactly the `decode_frag_scratch` TLS buffer.
Per run: a fresh 3-member loopback TCP cluster (packing on a short age), a
writer mount for the dataset and a fresh reader mount (cold client). 18
baseline runs: packed (2000 × 4 KiB files, packed before reading), ordinary
(16 × 8 MiB files), mixed, at 8 and 32 readers, 3 reps. Per-file read
latencies, client RSS and thread count from `/proc`, trace-derived first
allocations and retained bytes. Three after-runs with the fixed client at 32
readers.

Baseline (medians of 3; latencies are whole-file open/read/close):

| config | p50 | p95 | p99 | client RSS | decode scratch retained |
|---|---|---|---|---|---|
| packed, 8 readers | 8.39 ms | 13.18 ms | 16.89 ms | 26.1 MiB | 1.5 MiB (unused) |
| packed, 32 readers | 24.09 ms | 39.22 ms | 49.14 ms | 27.7 MiB | 1.3 MiB (unused) |
| ordinary, 8 readers | 126.6 ms | 138.7 ms | 138.7 ms | 87.1 MiB | 12.0 MiB |
| ordinary, 32 readers | 246.5 ms | 252.6 ms | 252.6 ms | 91.5 MiB | 12.0 MiB |
| mixed, 8 readers | 8.17 ms | 13.31 ms | 20.10 ms | 91.0 MiB | 13.3 MiB |
| mixed, 32 readers | 24.74 ms | 41.59 ms | 54.50 ms | 95.0 MiB | 13.3 MiB |

The decode-scratch column is `196608 × owners` (3 × 64 KiB fragments); a
separate 512 KiB × 8 FUSE-read reply buffer (`efs_fuse.c:6378`) accounts for
the remaining 4 MiB in every configuration and is not AL's mechanism.

After the fix (32 readers, one run each; 0 read errors):

| config | p50 | p95 | p99 | client RSS | decode scratch retained |
|---|---|---|---|---|---|
| mixed | 26.27 ms | 41.68 ms | 56.13 ms | 94.0 MiB | 4.0 MiB (64 × 64 KiB) |
| ordinary | 221.6 ms | 236.5 ms | 236.5 ms | 90.4 MiB | 4.0 MiB |
| packed | 23.58 ms | 37.77 ms | 53.06 ms | 28.5 MiB | 0 |

## The change

`src/client/read.c` (commit `cbeb09621`): the scratch acquisition moved below
the packed branch and the zero-hole branch, and is sized for the path —
`frag_len` when `2 * frag_len == chunk_size` (data fragments received straight
into the output halves, parity alone in scratch), `3 * frag_len` otherwise.
No behavioral change to any fetch, verification, decode or error path;
demand-read headroom and the AG bufpool bounds are untouched (this buffer is
TLS reply-buffer storage, not the pool).

Aliasing proof: `tests/test_erasure.c::test_roundtrip_inplace` encodes, then
decodes with data fragments aliased to the output halves and parity in
separate scratch for **every missing-fragment position** (D1, D2, P), at
partial, full and 1 MiB chunk sizes, matching the client's exact call shape.
`src/data/erasure.c::decode` reads only the two present fragments and writes
disjoint halves, so the in-place layout is safe by construction and now pinned
by the test.

## Gate and checks

- `make test-all` on intel2 (8 jobs, `scripts/make_test_summary.sh`):
  `TOTAL 546 FAIL 1 NOTRUN 11 STATUS FAIL (761s) OVERBUDGET 3 BUILD 30s`. The
  FAIL is `test_b6_unavailable.py` slow mode, the K55 base failure
  (deterministic on unmodified devel at the same commit, recorded by the
  AG/AJ gates on two hosts); nothing else fails.
- Targeted: `tests/test_erasure` (with the new in-place cases),
  `tests/test_packed_reader`, `tests/test_packed_segment`,
  `tests/test_read_errno.py` (failed reads, allocation ENOMEM),
  `tests/test_read_cache_reuse.py` — all PASS. Packed-only, ordinary and mixed
  after-runs read every file exact (0 errors); zero/hole and mixed-geometry
  coverage is in the gate (`test_b_geometry.py`, POSIX hole cases).
- TSan: `make tsan-check` PASS (19 run, 0 failed, 516 s).
- ASan: `make asan-check` FAILS **identically on clean devel and on the fix**:
  the ASan-built `efs-fuse` refuses to start ("cannot establish allocator
  policy"), so `env_removed`, `k36_recovery`, `k36_segment_recovery`,
  `k37_segment_rebuild` and `d4_progress` fail at startup (49 run, 5 failed,
  49–50 s). No sanitizer finding in any test log; this is a startup refusal,
  pre-existing on this devel (likely the wave-31 bounded-allocator policy
  meeting ASan's interposed allocator), not from AL. The clean-tree control
  is `asan-clean-devel.log` in the artifacts. **Recommended K row for the
  coordinator** (the F1 sanitizer gate cannot run its FUSE cases in this
  state); not registered here to keep K numbering contiguous with the
  in-flight K55 branch.

## Artifacts

intel2 `~/efs-artifacts/taskAL-20261028/`: `measure/` (18 baseline + 3 after
JSON/logs), `al_measure.py`, `al_run.sh`, `matrix.log`, `gate.log`,
`summary.txt`, `timings.tsv`, `fail-482.log` (K55), `asan.log`,
`asan-clean-devel.log`, `tsan.log`, `PRUNED.txt`, `SHA256SUMS` (manifest
sha256 `7b962ff293e0e6fbb762abca7f814cea165b1c1c2fb548fd085554711dbbb5dd`).
Per-run stores and both build trees were deleted after the runs; the
measurement reproduces from devel `9c4bfd171` plus the retained driver.
