# Writer investigation — Oct 7 checkpoint

The complete comparison is not finished. SSH and ping to xorinox (10.0.0.72)
stopped responding during the baseline batch, around 02:59 UTC. The last
retrieved checkpoint was 135/432 measurements, all PASS. The remote process may
still be running; check it before launching another batch. No cluster restart,
production deployment, filesystem tuning or writer-count change was performed.

## Implemented and checked

- Engine `--rw read|write|both`, with bounded read population outside timing and
  acknowledged perf control around measured operations only.
- `--full-paths` removes the prefix ladder for a combined-root comparison.
- Private worker release gates, idle-worker checks, actual writer count, and
  successful write counts per root.
- Opt-in writer diagnostics: admission, accepted-slot wait, service, return-to-
  caller delay, initial fallback mutex acquisition, blocking fallback count and
  peak submissions in flight. Production defaults leave diagnostics disabled.
- **Actual production fix:** inline writer mode previously returned before
  initializing root-selection state. Its new writes could select root zero
  even with multiple configured storage roots. Inline mode now initializes the
  path count and lifecycle locks before returning. New writes spread across
  roots; overwrites still reuse the existing root. The same source serves
  `efsd` and `efs-bench`.
- **Benchmark correctness fix:** bounded-window overwrites no longer claim a
  new fragment. They probe the existing root, avoiding misleading duplicate
  placement across roots.

Linux builds of both binaries passed. The Linux routing/lifecycle test passed
before its concurrency extension; the extended four-producer test passes
locally, including inline/pool spread, overwrite identity, job counters,
failed writes and failed pool startup. Profiler tests: 16 pass locally.
Linux phase/diagnostic CLI checks and four real DWARF isolation-preflight cases
passed. A short 30 ms full-QD smoke test exposed the new idle-worker gate;
backend smoke now uses explicit QD2 and a bounded window. The Linux smoke then
passed. Full Linux unit testing and the final expanded CLI test remain to run
when access returns. Documentation checks pass.

## Preliminary observations — not final conclusions

The initial isolated two-writer/QD16/one-root buffered diagnostic recorded
11,803 jobs: admission total 14,003,037 us (~1,186 us/job), accepted-slot delay
82,011 us (~7 us/job), service 1,796,863 us (~152 us/job), and caller resume
119,005 us (~10 us/job). This points primarily to waiting for writer capacity,
not CPU hashing during reads. It does not by itself prove a scheduler defect
or justify changing defaults. Root1 buffered QD64 partial uninstrumented samples
showed ~0.99 GiB/s with two writers versus ~4.16 GiB/s with automatic writers;
complete repeats and profiles are required before using this comparison.

Background QEMU guests were active. These are shared-host measurements, with
load/device provenance captured, not a dedicated-host maximum-performance claim.

## Running matrix and remaining work

`study-run.py` uses 64 KiB fragments, a fixed 64 MiB bounded total window,
QD1/16/64, inline/two/automatic writers, buffered/direct I/O, read/write
separately, and root1/root2/both: 108 configurations. It runs three randomized
uninstrumented baseline repeats per configuration, then 108 separate DWARF
profiles with writer diagnostics. Profiles exclude read population, worker
creation, cleanup and post-round latency sorting. The profiles have not yet
been retrieved; no final throughput/tail-latency finding is claimed.

After that batch, `device-check.py` is **prepared but not executed**. It measures
three randomized repeats of append+fsync, overwrite+fdatasync, and
create+rename+directory-fsync on each device; then direct raw 64 KiB reads and
preallocated writes at QD1/16/64, plus QD1 write+fdatasync. It also captures XFS
geometry/device models and short optional strace and scheduler diagnostics.
Those traced rates are not baseline performance measurements.

Still required: retrieve and assess all repeats/profiles; run the device and
wait diagnostics; assess variance and instrumented overhead; identify any
reproducible further bottleneck; validate any resulting change against both
throughput and tail latency, retaining durability. The two-writer capacity
limit alone is not grounds to rewrite the writer pool or change defaults.

## Resume locations

- Private source/build: `/tmp/efs-writer-study-20261007` on `xorinox_efs`.
- Running batch: `/tmp/efs-writer-study-results-final-20261007`.
- Progress log: `/tmp/efs-writer-study-run-final.log`.
- Driver: `/tmp/efs-writer-study-run.py`; launch was foreground over SSH with
  output redirected to the progress log. Check whether that process still lives.
- Rejected earlier batch (before placement fixes):
  `/tmp/efs-writer-study-results-20261007`; deliberately interrupted, never use
  it as matched evidence for the corrected multi-root workload.
- Device driver: `/tmp/efs-writer-device-check.py`; intended new output
  `/tmp/efs-writer-study-device-20261007`.
- Summary driver: `/tmp/efs-writer-study-summarize.py`; reads only the final batch.
- Remote smoke/profile/routing logs: `/tmp/efs-writer-study-smoke-final.log`,
  `/tmp/efs-writer-study-profile-tests.log`, `/tmp/efs-writer-routing-test.log`.

The scripts in this checkpoint record exact private paths for reproducibility;
they refuse existing output directories. If the old process exited, rerun only
missing stages using its saved manifest/results, or use a fresh output directory
and retain the interrupted results separately. Do not delete another process's
scratch directory or overwrite its recordings. Preserve the copied matching
binary with each perf dataset. The original local benchmark log was not modified
by this investigation.
