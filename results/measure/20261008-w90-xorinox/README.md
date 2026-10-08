# W90: independent storage churn on xorinox

Implemented on `devel-bench`, based on `53be1da7f190`. Production EFS code, services and global XFS options were not changed. The shared `devel` checkout remains separate.

Local worktree: `/Users/mike/Documents/Codex/2026-10-07/do-x20/work/devel-bench`. Remote source snapshot: `/data1/efs/bench/w90-20261008/src`.

## Host and workload

XFS on `/dev/nvme1n1`, HP SSD FX900 Pro 4 TB; 4 KiB blocks, 16 allocation groups, `noatime,logbufs=8,logbsize=256k`. About 3.3 TiB free before testing. Scratch is under `/data1/efs/bench/w90-20261008/`. Other host services were running.

Files, fixed-slot punch/reuse containers, and append/survivor-compaction containers use nonzero deterministic payloads and an identical aligned 4 KiB BLAKE3 tail. Containers are 1 GiB logical files; each worker has 2 GiB data-slot capacity and one extra reserved cleaning container. Percentage occupancy refers to those slots, not filesystem fullness. Segment rounding can change live counts by a record compared with files.

128 KiB cases use 24,000 measured operations, 1,000 warmup operations, a deterministic 50/50 read/replacement mix, one blocking worker, and batch size 32. 64 KiB cases use 48,000 operations at 80%/95% occupancy. Reads and final live objects are independently verified. Batch barriers are timed, but the volatile index is not crash recoverable. Replacement files require separate body flushes, while containers can batch their shared body flush. Buffered reads can hit cache.

## Results

| Suite | Payload | Occupancy | I/O | Layout | Ops/s | Read p99 ms | Write p99 ms | Write max ms | Copy/new bytes | Cleanings | Allocated after GiB |
|---|---:|---:|---|---|---:|---:|---:|---:|---:|---:|---:|
| fragment-64k | 64 KiB | 80% | direct | files | 716 | 0.279 | 1.703 | 557.1 | 0.00 | 0 | 1.600 |
| fragment-64k | 64 KiB | 80% | direct | punch | 1948 | 0.243 | 1.599 | 575.8 | 0.00 | 0 | 1.600 |
| fragment-64k | 64 KiB | 80% | direct | compact | 2315 | 0.252 | 0.571 | 2706.8 | 2.39 | 5 | 1.881 |
| fragment-64k | 64 KiB | 95% | direct | files | 534 | 0.281 | 2.911 | 578.5 | 0.00 | 0 | 1.900 |
| fragment-64k | 64 KiB | 95% | direct | punch | 620 | 0.257 | 1.727 | 987.0 | 0.00 | 0 | 1.900 |
| fragment-64k | 64 KiB | 95% | direct | compact | 580 | 0.259 | 0.599 | 3312.7 | 13.88 | 23 | 1.955 |
| matrix-128k | 128 KiB | 50% | direct | files | 1034 | 0.291 | 1.599 | 396.7 | 0.00 | 0 | 1.000 |
| matrix-128k | 128 KiB | 50% | direct | punch | 2333 | 0.287 | 1.207 | 395.3 | 0.00 | 0 | 1.000 |
| matrix-128k | 128 KiB | 50% | direct | compact | 3692 | 0.311 | 0.707 | 920.3 | 0.24 | 1 | 1.925 |
| matrix-128k | 128 KiB | 50% | buffered | files | 1327 | 0.070 | 1.999 | 573.2 | 0.00 | 0 | 1.000 |
| matrix-128k | 128 KiB | 50% | buffered | punch | 2448 | 0.042 | 2.207 | 621.4 | 0.00 | 0 | 1.000 |
| matrix-128k | 128 KiB | 50% | buffered | compact | 5950 | 0.049 | 1.431 | 794.1 | 0.24 | 1 | 1.925 |
| matrix-128k | 128 KiB | 80% | direct | files | 1011 | 0.297 | 1.647 | 554.6 | 0.00 | 0 | 1.600 |
| matrix-128k | 128 KiB | 80% | direct | punch | 2147 | 0.289 | 1.527 | 385.8 | 0.00 | 0 | 1.600 |
| matrix-128k | 128 KiB | 80% | direct | compact | 1720 | 0.313 | 0.699 | 1765.6 | 2.47 | 5 | 1.859 |
| matrix-128k | 128 KiB | 80% | buffered | files | 1563 | 0.061 | 2.023 | 548.7 | 0.00 | 0 | 1.600 |
| matrix-128k | 128 KiB | 80% | buffered | punch | 2564 | 0.039 | 2.463 | 402.0 | 0.00 | 0 | 1.600 |
| matrix-128k | 128 KiB | 80% | buffered | compact | 3614 | 0.047 | 1.439 | 1504.1 | 2.47 | 5 | 1.859 |
| matrix-128k | 128 KiB | 95% | direct | files | 1119 | 0.301 | 1.679 | 572.5 | 0.00 | 0 | 1.900 |
| matrix-128k | 128 KiB | 95% | direct | punch | 1523 | 0.289 | 1.239 | 577.9 | 0.00 | 0 | 1.900 |
| matrix-128k | 128 KiB | 95% | direct | compact | 430 | 0.313 | 0.735 | 2240.8 | 14.34 | 23 | 1.933 |
| matrix-128k | 128 KiB | 95% | buffered | files | 1537 | 0.066 | 2.023 | 397.6 | 0.00 | 0 | 1.900 |
| matrix-128k | 128 KiB | 95% | buffered | punch | 2640 | 0.041 | 2.751 | 386.0 | 0.00 | 0 | 1.900 |
| matrix-128k | 128 KiB | 95% | buffered | compact | 1392 | 0.039 | 1.455 | 1771.7 | 14.34 | 23 | 1.933 |
| repeat-95 | 128 KiB | 95% | direct | files | 984 | 0.309 | 1.711 | 398.6 | 0.00 | 0 | 1.900 |
| repeat-95 | 128 KiB | 95% | direct | punch | 1645 | 0.285 | 1.231 | 552.5 | 0.00 | 0 | 1.900 |
| repeat-95 | 128 KiB | 95% | direct | compact | 432 | 0.307 | 0.735 | 2229.3 | 14.34 | 23 | 1.933 |
| xfs-confirm-baseline-2 | 128 KiB | 95% | direct | punch | 1353 | 0.295 | 1.255 | 613.1 | 0.00 | 0 | 1.900 |
| xfs-confirm-baseline-3 | 128 KiB | 95% | direct | punch | 1513 | 0.291 | 1.215 | 582.8 | 0.00 | 0 | 1.900 |
| xfs-confirm-filestreams-2 | 128 KiB | 95% | direct | punch | 2089 | 0.289 | 1.727 | 562.9 | 0.00 | 0 | 1.900 |
| xfs-confirm-filestreams-3 | 128 KiB | 95% | direct | punch | 2349 | 0.291 | 1.703 | 400.8 | 0.00 | 0 | 1.900 |
| xfs-filestreams | 128 KiB | 95% | direct | punch | 2482 | 0.297 | 1.719 | 388.6 | 0.00 | 0 | 1.900 |
| xfs-hint-1m | 128 KiB | 95% | direct | punch | 1911 | 0.287 | 1.183 | 400.9 | 0.00 | 0 | 1.900 |
| xfs-hint-8m | 128 KiB | 95% | direct | punch | 1933 | 0.287 | 1.199 | 368.3 | 0.00 | 0 | 1.900 |
| xfs-preallocate | 128 KiB | 95% | direct | punch | 1773 | 0.293 | 1.223 | 380.7 | 0.00 | 0 | 2.900 |

All 35 recorded performance cases passed with zero errors and all final live objects verified. Raw case commands, stdout/stderr, executable/source identities and mount facts are retained in the suite directories. `sources.zip` matches every baseline manifest source hash. Executables remain in the remote result directories; they are omitted from this repository evidence copy.

## Validation

Linux builds completed without compiler warnings. New integrity/accounting tests pass: 54 layout/occupancy/I/O/persistence combinations, two-worker preallocation, nonempty-root and capacity protection, injected fsync failure and corrupted reads. Matrix configuration tests pass. Existing benchmark CLI and storage-principles regression tests pass; raw validation logs are in `validation/`. This is targeted benchmark validation, not a full production test suite.

## Interpretation and remaining work

For 128 KiB records at 95% occupancy, the first two direct-I/O runs placed hole-punch containers ahead of files; append compaction was much slower and copied about 14.34 survivor bytes per new record byte. Its worst foreground write exceeded two seconds. Rare cleaning pauses are visible in max latency even when p99 is low. At 50%, append compaction was fastest in the short baseline, which contained only one cleaning pass.

Filestreams produced 2,089–2,482 ops/s in three 128 KiB/95% punch runs, versus 1,353–1,645 across four baseline runs. Write p99 increased to roughly 1.70–1.73 ms from 1.22–1.26 ms. This is a promising throughput/tail tradeoff for further testing, not a production default. Preallocation used about 2.9 GiB including its reserved spare, versus 1.9 GiB for the sparse punch fixture. The 1 MiB/8 MiB hint results are single samples.

For 64 KiB/95%, the single runs measured files 534 ops/s, punch 620 and compaction 580; the layout gap was much smaller. Compaction still copied 13.88 survivor bytes per new record byte. This prevents treating logical-chunk results as fragment results.

These bounded runs establish functional behavior and preliminary layout evidence. They do not establish sustained endurance, NAND amplification, a durable index, distributed ownership/recovery, or production EFS GC correctness. Gross released blocks are not net free space or TRIM. Longer randomized/repeated runs, larger models, mixed lifetime distributions and more workers remain useful acceptance work. W90 stays open for repeated sustained acceptance.

## XFS opportunities

| Opportunity | Status / qualification |
|---|---|
| 1 GiB containers vs 128 KiB files | Tested all three layouts at 50/80/95%; append compaction has substantial high-occupancy survivor-copy cost. |
| Explicit preallocation | Tested container-only `--preallocate`; reserves unwritten extents without application zero-fill. |
| 1–8 MiB extent hints | Tested per-file hints separately; check allocation overhead/waste and repeat before choosing a default. |
| Hole punching | Tested aligned dead-slot punching and reuse; coalesced larger ranges remain a follow-up. |
| Filestream allocator | Tested private worker directories and repeated against the baseline; no global mount change. |
| Batched directory sync | Implemented bounded batches; file bodies still require individual flushes; no durable map is modeled. |
| Internal noatime | Already enabled on xorinox. |
| Journal buffers / size | Already eight buffers and 256 KiB, the documented maximum size. |
| Reflink | Follow-up for actual cloning workloads; shared extents introduce later COW. |
| Periodic TRIM | Separate host/device investigation; not run in this benchmark. |
| External log / realtime device | Requires provisioning and recovery testing; deferred. |

XFS option semantics: [kernel documentation](https://www.kernel.org/doc/html/latest/admin-guide/xfs.html) and [xfs_io manual](https://man7.org/linux/man-pages/man8/xfs_io.8.html). A 1 GiB logical file is not guaranteed to be one contiguous physical extent. Avoid blanket large `allocsize` overrides.

## Invoke on xorinox

```sh
ssh xorinox_efs
cd /data1/efs/bench/w90-20261008/src
./efs-bench.sh --storage-churn \
  --storage-root /data1/efs/bench/w90-20261008/scratch \
  --output /data1/efs/bench/my-churn-UNIQUE \
  --payload-sizes 131072 --steps 24000
```

Use a new output name. `--dry-run` prints the matrix without writing fixtures. Normal errors clean test-owned files; timeouts may retain partial private fixtures. The host/device and file-per-record model are independent of a mounted EFS export.
