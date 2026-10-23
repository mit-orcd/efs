# Intel LSM candidate indexing — 2026-10-14

**Implemented/deployed; bounded smoke PASS; short matched measurements improve.
Full acceptance/sustained throughput NOTRUN by human scope.** Implements the
first experiment proposed in the [Intel live profile](intel-live-performance-20261014.md),
following the [checksum-tail EIO correction](intel-direct-tail-eio-20261014.md).
No W100 reopening or optional-feature qualification.

## Source and implementation

Candidate/deployed source `6cbd99e549c8bd9576761556ef6b3b9ca8445e83`, devel,
build ID `6cbd99e549c8`, version `v0.3.2-pre-alpha-455-g6cbd99e54`, common
build metadata time `2026-10-14T20:10:43Z`. Baseline running source/library
`f76101e8fc5469e5999e25cc1c0db6418416f9f3`; the intervening1f11 checkpoint
is documentation only. Native optimized GCC/frame-pointer builds on each host;
no CPU, transport, durability, retry/window or format tuning.

- [kv_candidates.h](../../src/kv/kv_candidates.h) builds a range-to-segment
  bitmap in memory. Ascending set-bit positions preserve L0 newest-first,
  then L1 order. Unknown/wide coarse bounds conservatively include candidates.
  Retained bitmap allocation is capped at4MiB/LSM; allocation refusal or a
  larger inventory leaves linear coarse filtering available. Current production
  shapes require about20–36KiB per daemon. Multi-range/empty prefixes retain
  the general traversal rather than unioning hundreds of bitmap rows.
- Sorted/disjoint L1 first keys provide binary lower/upper bounds for point
  and prefix searches. The predecessor of the seek is retained; the end bound
  excludes a segment only when its first key is beyond the whole prefix.
  Missing resident bounds fall back to all L1. Block record binary search and
  coarse exclusion already existed and remain intact.
- [kv_lsm.c](../../src/kv/kv_lsm.c) uses the selector in point lookups and
  live prefix scans. Scan scratch is sized/initialized for candidates instead
  of every segment. Private retained snapshots keep their existing private
  scan/filter and scratch limits and never allocate this live bitmap.
- Flush, compaction and manifest load invalidate the derived index. A segment
  revision also participates in the existing view check after a pinned pread
  drops/reacquires the mutex. No bitmap pointer is retained across that unlock.
  The index is rebuilt under the same mutex; memtable/pending/apply precedence,
  tombstones, pinned lifetimes and transaction/read freshness are preserved.

The change does not implement lane-result reuse, long-lived attribute caching,
different on-disk indexing, or deferred durable
acknowledgements. The prior digest-tail read fix remains included.

## Environment and deployment

Main Intel: two physical Core Ultra7 256V hosts,8 logical CPUs each,5Gb/s
Ethernet; four files-engine daemons on10.0.0.207/.209 ports17432/17433,
400GiB/node, approximately99.12GiB used/node. Source roots and stores retained;
configured `/data2/efs/n2` and `n4` resolve to the already configured
`/home/efs/additional-work-dir/n2` and `n4` paths. No storage relocation occurred.
Intel/xorinox use Rocky10.2/Linux6.12/libfuse3.16.2; AMD uses Ubuntu/Linux7.0/
libfuse3.18.2. External client links negotiate2.5Gb/s; AMD is Ryzen AI MAX+395
and xorinox Ryzen9 7940HS. Private Intel and AMD-local clusters remain running.

TCP, daemon `--direct-io`. All eight main processes have
`EFS_COMMANDS=0`, `EFS_COMMANDS_SERVER=0`, `EFS_EXPIRY=0`,
`EFS_TRANSPORT=tcp`; persisted commands policy version1/mask0;
`.efs` absent on all four mounts. No root policy/package change needed.

|Host|Deployed efsd SHA256|Deployed Intel-client efs-fuse SHA256|
|---|---|---|
|Intel1|`472e53a7c947dc7c61590ec7a027cb0c6ee36a85a3f47c6bff9daa5663831ce0`|`b05e9d3eeb71791ad4a3ea8b15bdc5d13966f0634d5d2e4a584598151b44b2fe`|
|Intel2|`179080657f8b6267b0cd0c4633bdabc42e8cb84703bfdc905ab256c20a53025a`|`652d2adabee8fffef33a67f0b26f9c4cc9b3a8fce09e84244caf59c52bb571bf`|
|AMD|Not deployed in independent client tree|`ca6de7db22009333a6330562be0485ec8c7267eec05f286f4f90b8c910b4515c`|
|xorinox|Not deployed in independent client tree|`8e196a986cc9205a239a1adb8b04663adcff047d26a0490542c1d345a232c2ec`|

Builds staged first. All four owned main clients drained, then the four main
daemons stopped cleanly. Source links switched to the frozen release, every
peer launched before waiting for persisted recovery, then clients restored.
One status probe during recovery was degraded; clients remained unmounted
until4/4 healthy membership and identical versions were confirmed. No
formatting, store deletion, kernel-policy change or force-discard occurred.

Intel source `/data1/efs/src` points into `releases/6cbd99e549c8`; Intel mounts
remain `/data1/efs/mnt`. AMD/xorinox retain independent source/config/logs under
`/data1/efs/intel-client`, source links to their own release, and mounts at
`/data1/efs/intel-mnt`. The six private Intel and six AMD-local processes retain
exact prior PIDs/arguments/executables/profiles. Existing long jobs were already
stopped by the preceding upgrade and remain stopped. Only bounded smoke and
the short requested measurements ran.

## Measurements

### Isolated actual-LSM fixture

[lsm_index_smoke.c](../../tests/measure/lsm_index_smoke.c) runs unchanged against
the old and new static libraries. Real temporary segment files:128 disjoint L1
segments within one coarse range,32 overlapping L0s, one wide legacy-style L0
spanning coarse ranges, and one flushed update (34L0/128L1 during timing).
Each file holds one or two tiny records. Fixture construction uses a1MiB
memtable/NOSYNC WAL and stops its compactor for deterministic layout; timed
operations are read-only, warm native KV calls, not TCP/direct filesystem I/O.
This deliberately isolates metadata search cost.

Two alternating baseline/candidate trials per host, pinned to the same CPU0
with `taskset -c 0`. Each trial has five rounds of10,000 absent point gets and
10,000 empty reduction-like prefix scans. Discard the first warm-up round;
report median per-operation nanoseconds over the other eight rounds per build.

|Host / operation|Baseline ns/op|Indexed ns/op|Speed ratio|
|---|---:|---:|---:|
|Intel1 point miss|845.35|293.10|2.88x|
|Intel2 point miss|817.40|293.30|2.79x|
|Intel1 prefix miss|1356.35|533.80|2.54x|
|Intel2 prefix miss|1326.55|562.25|2.36x|

These numbers establish a gain for this small warm search fixture, not a
corresponding multiplier for arbitrary applications. L1 selection reduces a
full128-slot traversal to its relevant interval; the overlapping L0s remain
eligible. Bitmap construction after a publication is amortized, not included
in every hot query. No claim is made about cold/OOM/adversarial workloads.

### Mounted metadata smoke

[intel_metadata_smoke.py](../../tests/measure/intel_metadata_smoke.py) runs
identically before and after deployment on AMD and xorinox, launched
concurrently. Each client first reads six existing tiny files and checks exact
length/EOF/SHA256, then runs five serial rounds of120 path-based stat calls
across the same six paths. No strace/perf attached. Discard round0 and compare
medians of the remaining four **round means**, with round p95s shown separately.
Invocation starts are not precisely synchronized. Other clusters remain alive.

|Client|Before round-mean ms|After round-mean ms|Lower latency|Speed ratio|Before/after median round p95 ms|
|---|---:|---:|---:|---:|---:|
|AMD|2.951|2.077|29.6%|1.42x|4.125 /2.745|
|xorinox|3.103|1.702|45.1%|1.82x|4.148 /2.303|

All six read-control hashes match before/after. All four persisted MANIFEST
SHA256s match exactly across the comparison: L0/L1 counts n1=140/144,
n2=261/287,n3=261/287,n4=135/144. This rules out a different persisted segment
layout as the explanation. Restart, leader placement, network scheduling,
memtable state and short-run variance are still limitations; this is not a
long statistically powered or loaded throughput qualification. No million-file
creation/crawl completion or whole-workflow speedup is claimed.

## Smoke results and reproduction

PASS on both Intel hosts against old and candidate libraries: point hits and
misses, L0 tombstone masking, full/prefix scan contents, exact retained-snapshot
old value across a live flush, live updated value, compaction/reopen, cross-range
boundary keys, and later resurrection/tombstone flushes after index population.
The fixture deletes only its own temporary files. Initial standalone compilation
needed an explicit `-Isrc/kv` target; corrected before all final measurements.

After deployment:64 existing files/client (32previous EIO cases and32 MD5
controls) match the preceding fixed release's exact inode/size/SHA256 records
through all four clients. One new owned fsynced file per host is read with exact
bytes/EOF through every mount. Payload is
`bytes(range(256))*((role+1)*256)+bytes([role])*17`, roles0–3. The four owned
files and their directory are removed after verification. Final4/4 node health,
matching versions, eight process/profile checks and independent paths PASS.

Only bounded smoke ran. Full native, sanitizer, fault/concurrent suites,
allocation-refusal injection, prolonged churn and retained-reader compaction
acceptance are **NOTRUN**, as requested. This does not supersede their existing
release gates.

Reproduce the engine smoke on Linux with a native built library, GCC and an
unprivileged account; no deployed stores or root access required:

```sh
make -j4 tests/measure/lsm_index_smoke
taskset -c 0 ./tests/measure/lsm_index_smoke
```

For baseline comparison, compile the same committed smoke C file against the
old revision's internal/public headers and old `libefs.a`, using
`cc -O3 -g -fno-omit-frame-pointer -march=native -D_GNU_SOURCE
-IOLD/include -IOLD/src/kv tests/measure/lsm_index_smoke.c OLD/libefs.a
-lpthread -lm -ldl -libverbs -o baseline-smoke`. Run baseline then candidate
twice, on the same permitted CPU. Each run creates its own temp directory and
removes it; this requires no retained private capture. Compile the baseline
library separately rather than mixing old/new struct layouts or objects.

On the physical external clients, with the existing six documented Intel
paths available and a verified FUSE mount:

```sh
python3 tests/measure/intel_metadata_smoke.py /data1/efs/intel-mnt
```

Run once per client before and after, with the same concurrency; preserve
round records and fixture hashes. Missing fixtures or failed checks are errors,
not a zero-time pass. This mounted reproduction requires the existing fixture;
the independent engine smoke above does not. Neither helper restarts workloads
or changes configuration. Build source identity with the Makefile variables
shown in the previous deployment report, substituting this report's revision,
describe and common timestamp.

Raw evidence outside Git:
`/Users/mike/Documents/Codex/2026-10-10/n-x20/artifacts/intel-lsm-index-20261014/`;
isolated native raw runs also under `/data1/efs/evidence/lsm-index-work` on each Intel
host. `artifact-sha256.json` covers source archive, timing records/summaries,
process inventories, manifests and smoke outputs. Useful SHA256s:

- Source-only release archive:
  `07e6db57d321f4d55565ad539226be5540949bde639e2fe674602c0089ac05a8`.
- Native measurement summary:
  `3e2727294e60002d8898627161632180fb760b8f1540ad4af54b58847aff385d`.
- Mounted measurement summary:
  `3e3f1df740f7e786bda4a7e9c3138ce169edaa39bac94b70e1ee352c937f1515`.

**Raw evidence note (2026-10-23):** the bulk data in `lsm-index-work` (copied stores, generated payloads and build trees) was deleted. Text logs, results and profiler captures remain under `/data1/efs/evidence/`, each with a `PRUNED.txt` listing what was removed. Reproduce the bulk with the method on this page.
