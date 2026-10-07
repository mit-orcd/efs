# Allocation and multi-location benchmark coverage — 2026-10-06

Raw write matrix pairs initial allocation with a physically allocated, populated,
flushed overwrite window. --preallocate uses posix_fallocate; unsupported/error
results fail rather than silently reverting to allocating writes. Preparation is
outside timing/perf, and every prepopulated block is checked after the run.
Allocation layout and preparation counts are recorded in metrics and comparisons.

With multiple --storage-root parents, each raw/data case runs on each root alone
and on all roots together. Raw combined cases require QD >= root count; engine
data retains production fragment/writer routing and its 1..N prefix ladder.
Metadata runs independently per location because its local KV is single-root.
Hash-only CPU cases are not replicated. Total QD and working set stay constant.
The manifest maps canonical paths to filesystem IDs/free space and retains
findmnt output. Aliases are rejected, shared-device roots warn, and
--require-distinct-devices requires >=2 different filesystem devices. Independent
physical devices still require checking the underlying mount/RAID/LVM topology.

## Validation

Isolated build on nuc_efs in /tmp/efs-bench-multiroot-20261006:
- efs-bench and efsd Linux builds PASS.
- Benchmark CLI regressions PASS: preallocated io/io-blake3 with two roots,
  allocation/preparation/verification counts and cleanup, invalid read mode,
  and real engine write/read across the 1/2-root routing ladder.
- 14 Python harness regressions PASS on Mac and Linux, including matrix pairing,
  root selections, QD coverage, alias/device checks, report failures and parallel
  reports. Device/alias validation occurs before creating the result directory.
- Short 32-case unprofiled harness run PASS using io/io-blake3/data/meta, two
  private roots, 4 KiB I/O, QD2, 128 KiB total window and 0.03-second phases.
  This exercises harness root selection and real engine routing. Both roots are
  on the same filesystem: this is functional validation, not device scaling.

No deployment or full performance workload was run. The full two-device matrix,
source profiles and comparisons remain for user testing. The default two-root
matrix has 502 cases when the CPU ladder has four levels; use a reduced size/QD/
writer ladder for the first pass. Existing deployment scripts transfer the wrapper
and Python helper/sources; no invocation change is required beyond supplying the
scratch parents. Registered-buffer/asynchronous I/O remains a future controlled
experiment, not a production backend change in this checkpoint.
