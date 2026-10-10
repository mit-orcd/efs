# W93/W97 late-hour read and pressure investigation

NUC only, October 9 22:44 EDT / October 10 02:44 UTC start. Existing service
stores preserved, TCP/direct I/O, command service disabled.

## Source and builds

* Idle off/on/off and separate perf/strace: installed `ca810bac` overlap candidate.
* Credit-order candidate: `ea23775b-read-pressure` (source `ea23775b`).
* Confirmed-absence recovery fix and all subsequent same-binary comparisons:
  `437b7890-read-pressure` (source `437b7890`), built 03:08:18 UTC.
* Compiler/core verdicts and installed versions/checksums are retained separately
  for both candidates. Build and profiling do not overlap timed read/rate cells.

## Evidence interpretation

`off1/on1/off2` use native open/pread/close, both fixed old probe sets in all
cases, warmed paths and phase samples. `perf-read/strace-read` are diagnostic
only; their timings are not performance acceptance data. Raw perf remains on
NUC at `/data1/efs/w93-read-pressure-hour-20261009/read-perf.data`, checksum
in `perf-sha256.txt`; decoded report and syscall summaries are local.

Initial POSIX passes, but `peer.log/peer.tsv` preserve a 63/64 append/truncate
EIO failure. `append-repro` retains 25 isolated passes on the initial candidate.
`peer-failed-mnt*.log` preserve the stale chunk-map decode diagnostic. The
negative actual-source regression fails on the old helper, while corrected
captured-view tests and ASan/UBSan pass. Corrected full suites are recorded in
`posix-fixed` (212/212, 63.2 seconds), `peer-fixed` (64/64, 169.9 seconds).
Repeated corrected append gates retain separate logs and TSVs.

`late/early/late2` are 240/240/180-second same-build pressure controls.
The reverse control is shortened after a false 30-second startup timeout;
`common-180.json` compares the common initial interval. Failed startup and
bounded idempotent retry evidence are retained separately. Only
EARLY_RETIRE differs; FLOW/NOOP/OVERLAP=1, GROUP_DRAIN=0, OBS=1. Fixed Python
read probes are identical across cells. Original failed logs are never replaced
by reruns. `balanced-summary.json` and each cell's samples/latencies/window logs
retain raw rates, cohorts, errors, pressure and read distributions. These short
sequential cells cannot close long sustained-performance acceptance.

All 384 sampled independent first/last payload checks through two cold mounts
pass. Private sync/append/dirty-stop/shutdown/WAL-tail/GC faults return zero,
with actual opt-in process options in `fault-process-options.json`.
`fault-fixture` contains the source/binary manifest, JSON verdicts and raw logs.
The original missing-stop-helper controller error is retained separately.
Final service identity, both mounts, both opted-out clients and four responding
Raft hosts are verified in `final-service-state.json`; experiments/diagnostics
are off and existing service/test payloads are preserved. Rollback
executables and raw perf are deliberately excluded from the repository.

See [status and limitations](../../../docs/status/w93-read-pressure-20261009.md).
