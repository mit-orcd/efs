# AD: shared diagnostic counter cost — October 28, 2026

**IN PROGRESS.** Task AD from the [alpha checklist](alpha-1.0-2plus1-checklist.md).
This branch measures first; no product counter implementation or setting has
changed. The user directed an alternate host after node9901 was found running
Task H. All AD builds, fixtures and measurements use **node9900**; the existing
production FUSE process, node9901 and orcd-mini are untouched.

## Source and experiment

Frozen product source: `1d2852da437823942a7bff9bc8a41cfd498b1ec5`.
Baseline build identity `1d2852da43782`; local experiment identity
`1d2852da4-adnoop-8179e99f7bbf`. Both use native GCC `-O3 -g
-fno-omit-frame-pointer -march=native -mtune=native`, the same fixed build-time
string, and libfuse 3.16.2 from `/data1/efs/deps/libfuse`.

The experiment exists **only outside Git**, not as a product compile option or
environment switch. Its full patch is retained with SHA-256
`8179e99f7bbf1c202683be2ab2b5b4d84a76bc432bfe44565b51f0b2e2b9226d`:

- Empty `efs_iostats_add`; retain its callers and both timestamp calls.
- In the writer diagnostic wrapper, set `job->measure=0` and call the original
  `submit_and_wait_inner`; keep all scheduling, admission and I/O counters.
- Seed one diagnostic class's operation count to one, solely in the local
  experiment, to preserve the periodic dump's five-second cadence. Otherwise
  `efs_iostats_dump` would never advance `g_last_dump_us`, and repeatedly scan
  the RDMA registry on every pump pass. Such an A/B would measure a different
  problem. The experimental diagnostic totals are deliberately invalid and
  never qualify the management counter interface.

`efs_iostats_now_us` remains unchanged: it also drives maintenance deadlines,
not just diagnostics. This comparison removes shared counter updates while
retaining caller timing and dump cadence, not all timestamp or telemetry cost.
It also leaves less diagnostic snapshot work (empty latency rings and uptime);
use direct counter-body cycle attribution for the threshold, not a claim that
the no-op A/B isolates every nanosecond. Detailed `g_stats` writer timing is **not enabled
by the daemon**: `server_writer_stats_reset` is called by the local benchmark
and writer-routing test. The production workload therefore cannot establish
how that benchmark-only enabled path scales.

## Method

Node9900: two AMD EPYC 9474F CPUs, 96 physical cores / 192 logical CPUs, eight
NUMA nodes; RHEL kernel `5.14.0-687.39.1.el9_8.x86_64`; XFS `/tmp` on `/dev/md0`.
PMU access is permitted (`perf_event_paranoid=-1`); `perf` version
`5.14.0-687.56.1.el9_8.x86_64`; AMD `ibs_op` address/data-source sampling works.
This is a single machine with three processes, not a network-scale claim.

Each fresh private cluster uses free ports in 19000–19999, discovery off,
8 GiB quota per daemon, unconditional O_DIRECT, and 32 daemon writer threads
held fixed. Application writer concurrency is 8, 16 or 32. TCP uses loopback;
RDMA uses `172.16.99.0` and `mlx5_0`. Three repetitions per transport and
concurrency alternate baseline/no-op order. No other AD case or full gate
runs concurrently with a measurement.

The final `matrix2` uses D4's fixed-footprint rewrite mode: 2,048 files total
(256/128/64 per application writer), populated before the 45-second load,
then overwritten, truncated and rewritten. Seven eighths use 1–8,192 bytes;
every eighth uses 192–512 KiB, exercising unpacked large-file fragment I/O as
well as small-file packing. A second mount runs the foreground
write/fsync/read/stat/list/query stream. Throughput is rewrite mutations per
configured load second, not the population's creates per second. This is D4's
saturated **closed-loop** offered load, not a fixed arrival-rate generator.
As in the loopback D4 gate, fault-built daemons use 256 KiB memtables,
one-second pack minimum age, 200 ms pack passes and two-second query refresh;
packing, indexing, GC, flushing and compaction remain enabled. Original D4
deadlines and error/readback checks remain: ten-second foreground operations,
150-second drain, 30-second stall reporting, 1 GiB client / 2 GiB daemon RSS,
50,000 pack backlog and 60-second index pending age. D4 verifies every
acknowledged, non-deleted, non-uncertain live file; uncertain writes remain
errors, never a payload PASS by exclusion.

Before mounting or starting load, attach to all three **owned daemon PIDs**
while the formed cluster is idle, with events disabled (`-D -1`). This avoids
this perf version's race enumerating rapidly exiting connection threads.
Owned control pipes require an acknowledgement from every profiler. Ten
seconds into load, enable events, measure 30 seconds, then disable with another
acknowledged barrier. After verifying the profilers are still alive, request
SIGINT and retain their raw exit codes: planned `-2` is expected here, whereas
an early exit fails the case. Only profiler children get a 65,536 descriptor
soft limit; no host configuration or daemon limit changes.

- `perf stat`: task-clock, cycles, instructions, cache references and misses,
  separately per daemon; `/proc/PID/stat` CPU seconds as a second measure.
- `perf record -N -m 64 -e cycles -F 499 -g --call-graph fp`: sample attribution
  per daemon and aggregate, with kernel CPU included in the denominator.
- `perf record -N -m 64 -e ibs_op// -c 1048576 -d`: sampled data addresses and
  memory levels mapped to cache lines overlapping ELF `g_cls` and `g_stats`.
  A sampled L1 miss is not, by itself, proof of false sharing. Whole-process
  `cache-misses` are never described as counter-line misses.

The original matrix used measurement driver `82afb4d2c` (64-page buffers).
The final audit found **371 lost IBS samples and 766,959 recorded samples**
in `matrix2/rdma-w16-r2-noop/memory.data` (about 0.048% lost). All 36 CPU
recordings and the other 35 memory recordings have zero lost samples. The
original stays labelled PARTIAL. A separate matched 16-writer RDMA pair under
`capture-repair-256/` uses 256-page buffers; both D4 checks and all four
recordings pass with zero loss. Those extra trials do not replace an original
trial in the throughput medians or turn the original recording into COMPLETE.
The current driver defaults to 256 pages, and the current analyzer records
loss explicitly and returns exit 3 (INCOMPLETE) for any partial capture.

A second thread attempts FUSE `stat` syscalls at 10 Hz throughout the case,
recording p50/p99 and errors. These are syscall latency, not isolated RPC
service latency. Daemon logs preserve `raft-obs` pump-hold/lock-wait and
drain/tick/apply/persist high-water observations; these are not a complete
pump-cycle distribution. Profiler overhead is present in these diagnostic
throughputs, equally in both variants; no native uninstrumented speedup is
inferred from them. Three additional 32-writer TCP pairs use `--capture none`
(measurement tool only), retaining the same workload, CPU window and stat loop
without perf. Report medians **and ranges**, not just the best run.

Reproduce with built, matching daemon/client/management binaries in each root:

```sh
python3 scripts/measure_counter_cost.py \
  --source BASELINE_OR_LOCAL_EXPERIMENT --scratch NEW_PRIVATE_SCRATCH \
  --out NEW_RAW_DIRECTORY --writers 32 --transport tcp \
  --load-s 45 --profile-s 30 --working-set 64
python3 scripts/analyze_counter_profile.py NEW_RAW_DIRECTORY \
  BASELINE_OR_LOCAL_EXPERIMENT/tests/faults/efsd
```

Repeat for 8/16/32 writers with working sets 256/128/64 per writer, both variants
and transports, three times. RDMA
adds `--transport rdma --host 172.16.99.0` instead of TCP. Run on a compatible
private host, not a production store. The exact serial commands, build commands
and environment are retained below. The measurement driver never modifies the
chosen source; use an external copy for the explicitly uncommitted no-op patch.
Use the script at `82afb4d2c` for the original 64-page experiment; use the current
script for the 256-page capture repair. No CPU affinity or leader placement was
forced; per-case leadership is retained. The broad throughput spread is reported
without assigning it to a particular unmeasured cause.

## Calibration failures and limitations

- `pilot-tcp32` failed before formation because the measurement invocation
  included retired `--direct-io`. Current source always uses O_DIRECT. No
  workload ran; the corrected invocation uses no such selector.
- `pilot2-tcp32` passed D4 and orderly cleanup, but both default-buffer sampling
  profilers exited 255 with incomplete perf files. It is **not counter-cost
  evidence**; its silent exit was not root-caused. Bounded 64-page profiler
  buffers allowed a complete `pilot3`, but did not eliminate the later attach
  race. No incomplete capture is promoted to a passing profile.
- `pilot3-tcp32` passed D4, all profiler exits and checked cleanup. Its 90-second
  predecessor is not pooled with the 45-second comparison matrix.
- The first create/unlink matrix stopped at `tcp-w16-r2-noop`: D4 and cleanup
  passed but both sampling profilers exited 255 while opening a transient
  thread's `/proc/.../status`. Its partial matrix remains auxiliary evidence,
  not the final repeated rewrite comparison. `matrix2` starts afresh using
  the idle-attach control barrier.
- `pilot4-tcp32` failed the measurement startup barrier, before load: this perf
  sends `ack\n` including its terminating NUL. The driver now recognizes that
  documented-by-preflight byte sequence. `pilot5-tcp32` passed the repaired
  barrier, D4 rewrite checks and checked cleanup, with the planned profiler
  SIGINT exit codes retained. No failed case was overwritten or re-labelled.
- Analyzer calibration initially omitted `ip` from perf-script fields. This
  perf version then emitted empty symbols/zero addresses despite valid data.
  Those preliminary zero-attribution summaries are invalid. The committed
  analyzer requests IP explicitly, retains unresolved samples in the denominator
  and counts address coverage; raw captures remain unchanged. Attribution uses
  ELF instruction ranges even when perf cannot print a symbol, and verifies
  that the supplied ELF's build ID occurs in the capture. A wrong-ELF negative
  control refuses the no-op ELF against the baseline pilot before attribution.
  Zero-IP periods are also charged to the counter as a coverage-sensitivity
  check; this is not a statistical confidence bound on unsampled execution.
- Sampling does not prove zero cost or zero misses. Threshold decisions require
  symbol/address coverage, repeated measurements and the comparison; an
  unresolved/unsupported capture cannot qualify the under-one-percent rule.

### Internal error counters are not application verdicts

Do not read D4 PASS as zero internal RPC errors. For example, baseline
`matrix2/rdma-w8-r1-baseline` ends with 154 GET and disk-read non-OK outcomes
on each daemon, five PUT non-OK outcomes on node 3, and zero disk-write errors.
These are lifetime totals, including population/drain/verification. That case
has no application errors, no bad final payloads, no uncertain writes, and zero
packer read/put errors. Logs also retain two segmented-RDMA setup warnings;
actual RDMA frame counters are positive on all three daemons. Aggregate
`errors` counts include not-found/refused operations and do not identify their
individual return codes, so these totals alone establish neither corruption
nor an expected harmless cause. They remain visible in raw replies/logs;
AD does not close a zero-network-error, repair or durability gate. The local
no-op's deliberately invalid diagnostic totals never qualify error visibility.

## Evidence and remaining work

Raw evidence on node9900:
`/data1/efs/evidence/taskAD-20261028-node9900-1d2852da4/`.
Scratch: `/tmp/efs-taskAD-20261028-1d2852da4/`.
`environment.json`, `variants.json`, `local-noop.patch`, build logs, pilot logs,
`matrix-reproduction.py`, `matrix2-reproduction.py`, per-case
commands/PIDs/maps/perf captures, D4 exact
readback results and checked stop records are retained. Stores/payloads remain
temporarily while measurement is running; prune only after finalization and
checked mount/process absence, using `edelete`. Keep minimal matching ELFs for
perf symbolization, text/results/profiles and full hashes outside Git.

Remaining: complete and assess the repeated matrix, apply AD's one-percent
decision, run the full `test-all` summary gate serially, finalize hashes and
cleanup, then update AD's checkbox. No AD closure or new performance claim yet.
