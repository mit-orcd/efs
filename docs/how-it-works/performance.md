# Performance — the multi-Raft runtime and the hot-path contract

[Architecture](architecture.md) · [Data protocol](protocols/data.md) ·
[Verification](verification.md)

The topology in [the spec](architecture.md) makes linear scaling
*possible*. This document is the contract that makes it *actual* — the
difference between "the architecture scales" and "the implementation
scales." Each item is a requirement, traceable to P1/P4, not a tuning
suggestion.

## The multi-Raft runtime

**4096 logical groups are not 4096 physical mini-databases.** A node must
never run per-group timers, WALs, fsyncs, sockets, heartbeats, or threads —
on a 3-node cluster that would be ~1365 leaders per node doing bookkeeping
instead of work. The implementation is a **multi-Raft runtime**: a few
reactor threads (per NUMA domain, P4) drive many shard state machines; Raft
messages are batched by destination, heartbeats coalesced, AppendEntries
pipelined, WAL writes group-committed across groups, and KV applies batched —
while each shard keeps its own independent logical ordering, terms, and
commit indices. This is the same logical/physical split as the applied-state
KV (§5 of the spec), and it is an architectural requirement, not an
optimization: without it the bookkeeping cost of 4096 groups consumes the
hardware the groups were meant to exploit.

## The hot-path implementation contract

- **NUMA-local, queue-depth-driven execution (P4).** The data target and the
  metadata replica both run as asynchronous engines: a NIC RX queue is
  affinitized to a reactor core on its NUMA node, which drives a local NVMe
  queue pair; registered buffers are NUMA-local; ordinary PUT/GET and
  ordinary metadata commits take **no cross-core lock** and make no
  thread-to-thread handoff (no network-thread → queue → worker-thread →
  queue → storage-thread relay). Whether the engine is literally SPDK or an
  equivalent `io_uring`/O_DIRECT design is an implementation decision; the
  architectural requirement is the absence of cross-core coordination on the
  common path.
- **QoS isolation between planes.** "Run at hardware saturation" and
  "metadata stays responsive" are mutually hostile without isolation. Raft,
  membership, transaction decisions, and small metadata RPCs get their own
  RDMA queues / traffic class / credits, separate from bulk data — not
  static bandwidth reservation, but *latency* isolation, so a saturated
  400G NIC cannot queue a Raft packet behind gigabytes of EC fragments. On
  NVMe: metadata WAL, KV reads/writes, bulk fragment I/O, and rebuild I/O
  have separate queues and backpressure policies.
- **Rebuild is distributed and rate-limited (P1 applies to repair too).** A
  lost object's repair is deterministically assigned to an owner, reads its
  k surviving fragments from distributed peers, and writes the replacement —
  spread across the whole cluster, never centralized. Scheduling priority is
  **foreground I/O > metadata > rebuild**: rebuild consumes *unused*
  hardware bandwidth, so a node failure must not collapse foreground
  throughput. (This is also where degraded generations from
  [the data protocol](protocols/data.md) are re-striped.)
- **FUSE capabilities and client prefetch** are part of this contract —
  specified in §7.7 of the spec because they are also correctness-relevant.
- **Read-side metadata is fetched in windows, not per chunk.** A sequential
  read at 100 GB/s touches ~800k chunks/s; discovering each chunk's
  committed generation with an independent metadata RPC would make the
  metadata plane the ceiling even with a perfect data path. The lane
  structure makes the fix cheap: the client issues **batched range
  chunk-map fetches per lane** (lane i holds chunks i, i+64, i+128, …, so one
  range request per lane covers a contiguous file window), and the prefetch
  pipeline runs a *metadata window* ahead of the *data window* — chunk maps
  arrive before the data they describe is needed. Per-chunk lookups remain
  only for true random I/O.
- **Batching** of Raft messages, WAL group commit, KV applies, chunk
  publications, and read-authority rounds is specified where it lives
  (the multi-Raft runtime above, and the
  [data](protocols/data.md) / read protocols in the spec) and is binding:
  **no persistence boundary is paid per chunk, per metadata record or per
  Raft group when several operations can safely share one; durability
  boundaries are amortized to the largest batch the externally visible
  semantics allow.** The stronger-sounding claim "no NVMe sync per logical
  operation" would be a lie: publication (`fsync`/`close`/`O_SYNC`)
  crosses a persistence boundary — an ordinary completed write on PLP
  media, or an explicit FUA/flush without it
  ([data protocol](protocols/data.md)). A plain `write()` does not.
  What batching removes is paying that boundary once per chunk when one
  boundary could have covered thousands; it cannot remove the boundary
  itself.

---

## Baselines and ceilings (current)

Moved here verbatim from the work queue ([../status/README.md](../status/README.md))
on Oct 3 2026; the queue links here instead of carrying the tables.

**What the hardware allows.** Every performance item is measured against
this, not against last week's number. Cluster traffic rides `ibs1f0`
(200 Gb/s IPoIB, `ip route get 172.16.223.57` on a client); servers have six
NVMe each.

| ceiling | value | derivation |
| --- | --- | --- |
| one client, logical write | **~16.7 GB/s** | 25 GB/s line rate ÷ 1.5 (2+1 EC sends 3 fragments per 2 data) |
| one client, logical read | ~25 GB/s | line rate; a read fetches k=2 fragments |
| cluster, logical write | **~44–57 GB/s** | 4 hosts × 16.7–21.4 GB/s NVMe (`results/nvme/`) ÷ 1.5 |
| 1-client honest write today (Oct 1) | ~1.3–1.5 GB/s | **~8–9 %** of the client's ceiling (8 GiB dd+fsync 1.3 GB/s; 16 GiB 1.5 GB/s) |
| 9-client honest write today (Sep 28) | 2551–2810 MiB/s aggregate | **~6 %** of 44 GB/s (8 GiB dd+fsync, 9 own files) |
| 1-client cold read today (Oct 1) | 3.6 GB/s; 4 readers 6.5 GB/s | ~14 % of the client's ceiling (16 GiB, remount before read) |

[architecture.md §1](architecture.md) says: if a benchmark stops at a
mutex, one leader, one thread, FUSE serialization, one WAL or one
coordinator before a physical resource, *that is by definition an EFS bug*.
By that rule the write path is still a bug, not a tuning task.

Baselines, all honest (flush in the clock, reads after remount, `findmnt`
verified `fuse.efs-fuse`). Historical context is in `docs/archive/project-history.md`; the table below
links the retained measurement evidence.

| measurement | value | where |
| --- | --- | --- |
| 1-client 8 GiB `dd bs=1M conv=fsync`, fcstor007 | **1.3 GB/s** (6.80 s) | §1b 13:20Z block (`dd539`), Oct 1 |
| 1-client 16 GiB write / cold read / 4 readers | **1.5 / 3.6 / 6.5 GB/s** | §1b 18:00Z block (`agent-rd-20261001-*`), Oct 1 |
| 1-client 8 GiB dd+fsync / cold read, fcstor007, Oct 2 (P0.1) | **1518 / 3297 MB/s** | `results/measure/20261002-054132-p0-x16/SUMMARY.txt`, `-053311-p0-gates/` |
| 1-client **16 × 10 GiB dd+fsync** aggregate, fcstor007, untraced (row 11) | **3815 MB/s** (every stream 44.7–45.0 s, no close tail) | `results/measure/20261002-054132-p0-x16/SUMMARY.txt` P0.2 (the traced 04:02Z 663 MB/s on fstor007 is not a baseline) |
| 1-client 8 GiB bs=1M / 4 GiB bs=64k dd+fsync, fcstor008, P1 tree (D23+W41) | **1678 / 737 MB/s** | `results/measure/20261002-060052-p1-d23-w41/SUMMARY.txt` §4 (v2–v5 spread 1363–1678 / 737–828) |
| 4 × 8 GiB dd + 300-file create/close storm, fcstor010, old → P1 tree | dd wall **9.70 → 8.30 s**; storm p50 4.9 → 3.8 ms, **p99 37 → 101 ms** | same SUMMARY §4 (W53) |
| 9-client 8 GiB dd+fsync, aggregate | **2551** MiB/s (best 2810) | `results/measure/20260928-134637-dd-prof-r5b`, `-131651-dd-prof-r2b` |
| per-host local NVMe ceiling | 16.7–21.4 GB/s | `results/nvme/` |
| `efsd --bench data`, efs1 dev VM (1 virtio disk, **not** an fcstor node) | write 680 frag/s QD16 ≈ 82 % of the 1-disk fio ceiling, disk 82–99 % util; read 12.6 k frag/s (0.77 GiB/s) QD256 | `results/measure/20261005-045140-p3-benches/SUMMARY.txt`, Oct 5 |
| `efsd --bench meta`, efs1 dev VM | KV put / Raft append QD1 = 27 ops/s p50 36 ms = the host's fsync ceiling (fio wsync4k 27 ops/s); Raft batch32 = 950 entries/s | same SUMMARY, Oct 5 |
| `efs-fuse --bench`, efs1 → dev cluster (TCP) | cpu 2.85 GiB/s (2 vCPU saturate) ≫ put 0.26 GiB/s stored QD1 (p50 458 µs) ≈ write 51–71 MiB/s — the RTT is everything on this host | same SUMMARY, Oct 5 |
| IO-500 9×4 debug, fresh table | easy-write **5.173 GiB/s**, hard-write 0.519, mdtest-easy-write 6.185 kIOPS, hard-read 1 error (W38) | `results/io500/20261001-074905-rdma/` |
| IO-500 9×4 debug, 0 errors | easy-write 4.563, hard-write 0.640, hard-read 1.034, cold hardscan bad=0 | `results/io500/20260930-183504-rdma/` |

**Never quote intra-job fio write samples or `dd` progress lines** — those are
pre-flush and read several GiB/s. The number is bytes ÷ wall with the flush
inside. `dd if=/dev/zero` is also invalid here: all-zero payloads skip PUTs.
Use a non-zero source file.

## Local storage benchmarks

Local engine benchmarks now belong to `efs-bench` (Oct 6 2026):

```sh
./efs-bench --bench data --storage /scratch/efs-bench --time 10 --writers 8
./efs-bench --bench meta --meta-storage /scratch/efs-meta --time 10
./efs-bench --bench data --help
```

Roots must be empty scratch directories; the tool refuses nonempty roots.
Data supports repeated/comma-separated storage roots, `--direct-io`, and `--perf`;
metadata also accepts the first `--storage` root. The implementation reuses the
production fragment store, writer pool, KV and Raft log, with no cluster startup
or network. Existing seed-based network/store/read/metadata benchmark commands
remain in `efs-bench`. The server's benchmark RPC handler remains necessary for
the remote network-discard measurement.

Historical measurements using `efsd --bench` retain their original
command labels. New runs use `efs-bench --bench`; `efsd` no longer offers local
benchmark execution. The move changes command ownership, not measured workload
or backend behavior. No new storage-performance claim follows from this move.

## Latency measurement contract

Engine data latency surrounds the complete `slot_put` / `slot_get` call using
`CLOCK_MONOTONIC`, rounded to integer microseconds. It includes admission,
writer scheduling, filesystem work, I/O and return scheduling. It is not device
latency or end-to-end FUSE latency. QD workers issue the next request only after
the previous one completes: this measures a closed-loop workload and does not
model queued arrivals during a stall (coordinated omission).

Every operation contributes to a fixed-size histogram, including failures.
There is no sample cap or allocation on the measurement path. `lat_samples`
must equal successes plus errors; invalid coverage fails the data result.
Percentiles use nearest rank; `p99_lower_us..p99_us` encloses that sample
percentile. Integer values through 255 us are exact; larger buckets have width
below 0.782% of their lower bound. `max_us` is the exact observed integer-us
maximum, independent of histogram buckets. Neither histogram resolution nor
sample count establishes statistical confidence or a worst-case guarantee.

`latency_quality=low_samples` flags fewer than 10,000 observations. For
comparisons use longer untraced runs and at least five randomized repeats,
inspect sample counts and per-run ranges, and keep perf/strace in separate
runs. A `sufficient_samples` label only passes this count heuristic; correlated
I/O observations and run-to-run variance still matter. Three-second harness
defaults are exploratory, not acceptance-quality tail measurements.

`BENCH_HIST version=1` retains mergeable `bucket:count` pairs after each data
round. Bins 0..255 are exact microseconds. For index `i >= 256`, let
`s = floor(i/128)-1`; bounds are `(128+i%128)*2^s` through that value plus
`2^s-1`, capped by the observed maximum. Combine counts from identical workloads
and use nearest rank on the merged counts for a pooled percentile. Do not
average per-run p99 values and call the result a pooled p99. Metadata operation
percentiles also use this collector; raw-I/O and hash modes expose different
metrics and do not claim an engine operation p99.

## Benchmark profiling harness

`./efs-bench.sh` on a Linux benchmark host runs an untraced baseline and a
separate perf rerun for each case, then writes `ANALYSIS.md`. Build with
`make efs-bench` first. The host needs `perf` and permission to record the chosen
event; missing tools or failed/empty profiles produce failure, never a silent
unprofiled success. The script does not install packages or change kernel policy.

```sh
# Full local matrix; use an existing scratch parent on the device of interest.
./efs-bench.sh --storage-root /data1/bench-scratch
# Repeat --storage-root to sweep 1..N paths on multiple devices.
./efs-bench.sh --storage-root /data1/bench-scratch --storage-root /data2/bench-scratch
# Add the cluster network, bounded PUT/GET and metadata modes.
./efs-bench.sh --storage-root /data1/bench-scratch --seed HOST:17432
# Optional syscall summary in a third, separate rerun for each case.
./efs-bench.sh --storage-root /data1/bench-scratch --strace
# Show the matrix without running tools or allocating scratch.
./efs-bench.sh --dry-run
```

Defaults now also include the raw I/O groups described below. BLAKE3 one-shot and streaming at 4 KiB, 64 KiB, 128 KiB and 1 MiB,
with 1/2/4/affinity-CPU-count workers (deduplicated and capped by affinity);
data buffered/direct I/O, inline/two-thread/automatic writer pools and QD
1/16/64/256; local KV and Raft workloads including individual and batch32
appends. The fixed data payload remains 64 KiB. The harness bounds the local resident
working set to 256 MiB of fragment payload across QD slots (`--data-size`);
filesystem/checksum overhead is additional. The full bounded window is now
populated **before** timing or perf recording; measured writes replace existing
fragments. `--write-mode overwrite` requires `--window`; `--write-mode create`
requires no window and times file creation. Omitting the mode infers it from
the window. Earlier bounded-write results mixed creation and replacement,
especially in short synchronous runs; do not treat them as pure overwrite data. `--time` defaults to three
seconds **per timed phase**, so the full baseline/profile matrix takes minutes,
plus setup, compaction/reopen and report generation. This is a bounded set of
meaningful configurations, not every combination of arbitrary numeric options.
`--threads`, `--hash-sizes`, `--writers` and `--qds` customize the ladders.
Data CPU profiles combine write/read phases and, with multiple roots, the
path-count ladder; metadata profiles combine their KV/Raft phases.

The default result directory is `logs/efs-bench-<UTC timestamp>`. Without
`--storage-root`, scratch is created there on that directory's filesystem.
Every baseline/perf/strace run gets fresh private children; only those children
are removed. Caller-provided parent directories and their contents are preserved.
`--output` selects a **new** directory; existing paths are refused. A single
untraced fio/raw ceiling probe precedes the first local engine case;
`--skip-ceiling` omits it. Profiles omit the ceiling probe. Host/device/mount
metadata is recorded to distinguish tmpfs, page cache and actual storage.

Each case saves commands, stdout/stderr, baseline metrics and `perf/perf.data`,
plus flat, per-thread (`pid` sort identifies thread IDs), caller and top-three
benchmark-symbol source/assembly annotations. The matching executable and
`sources.zip` are retained with a SHA256/version/commit manifest. Source lookup
by perf annotation still uses the build paths; the archive preserves source
for later review. `ANALYSIS.md` reports the best observed baseline configurations,
hot symbols and heuristic CPU categories. Sample shares measure CPU execution,
not time blocked on disk/network, and do not prove the bottleneck. Call chains,
latency and device ceilings must support an optimization choice.

Perf defaults to `cycles`, 499 Hz and frame-pointer call graphs. Select
`--event cpu-clock` on a VM lacking a PMU, or `--call-graph dwarf` when needed.
`--no-perf` is an explicit baseline-only diagnostic mode and is labelled as such.
Strace uses `-f -c -w` and saves per-syscall counts/wall time/errors; optionally restrict
it with `--strace-expr trace=writev,fsync,futex`. Traced timings do not replace
baseline throughput. The summary includes population and cleanup, so it is not
a timed-loop latency measurement. For phase-specific traces, engine data runs
can set `EFS_BENCH_PHASE_MARKERS=1` and use timestamped `strace -f -ttt -T`,
then restrict completed calls to the `BENCH_PHASE begin/end` interval.
`--timeout` bounds each subprocess. Timeout/interruption
stops the owned process group and retains completed evidence; regenerate the
summary with `./efs-bench.sh --analyze <result-directory>`.

Cluster modes require a compatible deployed benchmark binary/build ID and a
seed. They profile **the benchmark process**, not the remote daemons. A
fixed-size PUT primes the full GET window; timed PUTs wrap within that window.
The default working set is 256 MiB logical, configurable through `--store-size`.
It remains in the dedicated benchmark export after the run. Use a separate
`--chunk-base` to avoid another benchmark writer. Remote metadata runs all phases
and removes their created names normally; `--export`, `--files` and `--dirs`
control that workload. With no seed, remote modes are explicitly omitted.

BLAKE3 can also run directly:

```sh
./efs-bench --bench blake3 --oneshot --size 64K --threads 1 --time 3
./efs-bench --bench blake3 --stream --size 1M --threads 8 --time 3
```

The existing `make blake3-bench` standalone tool shares the implementation.
It reports selected SIMD implementation, actual affinity-constrained workers,
hashes/bytes and throughput. One-shot includes init/finalize per buffer;
streaming reuses a hasher. Both reuse warm per-worker buffers and measure CPU
throughput, not end-to-end storage or cold-memory bandwidth. Worker timing uses
atomic coordination after warm-up and includes completion of the final batch.

## Separating I/O from checksum cost

The default harness now compares three independent workloads:

- `--bench io`: raw parallel `pread`/`pwrite`, no timed BLAKE3.
- `--bench io-blake3`: identical files, sizes, worker counts and access pattern;
  hash each write before I/O, hash/verify each read after I/O, inside timing.
- `--bench blake3`: CPU-only one-shot/streaming hashing, no I/O.

```sh
./efs-bench.sh --modes io,io-blake3,blake3 --storage-root /data1/bench-scratch
./efs-bench --bench io --storage /scratch/io --rw write --io-size 64K --qd 16 --window 64 --time 3
./efs-bench --bench io-blake3 --storage /scratch/io --rw read --io-size 64K --qd 16 --window 64 --time 3
```

Raw I/O defaults sweep 4 KiB/64 KiB/1 MiB (`--io-sizes`), buffered/direct I/O,
read-only/write-only rounds, and the QD ladder. QD is the number of concurrent
synchronous workers, one file per worker. Roots distribute workers round-robin
across devices. The bounded per-worker window cycles offsets; no production
fragment format, metadata engine, writer pool or checksum-policy changes are
involved. Actual storage-engine `data` and `meta` modes remain in the default
matrix as separate measurements.

Buffers and read files are prepared before timing. Successful writes/reads
are validated byte-for-byte outside timing, including pure-I/O runs. Read-only
cases therefore have initialization writes but report only parallel read work.
For these raw drivers perf starts disabled and acknowledges enable/disable
commands around the timed loop; setup, read-file population and final integrity
checks do not contaminate CPU profiles or source annotations. Strace summaries
still describe the full process, including initialization and verification.

Buffered writes measure page-cache acceptance by default. Raw writes can include
`fdatasync` per operation through `--sync` (direct CLI) or `--io-sync` (harness).
End-of-run flush/verification is outside the reported default write rate.
Direct I/O bypasses the page cache on supporting filesystems; tmpfs does not
establish a physical disk ceiling. No global drop-caches operation is performed.

`efsd` has no local benchmark CLI or benchmark runner objects. Its discard-only
BENCH_PUT handler remains for the remote network benchmark. Cluster deployment
and client refresh now ship the profiling wrapper, Python helper and matching
C/header/assembly sources to gateways/test clients, excluding object files.
The NUC deploy already transfers the complete source tree. Existing cluster
`bench.sh` uses the still-supported remote metadata mode and needs no CLI change.

Raw I/O workers sleep at a condition-variable start gate until all are ready.
The timed loop uses integer deadlines, one clock read per completed operation,
and an increment/wrap offset rather than division. `avg_us`/`max_us` report the
complete operation cycle, including loop bookkeeping and scheduling. This
latency definition differs from older syscall-region measurements.
`idle_workers`, `min_worker_ops` and `max_worker_ops` expose participation;
any idle worker or I/O/verification error produces `BENCH_FAIL` and a nonzero
exit. Diagnostics retain the actual failing phase/error instead of stale errno.

Analysis excludes invalid baselines even if they contain rates. A valid baseline
remains usable when its separate perf rerun fails. Time/vDSO and unresolved
addresses are shown separately, with warnings for substantial unresolved sample
coverage. The manifest records CPU affinity and perf permission settings; reports
show the recorded event (which may differ from the requested event). No kernel
address is assigned to a guessed function.

## Generating perf reports

Measurements run sequentially. After all workloads finish, the harness generates
reports and source annotations with up to four parallel case jobs by default.
`--report-jobs N` controls the limit; each job runs one perf subprocess at a time.
This keeps reporting CPU/memory pressure out of subsequent measurements.
Progress prints `[reports n/total]` with each case's result. Cases remain
REPORTS_PENDING until their reports finish; failed reports fail the overall run.

Each `perf/` directory includes `flat.txt`, `by_thread.txt` and `callers.txt`,
plus the original `.stdout`, `.stderr` and `.command.json` evidence and a
semicolon-delimited `symbols.stdout` for analysis. The flat and caller options
match the standard perf report forms; the thread report sorts `comm,pid,symbol`
to distinguish worker thread IDs. Reports filter `--comms efs-bench` to exclude
helper processes. Failed profiled workloads with captured data also get reports,
marked as diagnostic evidence rather than representative measurements.

Regenerate reports without running any workload, using the original recording
user on the Linux host (ownership/access is checked before starting):

```sh
./efs-bench.sh --reports-only logs/efs-bench-20261006T230611.472138Z --report-jobs 4
```

`--analyze DIR` only rebuilds Markdown from existing reports. It does not run
perf report. Older runs keep working with their `.stdout` report names.
Kernel copy and page-pinning/IOMMU symbols now have explicit heuristic categories;
use the raw symbols and callers to assess a proposed optimization.

## Allocation cost and multiple storage locations

Raw writes now default to paired allocating and preallocated-overwrite cases.
`--io-write-layouts allocating|preallocated|both` selects the comparison. The
preallocated case uses Linux `posix_fallocate`, writes every block with the
expected nonzero pattern and flushes it before timing/perf enable. An unwritten
allocated extent alone is insufficient: its first write still converts extents.
There is no silent fallback on unsupported allocation. `allocation` and
`prepared_blocks` identify the workload; setup and end-of-run verification/flush
remain outside default timing. Use `--io-sync` for per-operation durability.
Allocating writes retain their initial allocation work inside timing.

Repeat `--storage-root` for distinct locations. The harness runs each root alone
and then all roots together for raw I/O and engine data; combined raw cases need
at least one worker per root, so QD1 is measured only on the individual roots.
Total QD and working-set bytes remain fixed across geometries. Raw workers use
round-robin roots. Engine data uses the production fragment-store/writer routing
and its existing 1..N prefix ladder, exposing the extra routing/lookup cost as
locations are added. Metadata has one KV location, so it runs on each root alone.
CPU-only hashing is not duplicated per storage geometry.

The manifest records resolved paths, filesystem device IDs and free space;
findmnt output is retained per root. Symlink aliases of the same directory are
rejected. Multiple directories on one filesystem trigger a warning; use
`--require-distinct-devices` to require at least two distinct filesystem devices.
Different filesystem IDs still do not prove independent physical media (LVM,
RAID and shared controllers can couple them); inspect mount/device provenance.
Analysis separates device groups and allocation layouts instead of pooling their
best rates. Prefix-ladder data profiles combine their write/read/path phases.

For example, create two writable scratch parents on the intended devices and run:

```sh
./efs-bench.sh --storage-root /data1/efs/bench-scratch --storage-root /data2/efs/bench-scratch --require-distinct-devices
```

The default includes raw I/O, I/O+BLAKE3, CPU BLAKE3, engine data and metadata,
with sequential measurements and parallel report generation. Two roots expand
the default to 502 cases, so allow for setup, flushing and reports. A shorter
first pass keeps both allocation layouts and device comparisons:

```sh
./efs-bench.sh --modes io,io-blake3,data,meta --io-sizes 64K --qds 1,16 --writers 0,auto --storage-root /data1/efs/bench-scratch --storage-root /data2/efs/bench-scratch --require-distinct-devices
```

These are local storage benchmarks; they neither mount exports nor restart a
cluster. Measure the engine's store/writer/KV hotspots before carrying a raw-I/O
experiment into production. Registered-buffer/asynchronous I/O remains a separate
experiment whose benefit must be measured against these ceilings.

[Implementation and functional validation](../../results/measure/20261006-bench-allocation-multiroot/SUMMARY.md).

### October 7 benchmark review

Raw I/O workers wait on private release gates after a shared readiness barrier.
This avoids a shared-mutex release convoy at QD256. Results retain the idle-worker
failure gate and report `max_start_us`, the largest release-to-work delay.
Initially empty windows report `allocation=allocate_then_overwrite`, with
`allocation_ops` and `overwrite_ops`; most long buffered runs reuse warm pages.
Their throughput is page-cache acceptance, not a sustained physical-media ceiling.

The profiler defaults to `--call-graph auto`: DWARF for engine data, frame pointers
for other modes. DWARF recordings are larger; explicit `fp` or `dwarf` overrides
remain available. Old engine-data recordings contained impossible caller
addresses from unwinding through libc. Analysis flags noncanonical addresses when
CPU virtual-address width is recorded. Flat self-samples remain useful; old call
chains cannot be repaired, and deep DWARF stacks can still truncate. Failed perf
runs and incomplete reports are labeled invalid and retained only as diagnostics.

Metadata measurements validate returned KV values, flush/compact/stat status,
and recovered Raft index/term. Errors accumulate across phases, failed batches
are ended and invalidate the run, and failed or zero-operation phases print
`BENCH_FAIL`. No durability policy was relaxed to improve rates.

[Review, matched measurements and validation](../../results/measure/20261007-bench-hot-path-review/SUMMARY.md).

### Isolated writer investigation

Use `--data-rw split` to create separate write and read profiles. Read cases
populate the bounded window before timing; both cases gate perf sampling around
only their measured phase. `--data-full-paths` measures the selected roots together
without repeating the prefix ladder. The engine workers use private release gates
and report idle workers, release delay, actual writer count and per-root write
counts. Bounded overwrites probe their existing root instead of claiming every
operation creates a new fragment.

For example, repeat this comparison three times on a quiet host:

```sh
./efs-bench.sh --modes data --data-rw split --data-full-paths --writers 0,2,auto --qds 1,16,64 --data-size 64M --time 3 --skip-ceiling --storage-root /data1/efs/bench --storage-root /home/efs/additional-work-dir/bench
```

Use `--writer-stats` for separate diagnostic runs. It enables production writer
instrumentation only for the measured writes and prints `BENCH_WAIT` totals and
maxima in microseconds: admission (submission to an accepted slot, including
path selection and waiting for an empty slot), queue (accepted slot to service),
service (the complete synchronous writer job), and resume (service completion
to the caller returning). `lock_us` covers initial fallback-slot mutex acquisition;
it is not a total for all locks or condition-variable waits. It overlaps admission.
`fallback` counts jobs that entered the blocking slot path; `peak_active` counts
submissions in flight, including those waiting for a slot, rather than disk queue
occupancy. Divide totals by `jobs` for per-submission averages. Maxima are separate
observations and must not be added together. CPU profiles and optional short
strace/scheduler recordings complement these counters.

Uninstrumented repeated runs establish throughput and tail latency; instrumented
or traced runs identify waiting and must not be substituted for those baselines.
Writer statistics are disabled by default in the daemon. The routing fix also
initializes storage-root selection in inline mode, so new inline writes can use
all configured roots; overwrites retain their existing root. No writer-count or
sync/durability default changed.


Completed xorinox measurements and acceptance are recorded in the
[writer investigation](../../results/measure/20261007-writer-investigation/SUMMARY.md).
Buffered shard overwrites preserve extents instead of truncating before writing;
a successful write checks exact body/checksum length and truncates an old longer
tail. Direct I/O and persistence barriers are unchanged. The matched warm bounded
overwrite gain is about 4–8×; it is not physical-media or FUSE throughput.
A single-waiter signal experiment caused starvation and was rejected. Fair
admission needs reserved handoff and anti-bypass protection, including worst-wait
and shutdown gates. Writer-count defaults remain unchanged.


For synchronous engine-write diagnostics use `efs-bench --bench data --sync`
or `efs-bench.sh --data-sync`. This benchmark-only option opens fragment files
with O_SYNC; buffered writes also sync final length handling. Body/checksum
writes can therefore incur multiple persistence waits per PUT. The daemon's
build and defaults do not enable it. Compare untraced repeats before separate
perf/strace runs; O_SYNC does not establish SSD power-loss guarantees. See the
[synchronous writer measurements](../../results/measure/20261007-sync-writer-investigation/SUMMARY.md).
