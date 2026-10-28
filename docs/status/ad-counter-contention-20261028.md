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
not just diagnostics. This comparison isolates shared counter updates, not all
timestamp or telemetry cost. Detailed `g_stats` writer timing is **not enabled
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

The existing D4 driver runs 45 seconds, with two mounts: small-file writers
and a foreground write/fsync/read/stat/list/query stream. This is D4's
saturated **closed-loop** offered load, not a fixed arrival-rate generator.
As in the loopback D4 gate, fault-built daemons use 256 KiB memtables,
one-second pack minimum age, 200 ms pack passes and two-second query refresh;
packing, indexing, GC, flushing and compaction remain enabled. Original D4
deadlines and error/readback checks remain: ten-second foreground operations,
150-second drain, 30-second stall reporting, 1 GiB client / 2 GiB daemon RSS,
50,000 pack backlog and 60-second index pending age. D4 verifies every
acknowledged, non-deleted, non-uncertain live file; uncertain writes remain
errors, never a payload PASS by exclusion.

Ten seconds into load, profile all three **owned daemon PIDs** for 30 seconds:

- `perf stat`: task-clock, cycles, instructions, cache references and misses,
  separately per daemon; `/proc/PID/stat` CPU seconds as a second measure.
- `perf record -N -m 64 -e cycles -F 499 -g --call-graph fp`: sample attribution
  per daemon and aggregate, with kernel CPU included in the denominator.
- `perf record -N -m 64 -e ibs_op// -c 1048576 -d`: sampled data addresses and
  memory levels mapped to cache lines overlapping ELF `g_cls` and `g_stats`.
  A sampled L1 miss is not, by itself, proof of false sharing. Whole-process
  `cache-misses` are never described as counter-line misses.

A second thread attempts FUSE `stat` syscalls at 10 Hz throughout the case,
recording p50/p99 and errors. These are syscall latency, not isolated RPC
service latency. Daemon logs preserve `raft-obs` pump-hold/lock-wait and
drain/tick/apply/persist high-water observations; these are not a complete
pump-cycle distribution. Profiler overhead is present in these diagnostic
throughputs, equally in both variants; no native uninstrumented speedup is
inferred from them. Report medians **and ranges**, not just the best run.

Reproduce with built, matching daemon/client/management binaries in each root:

```sh
python3 scripts/measure_counter_cost.py \
  --source BASELINE_OR_LOCAL_EXPERIMENT --scratch NEW_PRIVATE_SCRATCH \
  --out NEW_RAW_DIRECTORY --writers 32 --transport tcp \
  --load-s 45 --profile-s 30
python3 scripts/analyze_counter_profile.py NEW_RAW_DIRECTORY \
  BASELINE_OR_LOCAL_EXPERIMENT/tests/faults/efsd
```

Repeat for 8/16/32 writers, both variants and transports, three times. RDMA
adds `--transport rdma --host 172.16.99.0` instead of TCP. Run on a compatible
private host, not a production store. The exact serial commands, build commands
and environment are retained below. The measurement driver never modifies the
chosen source; use an external copy for the explicitly uncommitted no-op patch.

## Calibration failures and limitations

- `pilot-tcp32` failed before formation because the measurement invocation
  included retired `--direct-io`. Current source always uses O_DIRECT. No
  workload ran; the corrected invocation uses no such selector.
- `pilot2-tcp32` passed D4 and orderly cleanup, but both default-buffer sampling
  profilers exited 255 with incomplete perf files. It is **not counter-cost
  evidence**. Bounded 64-page profiler buffers fixed capture in `pilot3`.
- `pilot3-tcp32` passed D4, all profiler exits and checked cleanup. Its 90-second
  predecessor is not pooled with the 45-second comparison matrix.
- Analyzer calibration initially omitted `ip` from perf-script fields. This
  perf version then emitted empty symbols/zero addresses despite valid data.
  Those preliminary zero-attribution summaries are invalid. The committed
  analyzer requests IP explicitly, retains unresolved samples in the denominator
  and counts address coverage; raw captures remain unchanged.
- Sampling does not prove zero cost or zero misses. Threshold decisions require
  symbol/address coverage, repeated measurements and the comparison; an
  unresolved/unsupported capture cannot qualify the under-one-percent rule.

## Evidence and remaining work

Raw evidence on node9900:
`/data1/efs/evidence/taskAD-20261028-node9900-1d2852da4/`.
Scratch: `/tmp/efs-taskAD-20261028-1d2852da4/`.
`environment.json`, `variants.json`, `local-noop.patch`, build logs, pilot logs,
`matrix-reproduction.py`, per-case commands/PIDs/maps/perf captures, D4 exact
readback results and checked stop records are retained. Stores/payloads remain
temporarily while measurement is running; prune only after finalization and
checked mount/process absence, using `edelete`. Keep minimal matching ELFs for
perf symbolization, text/results/profiles and full hashes outside Git.

Remaining: complete and assess the repeated matrix, apply AD's one-percent
decision, run the full `test-all` summary gate serially, finalize hashes and
cleanup, then update AD's checkbox. No AD closure or new performance claim yet.
