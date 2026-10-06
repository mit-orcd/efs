# EFS benchmark profiling harness — Oct 6 2026

Added `efs-bench --bench blake3`, sharing the existing standalone BLAKE3
microbenchmark. One-shot and streaming modes report SIMD/affinity, bytes/hashes
and throughput. Warm-up synchronization is atomic; timing includes final batch
completion. Strict finite duration/integer/size parsing, aligned allocation and
worker-creation cleanup replace unsafe old microbenchmark behavior.

`efs-bench.sh` drives a finite configurable local matrix: BLAKE3 sizes/threads,
buffered/direct data, inline/fixed/automatic writer pools and QD selection,
plus metadata. The local harness defaults to 256 MiB of fragment payload,
wrapping writes after filling each slot window; standalone local CLI retains
append-only writes without `--window`. Optional seed adds network, bounded store/read and remote meta.
Each case has an untraced baseline and separate perf rerun; optional strace is
a third run. It saves raw perf, flat/thread/caller/source-annotation reports,
matching binary/source archive, host/mount provenance, commands and atomic
progress records. Missing/empty/failed perf profiles fail visibly. Timeout or
interrupt stops only the owned process group and preserves evidence. New private
scratch children are removed without clearing caller-owned parents.

Validation:

- NUC Linux full `make -j8 test`: PASS, including six harness regressions and
  CLI/parser/BLAKE3 tests. [Full output](nuc-test.log).
- NUC real baseline-only local smoke: five cases PASS (BLAKE3 one-shot/stream,
  buffered/direct data and metadata). Additional inline/fixed/auto writer and QD1/16
  bounded data matrix: 12/12 PASS, 1 MiB fragment payload cap.
  [Matrix output](nuc-data-matrix.log). A one-slot window CLI regression proves
  reads use resident fragments after cumulative writes wrap.
- NUC has no perf package and sudo needs a password; no package or kernel-policy
  changes were made. Baseline-only mode was explicitly selected there.
- Gateway xefsgw private build, real cycles perf: five reduced cases PASS,
  0.5 s/phase, one BLAKE3 thread at 64 KiB, inline data QD1 and metadata.
  Raw perf, resolved symbols, caller/thread reports and C/assembly annotations
  were produced. BLAKE3 AVX-512 was the dominant hash profile.
- Optional separate strace: both 64 KiB BLAKE3 cases PASS, plus a final current
  harness rerun at 4 KiB with baseline/perf/strace all PASS.
- Final bounded gateway matrix: seven cases PASS, baseline/perf/strace each,
  0.3 s/phase, 1 MiB local payload working set, BLAKE3 modes, inline/two-thread
  buffered/direct data and metadata. Artifacts copied to
  `benchmark-profiles-20261006/gateway-bounded` in this chat workspace and kept
  at `/tmp/efs-bench-perf-bounded-20261006` on the gateway.
- The gateway's `/tmp` is tmpfs: its storage smoke proves execution, not physical
  disk performance. No new production performance claim or cluster rollout.
- Documentation 5/5 and diff whitespace gate PASS.

Remote RPC modes were matrix-validated and covered by existing build/tests;
this task did not run them against the live cluster. Full default-duration
performance sweeps remain to be run on the intended storage devices.

Raw gateway artifacts, including matching executables and perf data, were copied
to this chat workspace under `benchmark-profiles-20261006/gateway-perf` and
`gateway-strace`. They also remain at `/tmp/efs-bench-perf-smoke-20261006` and
`/tmp/efs-bench-perf-strace-smoke-20261006` on the gateway. The final 4 KiB run is
`/tmp/efs-bench-perf-final-20261006`. Do not delete these before reviewing profiles.

Usage and interpretation: [benchmark profiling harness](../../../docs/how-it-works/performance.md#benchmark-profiling-harness).
