# AD: shared diagnostic counter cost — October 28, 2026

**MEASURED — NO PRODUCT CHANGE.** Task AD from the
[alpha checklist](alpha-1.0-2plus1-checklist.md). At 32 application writers,
counter updates account for 0.0115–0.0200% of sampled aggregate daemon cycles
on TCP and 0.0075–0.0166% on RDMA. The highest individual daemon is 0.0358%
(TCP) / 0.0211% (RDMA), below AD's 1% stop criterion. Keep the existing shared
counters; no worker shards or product setting are introduced. The user
directed an alternate host after node9901 was found running
Task H. All AD builds, fixtures and measurements use **node9900**; the existing
production FUSE process, node9901 and orcd-mini are untouched.

## Source and experiment

Frozen product source: `1d2852da437823942a7bff9bc8a41cfd498b1ec5`.
Baseline build identity `1d2852da43782`; local experiment identity
`1d2852da4-adnoop-8179e99f7bbf`. Both use native GCC `-O3 -g
-fno-omit-frame-pointer -march=native -mtune=native`, the same fixed build-time
string, and libfuse 3.16.2 from `/data1/efs/deps/libfuse`.
The original measurement driver and full-gate source overlay are `82afb4d2c`;
uniform instruction-range attribution is replayed with `ef0ddf653`. The final
loss-aware analyzer and 256-page capture repair use `14ba987c2`. All have the
same unchanged compiled product source above. Source/tool hashes and both
actual ELF identities are retained in the external manifest.

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

Across the 21 baseline trials in the original matrix (18 profiled, three
unprofiled), summed per-daemon lifetime totals are GET non-OK **10,548**, disk
read non-OK **10,548**, PUT non-OK **2,785**, disk write non-OK **11**, GC delete
non-OK **0**. These are not counts of unique files or application failures.
`writer.c::run_job` counts every non-OK result of the storage write routine;
`store.c::server_write_fragment_with_sum_sync` can return STALE at its dead-inode
guard before issuing physical I/O. Thus even the label `disk_write errors`
does not prove a device error, and these aggregates do not classify the 11
outcomes. None is suppressed, cleared, or labelled harmless by this review.

## Results and decision

All **42 original D4 trials** pass their application/readback bounds, with
**89,606 final files checked**, no application errors, bad final payloads or
uncertain last writes. Both capture-repair trials also pass (2,163 and 2,143
final files checked). Every trial has positive KV compaction counters on all
three members, zero final packer read/put errors and checked orderly cleanup.
Every RDMA trial has positive actual transmit/receive data-frame counts on all
three daemons; TCP trials have zero RDMA data frames. Setup warnings and
internal non-OK outcomes remain recorded, as above.

Each row below summarizes the three **baseline** repetitions. Percentages are
period-weighted sampled cycles, with kernel CPU included. The individual-daemon
column is the maximum across its nine daemon observations, not an average that
hides a hot member. No zero-IP periods occur; charging every such period to
the counter therefore leaves these results unchanged.

| Transport | Application writers | Aggregate counter cycles %, median [min, max] | Highest daemon % | Counter IP samples / total cycle samples |
| --- | ---: | ---: | ---: | ---: |
| TCP | 8 | 0.0070 [0.0066, 0.0119] | 0.0214 | 25 / 403,886 |
| TCP | 16 | 0.0088 [0.0002, 0.0102] | 0.0377 | 22 / 477,697 |
| TCP | 32 | 0.0161 [0.0115, 0.0200] | 0.0358 | 44 / 662,693 |
| RDMA | 8 | 0.0203 [0.0021, 0.0207] | 0.0501 | 36 / 292,355 |
| RDMA | 16 | 0.0185 [0.0034, 0.0269] | 0.0526 | 37 / 342,765 |
| RDMA | 32 | 0.0112 [0.0075, 0.0166] | 0.0211 | 35 / 377,989 |

IBS observes respectively **1 / 0 / 4** accesses to cache lines overlapping
baseline `g_cls` at TCP 8/16/32, and **5 / 4 / 4** at RDMA 8/16/32. Across
these 18 observations: three L1 hits, three L2 hits, four L3 hits, seven
remote-core/node cache-hit descriptions and one unclassified level. These
baseline memory recordings are loss-free. Counts are far too small to estimate
a precise miss rate or prove false sharing absent. Lines include snapshot
readers and potentially adjacent globals; a no-op snapshot can still touch
them. A zero observation is not zero accesses. Enabled benchmark-only writer
statistics are not exercised by this daemon workload.

Instrumented throughput and summed CPU seconds across the three daemons;
numbers are median [min, max]. CPU refers to the 30-second sampling window,
not startup, population or final verification:

| Transport / writers | Baseline mutations/s | No-op mutations/s | Baseline CPU s | No-op CPU s |
| --- | ---: | ---: | ---: | ---: |
| TCP / 8 | 469 [101, 502] | 273 [198, 484] | 229.8 [126.3, 244.9] | 216.9 [155.9, 252.8] |
| TCP / 16 | 391 [143, 518] | 257 [111, 348] | 203.1 [141.2, 241.3] | 161.5 [139.5, 184.7] |
| TCP / 32 | 571 [468, 677] | 224 [125, 656] | 253.9 [248.6, 270.2] | 150.1 [145.0, 249.6] |
| RDMA / 8 | 507 [95, 514] | 263 [227, 504] | 246.9 [149.8, 267.6] | 182.5 [179.3, 264.7] |
| RDMA / 16 | 597 [155, 611] | 403 [170, 622] | 261.7 [147.3, 283.7] | 214.5 [162.6, 259.5] |
| RDMA / 32 | 604 [113, 707] | 364 [151, 551] | 275.6 [170.4, 286.0] | 211.6 [141.3, 266.8] |

At 32 writers without perf, TCP baseline is **554 [211, 730] mutations/s**,
versus no-op **521 [160, 570]**. CPU is **140.5 [61.2, 164.4] s** versus
**128.5 [46.3, 141.7] s** per 30-second window. Stat syscall p99 is
**53.3 [35.1, 55.1] ms** versus **332.6 [56.9, 383.9] ms**. The overlapping
throughput ranges do not establish a counter-removal gain or performance
equivalence. Do not call either a native default-configuration IOR result:
D4's observers and `EFS_RAFT_OBS=1` remain active.

The instrumented 32-writer observer measurements are likewise variable:

| Metric, median [min, max] | TCP baseline | TCP no-op | RDMA baseline | RDMA no-op |
| --- | ---: | ---: | ---: | ---: |
| Stat syscall p99, ms | 267.1 [53.6, 355.6] | 47.0 [46.0, 61.4] | 83.7 [54.8, 389.1] | 62.1 [56.5, 405.2] |
| Pump-hold high-water, ms | 23.1 [18.9, 24.5] | 29.1 [12.9, 32.1] | 16.8 [15.8, 37.7] | 15.1 [14.0, 20.4] |
| Lock-wait high-water, ms | 2.75 [2.51, 3.44] | 2.96 [2.68, 12.10] | 2.85 [1.93, 3.25] | 2.70 [2.24, 7.28] |

`matrix-summary.json` retains medians/ranges for every configuration. Logged
high-waters are five-second buckets whose dump timestamps fall in the sampling
window; a boundary bucket can include preceding work. No complete pump-cycle
distribution, isolated RPC service latency or per-call nanosecond cost was
measured. The task's explicit under-1%-CPU decision is supported; these runs
do not settle other admission, latency or metadata-serialization tasks.

**Keep the implementation.** Five fixed counter classes occupy 2,800 bytes;
there is no per-worker lifetime registry to grow or retire. Totals, errors,
latency sums/maxima, `iostats:` and `io-stats` meanings remain unchanged.
The current p50 is the upper median of a weak concurrent snapshot of the last
64 ring reservation positions per class, not a lifetime percentile or a
per-worker-median merge. No new collection/aggregation semantics are needed.

## Code gate and validation

On node9900, after measurement and with no concurrent profiling:

```sh
TMPDIR=/tmp/efs-taskAD-20261028-1d2852da4/gate-tmp \
EFS_GIT_ID=1d2852da43782 EFS_GIT_BRANCH=agent/devel-task-ad \
EFS_GIT_DESCRIBE=1d2852da43782 EFS_BUILD_TIME=2026-10-28T18:40:00Z \
EFS_FUSE_PREFIX=/data1/efs/deps/libfuse EFS_TEST_TARGET=test-all \
EFS_TEST_JOBS=8 EFS_BUILD_JOBS=8 scripts/make_test_summary.sh
```

**545 commands, 0 FAIL, 4 NOTRUN; INCOMPLETE / exit 3**, 690 seconds plus
15-second build. Host limits: external cluster scripts absent, Valgrind absent
for RDMA Memcheck and real leak fixtures; the sanitizer self-test emits a
host-refusal NOTRUN marker (not a claim that the full sanitizer suite ran).
Six commands exceed their individual budgets: K35 fsync 107.9/80 s, B6 down
62.3/55 s, K43 coordinator kill 52.8/25 s, D39 model shards 33.7/25, 19.0/15,
6.4/5 s. Total is below 1,100 s; no budget, timeout or durability rule changed.
This is not a zero-skip release gate.

All 36 original counter attributions replay with the original executable paths
absent, using the retained matching ELFs. Every target/total cycle period and
every memory/address count matches the original analysis. Wrong-ELF refusal
and the original partial capture's exit-3 negative control pass; the separate
256-page repair pair passes the strengthened zero-loss checks. This preserves
the partial evidence rather than replacing it with a clean label.

Final tooling compiles as Python on node9900. Documentation checks pass **5/5**.
Canonical graph validation passes six known-bug traces; its unit suite runs
nine tests, eight PASS and one scope-selection SKIP because the Linux source
archive has no Git index. Existing navigation gaps remain: one partial
`protocol.h` parse, 49 symbol-empty files and 1,462 unresolved call references.
They are recorded as graph coverage limits, not runtime evidence.

Readback is through the workload mounts, not a cold remount or restart. It does
not qualify persisted replicas, crash recovery, shared-file writers or a
multi-host throughput claim. No sample is presented as full payload verification.

## Evidence and cleanup

Raw evidence on node9900:
`/data1/efs/evidence/taskAD-20261028-node9900-1d2852da4/`.
Scratch prefix: `/tmp/efs-taskAD-20261028-1d2852da4/`.
`environment.json`, `variants.json`, `local-noop.patch`, build logs, pilot logs,
`matrix-reproduction.py`, `matrix2-reproduction.py`, per-case
commands/PIDs/maps/perf captures, D4 exact
readback results and checked stop records are retained. `capture-loss-audit.json`
accounts for all 72 original recordings; `capture-repair-256/` holds the paired
recheck; `symbols/replay-validation.json` proves replay without build-tree ELFs.
Minimal matching ELFs are retained, not whole build trees:

- Baseline SHA-256 `d1b46842005056f4a1472b4434afd1b16f6e80adf20f593449938630f070a967`,
  ELF build ID `c570f4f034d1612f183687391f25e515663c9c8d`.
- No-op SHA-256 `e3f53218a74f9c0ad06e2dea3f66a5fd618d64abeae2401e16a069553e03f577`,
  ELF build ID `c3f0cf1e53400ac38f8ad1b8a983bb26cdc03525`.

**Finalized and pruned:** the private scratch prefix is absent after
`edelete --delete --force --uid 246111`, with no owned process/cwd/mount.
Approximately 29 GiB of synthetic stores/payloads, source/build trees and gate
scratch were removed. The existing production FUSE PID 2380112, started Oct 27,
remains running. The raw evidence (about 4.8 GiB), minimal ELFs and all failures
remain outside Git. Post-prune strict replay also passes using only those
retained files. `PRUNED.txt` and pre/post-prune checks record the cleanup.

The seal re-hashes **1,626 regular/linked files** twice and verifies **22
symlink targets**. Useful SHA-256 anchors:

- `SHA256SUMS`: `0d46663f6194ecc6d818c36f99f296cddfe692c3fd1b6934a688289266ff2997`.
- `final-source-manifest.json`: `f7e77fdc11c32ed4bfbe78e11f67df8070d3b45d8667616a5b72406e050d963e`.
- `matrix-summary.json`: `a45a14f5568951a6db635432f39bb2b3a5c95149fc99cbfb5a9cdf660634e92c`.
- `symbols/replay-validation.json`: `4be797c6a90e9e9f23bd2245b123e0087b8bf9ff165f8b86c708cd193572e9f1`.
- `PRUNED.txt`: `fbf424973d6d30b04f3e8e56ce1ee5aa0130163344a7b462903a5934d184183a`.

Bulk store deletion forfeits replaying the exact former workload state; the
source/method recreates fresh fixtures. Counter attribution remains replayable
without them. This closes AD's measurement/no-change outcome, not any other
alpha release, recovery or zero-error gate.
