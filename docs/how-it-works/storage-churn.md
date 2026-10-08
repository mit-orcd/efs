# Independent storage churn — W90

[Performance guide](performance.md) · [W90 scope](../backlog/work-items.md#w90) ·
[Storage/XFS experiment plan](../backlog/storage-engine-v2.md)

`efs-bench --bench churn` is a storage laboratory, with a volatile object index.
It does not call the production store, connect to EFS, use distributed GC
ownership, or provide crash recovery. Production integration remains separate.

## User invocation

Build on the Linux host where the executable runs (`-march=native`). Select a
scratch parent outside all live fragment and Raft roots. The runner creates fresh
private case directories and removes them only after successful verification.
Failed cases retain their logs and scratch root for diagnosis. Normal failures
clean files owned by the test; killed or timed-out processes can leave partial
fixtures in that private root.

```sh
make efs-bench
./efs-bench.sh --storage-churn --storage-root /data1/efs/bench --dry-run
./efs-bench.sh --storage-churn --storage-root /data1/efs/bench \
  --output /data1/efs/bench/churn-results-UNIQUE \
  --payload-sizes 131072 --io-modes direct,buffered --steps 24000
# 64 KiB models an EFS fragment; 128 KiB models a logical chunk-sized record.
./efs-bench.sh --storage-churn --storage-root /data1/efs/bench \
  --payload-sizes 65536 --io-modes direct --qds 1,2 --repeat 2
# Keep the file-per-record reference; apply these knobs only to containers.
./efs-bench.sh --storage-churn --storage-root /data1/efs/bench \
  --preallocate --extent-hint 1048576 --io-modes direct
```

The default matrix selects `files`, `punch`, `compact`, 50/80/95% experiment
occupancy, both I/O modes, one blocking worker and batched persistence.
`--container-bytes` defaults to 1 GiB. `--capacity-per-worker` defaults to 2 GiB
of data-slot capacity; containers have one additional reserved 1 GiB cleaning
file per worker. At least two data containers are required. Files use the same
payload-plus-4-KiB-checksum-tail record geometry. Each container has a fixed
logical length; it can remain sparse or be explicitly preallocated. One big
file is not automatically one physically contiguous extent.

The occupancy percentage applies to the model's bounded data slots, **not host
filesystem fullness**. Container-tail rounding makes effective data capacity
slightly lower than the nominal capacity. Files round byte capacity to whole
records; containers additionally round each segment, so live counts can differ
by a record at the same percentage. Higher QD means more blocking workers
with separate models and proportionately larger fixtures, not multiple queued
asynchronous I/Os per worker. The program checks substantial filesystem free
headroom before creating the bounded fixture.

## Measured workload and validity

Each worker starts with a deterministically populated live set. Warmup is
outside the measured interval. Reads verify identity, generation, stored digest
and independently retained expected digest. Replacements allocate a fresh
location, write a nonzero record, switch the volatile map, and retire the old
location. The live-object count stays constant; every final live object is
verified and accounted for.

- `files` creates individual records and unlinks retired files.
- `punch` releases aligned retired ranges and reuses empty slots.
- `compact` never overwrites a retired append slot. When append space is full,
  it selects the least-live segment, verifies and copies survivors to the
  reserved segment, then releases/recreates the victim. Reclamation is bounded
  and synchronous within the worker; other workers can continue independently.

Foreground write latency includes required reclamation. Container cleaning has
its own latency, copy-byte, cleaning-count and dead-record backlog fields.
Rare cleaning pauses can fall outside foreground p99; inspect write max and
cleaning latency as well as percentiles.
`none` is unsynced acceptance; final allocation measurement flushes outside its
clock. `each` and `batch` include body and parent-directory persistence barriers
in measured wall time. Each replacement file still needs its own body fsync;
the matrix driver defers those body flushes to the batch boundary by default,
then fsyncs every still-live pending body before the directory barrier. This
allows the filesystem/device to coalesce work without omitting required flushes.
Use `--file-immediate-sync` on the matrix driver for the earlier immediate-body
reference policy. Raw `--bench churn` requires `--file-batch-sync` to select
deferred file-body batching; it is valid only for files with `--persist batch`. The final
batch barrier is in wall time. Batch intermediate operations are not individually
durable completions. No durable map/checkpoint is modeled, so neither mode proves
power-loss durability of a complete storage engine.

`--seconds 120` selects a timed run per worker; the matrix raises the default
operation cap to ten million. `--steps` remains a hard cap: reaching it before
the requested duration invalidates the run. Required final barriers are timed.
Initialization runs in parallel across workers. Initial file bodies are flushed
as a complete live set after warmup, before measurement; measured replacement
flushes retain their selected persistence policy.

For scaling, keep in mind that capacity is per worker: eight workers mean
16 GiB of data-slot capacity and eight reserved 1 GiB cleaning containers with
the default geometry. This is not a fixed-working-set scaling experiment.
`extents_after` counts allocated FIEMAP mappings after the final flush, outside
timing. `extents_available=0` reports filesystems without FIEMAP support; it does
not imply zero fragmentation. Extent counts are filesystem mappings, not NAND
placement. Normalize them by allocation and consider different inode counts.

The fixed operation budget bounds work. Default work is six operations per
capacity slot at a 50/50 read/write mix. `--steps` overrides it; short runs can
finish before container cleaning occurs. Inspect `cleanings` before interpreting
GC comparisons; a first-pass success is not a steady-state or endurance result.
No-progress/allocation/read/write/sync failures invalidate a run. Zero read or
write work also invalidates it. Timed-out fixtures remain private and retained.

## Evidence and interpretation

Each result contains exact commands, stdout/stderr, result fields, device/mount
information, source and executable SHA-256 identities, and a preserved binary.
`SUMMARY.md` updates after each case. Compare repeated runs on the same device
and inspect errors and verified counts before comparing latency or throughput.

- Buffered reads may hit the page cache. This driver does not drop global caches.
- `allocated_before/after` use `st_blocks`; directory/journal cost is excluded.
  `released_allocated_bytes` is gross block release across retirements, not net
  free capacity or SSD TRIM/NAND erasure. Preallocation can reserve released space
  again immediately. Copy bytes include the checksum tail.
- `dead_records` measures append segments' remaining retired slots, with bounded
  per-worker peak backlog. It is not an EFS GC queue or a global oldest age.
- `model_bytes` estimates explicit worker/model storage; `rss_kib` is process
  high-water RSS including setup. Filesystem and kernel caches are outside it.
- The source snapshot can have unknown Git version metadata when transferred
  without `.git`; the manifest's source hashes and preserved executable identify
  the actual experiment. Host activity is not isolated from unrelated workloads.

Per-file extent hints and directory filestream flags apply only to private
benchmark files/directories. Unsupported settings fail, rather than quietly
pretending the requested variation ran. No mount option or XFS filesystem
provisioning is changed by this experiment.

The initial [xorinox evidence](../../results/measure/20261008-w90-xorinox/README.md)
records 35 passing performance cases and their limits. The follow-up
[scaling and shared-range ledger](../../results/measure/20261008-w90-scaling-ranges/README.md)
compares longer 64 KiB workloads and explicitly distinguishes immediate versus
deferred file-body flush scheduling. Broader lifetime distributions and
production recovery acceptance remain open.

For concurrent access to one container, see the separate
[shared-range experiments](storage-ranges.md). Churn workers own separate models
and containers; increasing their count does not itself test overlapping ranges.
