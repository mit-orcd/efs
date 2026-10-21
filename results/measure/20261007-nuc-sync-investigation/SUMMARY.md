# nuc_efs I/O and synchronous-write comparison — 2026-10-07

## 1. Purpose and linked item

User-requested NUC study comparing SATA vs NVMe persistence paths and
before/fair writer admission, with synchronous (benchmark O_SYNC) writes.
Linked item: **unlinked** to a numbered W/D gate — added by commit `41e2909aa`
"results: compare nuc SATA and NVMe with synchronous writer admission"; it is
the NUC counterpart of the xorinox writer/sync investigations
(`../20261007-sync-writer-investigation/`).

## 2. Verdict

**PASS** as a measurement: all 80 untraced runs pass with zero errors; 16 perf
captures/reports, 16 strace cases, 4 wall-clock strace cases, and the NUC
admission/actual-store tests all pass. Conclusion is diagnostic, not an
adoption gate: fair admission helps buffered throughput on this 4-writer host;
a large SATA-vs-NVMe synchronous persistence gap is documented but not
causally diagnosed.

**Measurement caveat (Oct 7):** these short bounded-write runs mixed initial
file creation and replacement. Per-run p99 averages below are historical
summaries, not pooled p99. The
[corrected latency study](../20261007-latency-validation/SUMMARY.md) separates
population from timing and retains every observation. Do not use these older
tail figures as steady-state acceptance evidence.

## 3. Source commit and build identity

Private source/build in `/tmp/efs-nuc-sync-study-20261007` (not retained).
"before" binary uses `writer.c` from the `b3c40474` parent; "fair" uses
reserved per-slot handoff. Both binaries share the same committed buffered
storage fix and benchmark O_SYNC support. Binary sha256 (recorded in
`provenance.txt` and per-profile manifests, commit field "unknown"):
before `c10503f65cf8df78…`, fair `4c634faea10703d3…`.

## 4. Environment

Single NUC host (8 CPUs; auto selected FOUR writers, vs 12 in the xorinox
study). Root1 `/data1/efs/bench`: SATA MK0800GEYKE, XFS noatime logbsize=256k.
Root2 `/data2/efs/bench` → `/home/efs/additional-work-dir`: Samsung 970 EVO
1 TB NVMe via LVM rl-home, XFS relatime logbsize=32k (see `provenance.txt`).
Four live daemons retained; no deployment/restart or mount-setting change.

## 5. Method

Driver script `study.py` (retained). 80 randomized untraced runs: 5 repeats ×
buffered/direct × sync on/off × each root × before/fair admission; QD16,
64 KiB fragments, 64 MiB overwrite window, 1.5 s phases. Separate diagnostics:
16 perf captures (per-case `profile-*-manifest.json` + `*-flat.txt`), 16 CPU
strace cases (`*-cpu-strace.txt`), 4 wall-clock strace cases
(`wall-strace-root{1,2}-{buffered,direct}.txt`, `strace -f -c -w`). O_SYNC
applies to each buffered body/checksum write separately with a final length
fence fsync; direct retains its single body/checksum writev — deliberately
conservative, not one barrier per PUT. No relaxation of sync/durability.

## 6. Results

80 untraced runs (mean of 5 repeats):

| Sync | Mode | Storage | Before GiB/s | Fair GiB/s | p99 before/fair µs | Worst max before/fair ms |
|---|---|---|---:|---:|---:|---:|
| off | Buffered | SATA | 8.623 | 11.646 | 910 / 245 | 9.9 / 3.2 |
| off | Buffered | NVMe | 8.252 | 10.378 | 956 / 284 | 5.1 / 3.0 |
| off | Direct | SATA | 0.402 | 0.412 | 49887 / 4379 | 489.0 / 29.4 |
| off | Direct | NVMe | 1.970 | 1.959 | 7517 / 1586 | 106.4 / 86.5 |
| on | Buffered | SATA | 0.311 | 0.308 | 29017 / 5626 | 87.6 / 29.0 |
| on | Buffered | NVMe | 0.017 | 0.016 | 603014 / 104830 | 1032.3 / 121.8 |
| on | Direct | SATA | 0.301 | 0.307 | 30233 / 4457 | 190.0 / 26.8 |
| on | Direct | NVMe | 0.036 | 0.034 | 264455 / 74997 | 614.4 / 100.7 |

Fairness improves buffered cached throughput on this four-writer host, unlike
the ~5% overhead observed with twelve writers on xorinox; results cannot
determine a universal default writer count. Cached rates are page-cache
performance, not physical-media bandwidth.

The SATA device delivers ~0.30 GiB/s for synchronous writes; the NVMe only
~0.02–0.04 GiB/s. Hardware/cache/firmware and filesystem layouts differ, so
this is evidence of a persistence-path gap, not a causal diagnosis. Wall-clock
strace (`-f -c -w`), current code with O_SYNC: SATA write averages 386 µs,
direct writev 703 µs; NVMe write 5771 µs, direct writev 6044 µs. Buffered
final fsync averages 7 µs SATA vs 1167 µs NVMe. Tracing perturbs scheduling;
these are separate diagnostics, not untraced acceptance latency. Default
`strace -c` reports syscall CPU time, so earlier summaries must not be read as
elapsed latency. Aggregate futex totals overlap writers' storage waits and do
not establish that futex causes the persistence gap.

## 7. Limitations and remaining gates

Short phases are diagnostic only; longer workloads larger than RAM plus
flush/latency/device instrumentation are still required for sustained-media
conclusions. No universal default writer count established. Tail figures
superseded by the corrected latency-validation method (caveat above).

## 8. Evidence index

| sha256 (16) | File |
| --- | --- |
| c8ccfa62ae8a92a6 | results.json — all 80 randomized untraced runs |
| 67a0c5a3a17eb700 | provenance.txt — device/mount identity and binary hashes |
| d79622bb2122bbfd | study.py — driver script for the full study |
| f7f35bae2ca1e5a3 | wall-strace-root1-buffered.txt — SATA buffered wall-clock profile |
| e0fe50ac02636a4d | wall-strace-root2-direct.txt — NVMe direct wall-clock profile |
| 38115abdae418ca1 | root1-buffered-sync0-flat.txt — SATA buffered perf flat report |
| 6d736e3ce73f4cd3 | root2-direct-sync1-flat.txt — NVMe direct sync perf flat report |
| 2a2165dde1c767f1 | profile-root1-sync0-manifest.json — perf capture manifest example |
