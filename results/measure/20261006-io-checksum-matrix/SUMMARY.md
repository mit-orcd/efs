# Parallel I/O and checksum isolation — Oct 6 2026

Added raw synchronous parallel workers in `efs-bench --bench io` and
`io-blake3`, with separate read/write cases, configurable 4 KiB-aligned sizes,
buffered/direct I/O, bounded per-worker windows and optional per-write fdatasync.
CPU-only BLAKE3 remains a separate mode. Raw I/O does not change production
checksum policy. Preparation and post-run integrity checks are outside timing;
perf FIFO control excludes those phases from raw I/O CPU profiles.

Validation:

- NUC full `make -j8 test`: PASS. Includes seven harness regression tests and
  raw read/write CLI cases, cleanup, nonempty-root refusal and invalid inputs.
- NUC three-way baseline matrix: 26/26 PASS, 4K/64K/1M, QD2,
  buffered/direct raw reads/writes, both checksum policies, CPU-only BLAKE3.
- Gateway actual cycles profiles: eight/eight raw cases PASS at 64K/QD2,
  buffered/direct reads/writes, with resolved C annotations and caller reports.
  Every pure-I/O profile has no BLAKE3 samples; every combined profile has them.
  Artifacts: `benchmark-profiles-20261006/gateway-io` in the chat workspace,
  also `/tmp/efs-io-perf-20261006` on xefsgw. Gateway /tmp is tmpfs: execution
  validation only, not physical-storage performance evidence.
- Cluster deployment audit found missing shell wrapper and matching C source
  on gateways/test clients. Shipping now includes both and BLAKE3 assembly;
  a real rsync regression checks executable mode, source filtering, preservation
  of unrelated files and failure on missing inputs. Existing script tests pass.
- NUC deployment already copies the full source tree. No stale `efsd --bench`
  script callers found under cluster/devops. No live deployment this turn.

`efsd` has no local benchmark runner. The network-ceiling BENCH_PUT discard RPC
remains in its handler for the separate remote network test.
